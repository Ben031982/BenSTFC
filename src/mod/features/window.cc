// The game window on Windows: [graphics] free_resize, the -ccm profile in the title bar, [graphics] allow_cursor,
// [graphics] remember_window, [graphics] borderless_fullscreen with its F11 toggle, [graphics] background_fps, and the
// monitor's DPI scale.

#include "features/features.h"

#if _WIN32
#include "config.h"
#include "features/window_placement.h"
#include "file.h"
#include "shortcuts.h"

#include "game/app.h"

#include <il2cpp/hook.h>
#include <il2cpp/il2cpp.h>
#include <spdlog/spdlog.h>
#include <unity/core.h>
#include <unity/input.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

#include <Windows.h>

namespace
{
// The first visible, unowned top-level window of this process. Found once it exists, then cached.
HWND GameWindow()
{
  static HWND window = nullptr;
  if (window)
    return window;

  const auto process = GetCurrentProcessId();
  for (auto hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetNextWindow(hwnd, GW_HWNDNEXT)) {
    DWORD owner_process = 0;
    GetWindowThreadProcessId(hwnd, &owner_process);
    if (owner_process == process && !GetWindow(hwnd, GW_OWNER) && IsWindowVisible(hwnd)) {
      window = hwnd;
      break;
    }
  }
  return window;
}

// With several clients running, "-ccm alt.toml" shows up as "[alt] Star Trek Fleet Command".
bool ApplyProfileTitle()
{
  auto* window = GameWindow();
  if (!window)
    return false;

  const auto length = GetWindowTextLengthW(window);
  if (length == 0)
    return false;
  std::wstring title(static_cast<size_t>(length) + 1, L'\0');
  title.resize(GetWindowTextW(window, title.data(), length + 1));

  const auto&  profile = File::Profile();
  const auto   bytes   = static_cast<int>(profile.size());
  std::wstring prefix(MultiByteToWideChar(CP_UTF8, 0, profile.data(), bytes, nullptr, 0), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, profile.data(), bytes, prefix.data(), static_cast<int>(prefix.size()));
  return SetWindowTextW(window, (L"[" + prefix + L"] " + title).c_str()) != 0;
}

// With free_resize the original is skipped: the game's per-frame aspect-ratio enforcement is what would snap the
// window back.
//
// TODO: the old hook also stretched a 1024x768 fullscreen fallback to the monitor, but only once a window handle
// captured by a SetWindowLongW detour was set, and that detour was never installed. Bring it back with a tested
// trigger if the fallback is still seen.
void AspectRatioConstraintHandler_Update_Hook(auto original, il2cpp::Object* _this)
{
  static bool title_pending = !File::Profile().empty();
  if (title_pending && ApplyProfileTitle())
    title_pending = false;
  if (!Config::Get().graphics.free_resize)
    original(_this);
}

// allow_cursor = false: the plain arrow, never clipped to the window. SetCursor_Injected is static; the trailing
// MethodInfo is passed through unchanged. The UI scale hook applies the same cursor when canvases rescale, since the
// game sets its own cursor again then.
void Cursor_SetCursor_Injected_Hook(auto original, intptr_t texture, const unity::Vector2* hotspot, int32_t mode,
                                    const MethodInfo* method)
{
  if (Config::Get().graphics.allow_cursor)
    return original(texture, hotspot, mode, method);
  SetCursor(LoadCursor(nullptr, IDC_ARROW));
  ClipCursor(nullptr);
}

intptr_t AspectRatioConstraintHandler_WndProc_Hook(auto original, HWND window, uint32_t message, intptr_t w_param,
                                                   intptr_t l_param)
{
  // Skip the game's aspect-ratio clamp and hand the message straight to Unity. The message arrives in the form of
  // the window's current procedure (the game's, installed through its own P/Invoke), and the A and W calls each
  // convert from their own form; the wrong one garbles non-ASCII text such as chat input.
  if (Config::Get().graphics.free_resize) {
    const auto forward = IsWindowUnicode(window) ? CallWindowProcW : CallWindowProcA;
    return forward(reinterpret_cast<WNDPROC>(AspectRatioConstraintHandler::unityWndProc()), window, message, w_param,
                   l_param);
  }
  return original(window, message, w_param, l_param);
}

bool InstallFreeResize()
{
  il2cpp::HookSet hooks("FreeResize");
  IL2CPP_STAGE(hooks, AspectRatioConstraintHandler::Class::Method("Update"), AspectRatioConstraintHandler_Update_Hook);
  IL2CPP_STAGE(hooks, AspectRatioConstraintHandler::Class::Method("WndProc"),
               AspectRatioConstraintHandler_WndProc_Hook);
  return hooks.Commit();
}

bool InstallCursor()
{
  il2cpp::HookSet hooks("Cursor");
  IL2CPP_STAGE(hooks, unity::Cursor::Method("SetCursor_Injected"), Cursor_SetCursor_Injected_Hook);
  return hooks.Commit();
}

// --- Window memory ---

// [graphics] remember_window: the window's rectangle, and whether it was maximized, go to a file named like the vars
// file and come back once when the next session's window appears. Only a window with a title bar takes part, so
// the game's own full screen modes stay the game's; borderless full screen from F11 is saved as the window it returns
// to, with the borderless flag. Nothing is saved before that restore, so the game's own first size never overwrites
// the saved one.
bool                                       window_memory_restored = false;
std::optional<window_placement::Placement> window_memory_saved;

// Borderless full screen from F11, below.
bool borderless_active = false;
bool EnterBorderless(HWND window, const window_placement::Placement& from);

std::filesystem::path WindowMemoryFile()
{
  auto name = File::Vars();
  if (const auto dot = name.rfind('.'); dot != std::string::npos)
    name.resize(dot);
  return File::Path(name + ".window");
}

bool HasTitleBar(HWND window)
{ return (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) == WS_CAPTION; }

// A maximized window is saved by the rectangle it restores to, so it comes back maximized and still shrinks to that
// size. Any other window is saved where it is on screen: a window snapped to half the screen reports the size it had
// before the snap as its normal rectangle, which would bring it back nearly full screen.
std::optional<window_placement::Placement> CurrentWindowPlacement(HWND window)
{
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  if (!GetWindowPlacement(window, &placement))
    return std::nullopt;
  const bool maximized = placement.showCmd == SW_SHOWMAXIMIZED;
  RECT       rect      = placement.rcNormalPosition;
  if (!maximized && !GetWindowRect(window, &rect))
    return std::nullopt;
  return window_placement::Placement{
      .left      = static_cast<int32_t>(rect.left),
      .top       = static_cast<int32_t>(rect.top),
      .right     = static_cast<int32_t>(rect.right),
      .bottom    = static_cast<int32_t>(rect.bottom),
      .maximized = maximized,
  };
}

// Puts the window at `placement`. A maximized one, or one on no monitor (one since unplugged), goes through
// SetWindowPlacement, which maximizes on the monitor of the normal rectangle and moves a window that is off every
// monitor back onto one. Any other is set where it was on screen, so a window snapped to half the screen comes back
// snapped.
bool ApplyPlacement(HWND window, const window_placement::Placement& placement)
{
  const RECT rect{placement.left, placement.top, placement.right, placement.bottom};
  if (placement.maximized || !MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)) {
    WINDOWPLACEMENT restored{};
    restored.length           = sizeof(restored);
    restored.showCmd          = placement.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    restored.rcNormalPosition = rect;
    return SetWindowPlacement(window, &restored) != 0;
  }
  if (IsZoomed(window))
    ShowWindow(window, SW_SHOWNORMAL);
  return SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                      SWP_NOZORDER | SWP_NOACTIVATE)
         != 0;
}

void SaveWindowMemory(const window_placement::Placement& placement)
{
  if (!Config::Get().graphics.remember_window)
    return;
  std::ofstream file(WindowMemoryFile(), std::ios::trunc);
  if (file << window_placement::Format(placement) << '\n') {
    window_memory_saved = placement;
    return;
  }
  static bool warned = false;
  if (!std::exchange(warned, true))
    spdlog::warn("[WindowMemory] could not write {}", WindowMemoryFile().string());
}

void RestoreWindowPlacement(HWND window)
{
  std::ifstream file(WindowMemoryFile());
  std::string   line;
  if (!file || !std::getline(file, line))
    return;
  const auto saved = window_placement::Parse(line);
  if (!saved) {
    spdlog::warn("[WindowMemory] ignoring the saved window '{}'", line);
    return;
  }
  // A window on no monitor is left to the game, unless it is maximized or goes borderless, which place it on one.
  const RECT rect{saved->left, saved->top, saved->right, saved->bottom};
  if (!saved->maximized && !saved->borderless && !MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)) {
    spdlog::info("[WindowMemory] the saved window {} is on no monitor; leaving the window where the game put it", line);
    return;
  }
  if (!ApplyPlacement(window, *saved)) {
    spdlog::warn("[WindowMemory] could not restore the window: error {}", GetLastError());
    return;
  }
  window_memory_saved = saved;
  spdlog::info("[WindowMemory] restored the window to {}", line);
  if (saved->borderless && Config::Get().graphics.borderless_fullscreen)
    EnterBorderless(window, *saved);
}

void WindowMemoryFrame()
{
  static Throttle throttle(std::chrono::seconds(2));
  if (!Config::Get().graphics.remember_window || !throttle.Due())
    return;
  auto* window = GameWindow();
  if (!window || IsIconic(window) || !HasTitleBar(window) || borderless_active)
    return;
  if (!window_memory_restored) {
    window_memory_restored = true;
    RestoreWindowPlacement(window);
    return;
  }

  const auto current = CurrentWindowPlacement(window);
  if (current && current != window_memory_saved)
    SaveWindowMemory(*current);
}

// --- Borderless full screen ---

// [graphics] borderless_fullscreen: F11 ([shortcuts] toggle_borderless) takes the window's frame away and stretches it
// over its monitor, and gives the window back as it was. With remember_window the window it returns to is saved with
// the borderless flag, so the next session starts borderless too. A window without a title bar the mod did not make is
// in one of the game's own full screen modes, which F11 leaves alone.
constexpr int32_t           kBorderlessRetakes = 3;
LONG_PTR                    borderless_style   = 0;
window_placement::Placement borderless_from;
int32_t                     borderless_retakes = 0;

bool EnterBorderless(HWND window, const window_placement::Placement& from)
{
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
    spdlog::warn("[Borderless] could not find the window's monitor: error {}", GetLastError());
    return false;
  }
  // Restored first, so the frame comes back on a normal window rather than one Windows still counts as maximized.
  if (IsZoomed(window))
    ShowWindow(window, SW_SHOWNORMAL);
  const auto style = GetWindowLongPtrW(window, GWL_STYLE);
  SetWindowLongPtrW(window, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
  const auto& area = monitor.rcMonitor;
  if (!SetWindowPos(window, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top,
                    SWP_FRAMECHANGED | SWP_NOOWNERZORDER)) {
    spdlog::warn("[Borderless] could not cover the monitor: error {}", GetLastError());
    SetWindowLongPtrW(window, GWL_STYLE, style);
    SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ApplyPlacement(window, from);
    return false;
  }
  borderless_style           = style;
  borderless_from            = from;
  borderless_from.borderless = false;
  borderless_active          = true;

  auto saved       = borderless_from;
  saved.borderless = true;
  SaveWindowMemory(saved);
  spdlog::info("[Borderless] borderless full screen at {} {} {} {}", area.left, area.top, area.right, area.bottom);
  return true;
}

void LeaveBorderless(HWND window)
{
  borderless_active = false;
  SetWindowLongPtrW(window, GWL_STYLE, borderless_style);
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  if (!ApplyPlacement(window, borderless_from))
    spdlog::warn("[Borderless] could not give the window back: error {}", GetLastError());
  SaveWindowMemory(borderless_from);
  spdlog::info("[Borderless] windowed again at {}", window_placement::Format(borderless_from));
}

void BorderlessFrame()
{
  const auto& config = Config::Get();
  auto*       window = GameWindow();
  if (!window)
    return;
  // Turned off in the settings file while borderless.
  if (borderless_active && !config.graphics.borderless_fullscreen) {
    LeaveBorderless(window);
    return;
  }

  // Keys typed into a text field (chat, search) are text, not shortcuts.
  if (config.graphics.borderless_fullscreen && config.control.hotkeys_enabled && !unity::Key::IsInputFocused()
      && Shortcut::IsDown(Action::kToggleBorderless)) {
    borderless_retakes = 0;
    if (borderless_active) {
      LeaveBorderless(window);
    } else if (!HasTitleBar(window)) {
      spdlog::info("[Borderless] the game is in its own full screen mode; set it to windowed for F11");
    } else if (const auto from = CurrentWindowPlacement(window); from && !IsIconic(window)) {
      EnterBorderless(window, *from);
    }
    return;
  }

  // The game can give its window the frame back, such as when it changes the resolution; it is taken again a few
  // times, then left to the game.
  static Throttle throttle(std::chrono::seconds(1));
  if (!borderless_active || !throttle.Due() || !HasTitleBar(window))
    return;
  if (++borderless_retakes > kBorderlessRetakes) {
    spdlog::warn("[Borderless] the game keeps taking its window back; leaving it windowed");
    borderless_active = false;
    SaveWindowMemory(borderless_from);
    return;
  }
  spdlog::info("[Borderless] the game gave the window its frame back; taking it again");
  EnterBorderless(window, borderless_from);
}

// --- Background frame rate ---

// [graphics] background_fps: while a window of another program has the focus, the game draws at most that many
// frames a second, with vsync off since Unity ignores the cap otherwise. The game's own cap and vsync come back when
// it has the focus again. 0 leaves both alone.
bool    background_capped     = false;
int32_t background_saved_rate = -1;
int32_t background_saved_sync = 0;

bool GameHasFocus()
{
  DWORD process = 0;
  if (auto* foreground = GetForegroundWindow())
    GetWindowThreadProcessId(foreground, &process);
  return process == GetCurrentProcessId();
}

void RestoreForegroundPacing()
{
  unity::FramePacing::SetVSyncCount(background_saved_sync);
  unity::FramePacing::SetTargetFrameRate(background_saved_rate);
  background_capped = false;
}

void BackgroundFpsFrame()
{
  static Throttle throttle(std::chrono::milliseconds(500));
  if (!throttle.Due())
    return;
  const int32_t limit = Config::Get().graphics.background_fps;
  const bool    cap   = limit > 0 && !GameHasFocus();
  if (!cap) {
    if (background_capped)
      RestoreForegroundPacing();
    return;
  }

  const auto rate = unity::FramePacing::targetFrameRate();
  const auto sync = unity::FramePacing::vSyncCount();
  if (!rate || !sync)
    return;
  if (!background_capped) {
    background_saved_rate = *rate;
    background_saved_sync = *sync;
    background_capped     = true;
    static bool logged    = false;
    if (!std::exchange(logged, true))
      spdlog::info("[BackgroundFps] {} fps in the background; the game's {} fps and vsync {} return with the focus",
                   limit, *rate, *sync);
  }
  // Applied again whenever the game sets its own values while in the background, such as from its settings.
  if (*sync != 0)
    unity::FramePacing::SetVSyncCount(0);
  if (*rate != limit)
    unity::FramePacing::SetTargetFrameRate(limit);
}
} // namespace

// The display scale of the monitor holding the game window, cached until `refresh` or a move to another monitor: the
// display mode's physical width over the width Windows reports to this process, 1.5 when it scales the game to 150%.
float WindowDpiScale(bool refresh)
{
  static HMONITOR last_monitor = nullptr;
  static float    scale        = 1.0f;

  // The game's own window, not GetActiveWindow(): that is null while the game is in the background, which would
  // pick the primary monitor.
  const auto monitor = MonitorFromWindow(GameWindow(), MONITOR_DEFAULTTONEAREST);
  if (!refresh && monitor == last_monitor)
    return scale;

  MONITORINFOEX info{};
  info.cbSize = sizeof(info);
  DEVMODE mode{.dmSize = sizeof(DEVMODE)};
  if (GetMonitorInfo(monitor, &info) && EnumDisplaySettings(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
    const auto logical_width = info.rcMonitor.right - info.rcMonitor.left;
    if (logical_width > 0)
      scale = static_cast<float>(mode.dmPelsWidth) / static_cast<float>(logical_width);
  }
  last_monitor = monitor;
  return scale;
}

const Feature kFreeResize{"FreeResize", &Config::Patches::freeresizehooks, InstallFreeResize};
// TODO: allow_cursor is not a test patch; it sits in testpatches only because it always has.
const Feature kCursor{"Cursor", &Config::Patches::testpatches, InstallCursor};
const Feature kWindowMemory{"WindowMemory", nullptr, nullptr, WindowMemoryFrame};
const Feature kBorderless{"Borderless", nullptr, nullptr, BorderlessFrame};
const Feature kBackgroundFps{"BackgroundFps", nullptr, nullptr, BackgroundFpsFrame};
#else
float WindowDpiScale(bool)
{ return 1.0f; }
#endif

// [ngo] battle_summary: when a battle report is opened, the game's own export of it is summed up in a panel at the
// bottom of the report, between the two ships, and once in the log ("[BattleSummary] ..."). It steps aside while the
// game's detailed log is open, and closes with the report. Unity work runs on the frame tick; the detours only note
// which report was asked for. On Windows, its button copies the summary as an image to the clipboard, to paste into
// Discord; the image is drawn with GDI and never saved.

#include "config.h"
#include "features/battle_summary.h"
#include "features/features.h"

#include "game/battle.h"
#include "game/ui.h"

#include <il2cpp/hook.h>
#include <il2cpp/il2cpp.h>
#include <spdlog/spdlog.h>
#include <unity/tracker.h>
#include <unity/ui.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#if _WIN32
#include <cstring>
#include <vector>

#include <Windows.h>
#endif

namespace
{
// Reports summed up this session, by id; a report whose export did not parse is only in battle_summary_done.
auto& battle_summary_cache = *new std::unordered_map<int64_t, battle_summary::Summary>;
auto& battle_summary_done  = *new std::unordered_set<int64_t>;
// The report opened last, and its summary once it is fetched.
int64_t battle_summary_pending = 0;
auto&   battle_summary_current = *new std::optional<battle_summary::Summary>;
// The report screen was on screen at the last tick: when it closes, the summary goes with it.
bool battle_summary_view_shown = false;

// The panel and its label, on the report's canvas. Never destroyed: Unity destroys them with the canvas.
auto&       battle_summary_label = *new il2cpp::GcHandle;
auto&       battle_summary_text  = *new il2cpp::GcHandle;
auto&       battle_summary_panel = *new il2cpp::GcHandle;
std::string battle_summary_shown_text;
int32_t     battle_summary_placed_lines = -1;
float       battle_summary_placed_scale = 0;
// The panel is on screen.
bool battle_summary_visible = false;

#if _WIN32
// The copy button and its label, children of the panel.
auto& battle_summary_button       = *new il2cpp::GcHandle;
auto& battle_summary_button_label = *new il2cpp::GcHandle;
// The left mouse button was down at the last frame: a click is the frame it goes down.
bool battle_summary_mouse_down = false;
// After a copy, the button says how it went until then.
std::chrono::steady_clock::time_point battle_summary_copied_until;
std::string                           battle_summary_button_text;

constexpr const char* kBattleSummaryCopyText = "In Zwischenablage kopieren";
constexpr float       kBattleSummaryButtonWidth  = 300;
constexpr float       kBattleSummaryButtonHeight = 36;
// The row below the text that holds the button.
constexpr float kBattleSummaryButtonRow = 48;
#else
constexpr float kBattleSummaryButtonRow = 0;
#endif

// The sizes at [ngo] fleet_bar_scale 1; the scale multiplies them all.
constexpr float kBattleSummaryFontSize   = 20;
constexpr float kBattleSummaryLineHeight = 28;
constexpr float kBattleSummaryPadding    = 18;
constexpr float kBattleSummaryWidth      = 1000;
// Centred between the two ships, this far above the screen's bottom edge: above the report's reward bar.
constexpr float kBattleSummaryBottom = 160;

// The summary of a fetched report, made once; null while it is not fetched or when its export does not parse.
const battle_summary::Summary* BattleSummaryOf(BattleResultsManager* manager, int64_t id)
{
  if (const auto found = battle_summary_cache.find(id); found != battle_summary_cache.end())
    return &found->second;
  if (!manager || battle_summary_done.contains(id))
    return nullptr;
  auto* report = manager->Report(id);
  if (!report)
    return nullptr;
  battle_summary_done.insert(id);
  auto*      target      = manager->TargetData(report);
  auto*      export_text = target ? manager->ExportText(target) : nullptr;
  const auto text        = export_text ? il2cpp::ToUtf8(export_text) : std::string();
  if (text.empty()) {
    spdlog::info("[BattleSummary] report {}: the export is empty (target data {})", id, target ? "found" : "missing");
    return nullptr;
  }
  auto summary = battle_summary::Parse(text);
  if (!summary) {
    spdlog::info("[BattleSummary] report {}: the export does not parse; it starts with: {}", id, text.substr(0, 200));
    return nullptr;
  }
  spdlog::info("[BattleSummary] report {}: {}", id, battle_summary::Describe(*summary));
  spdlog::info("[BattleSummary] report {}: {}", id, battle_summary::DescribeDamage(*summary));
  for (const auto& section : summary->unread)
    spdlog::info("[BattleSummary] report {}: a section not read yet: {}", id, section);
  return &battle_summary_cache.emplace(id, std::move(*summary)).first->second;
}

void BattleResultsManager_ShowBattleReport_Hook(auto original, BattleResultsManager* _this, il2cpp::Object* header)
{
  original(_this, header);
  if (!header || !Config::Get().ui.battle_summary)
    return;
  battle_summary_pending = header->Get<"ID", int64_t>();
  battle_summary_current.reset();
  if (const auto* summary = BattleSummaryOf(_this, battle_summary_pending))
    battle_summary_current = *summary;
}

void BattleResultsManager_OnSuccessFetchBattleReport_Hook(auto original, BattleResultsManager* _this, int64_t id)
{
  original(_this, id);
  if (id != battle_summary_pending || !Config::Get().ui.battle_summary)
    return;
  if (const auto* summary = BattleSummaryOf(_this, id))
    battle_summary_current = *summary;
}

bool InstallBattleSummary()
{
  unity::Track<BattleReportViewController>();
  il2cpp::HookSet hooks("BattleSummary");
  IL2CPP_STAGE(hooks, BattleResultsManager::Class::Method("ShowBattleReport", 1),
               BattleResultsManager_ShowBattleReport_Hook);
  IL2CPP_STAGE(hooks, BattleResultsManager::Class::Method("OnSuccessFetchBattleReport", 1),
               BattleResultsManager_OnSuccessFetchBattleReport_Hook);
  return hooks.Commit();
}

// Builds the panel in the report's own font. Null while the report has no text up yet; the next tick tries again.
unity::TMP_Text* CreateBattleSummaryPanel(BattleReportViewController* view, unity::Transform* parent)
{
  auto* view_object = view->gameObject();
  auto* source      = view_object ? view_object->GetComponentInChildren<unity::TextMeshProUGUI>() : nullptr;
  auto* font        = source ? source->font() : nullptr;
  if (!font)
    return nullptr;

  il2cpp::GcHandle panel, text;
  auto*            panel_rect = unity::CreateUiObject("CommunityMod_BattleSummary", parent, panel);
  if (!panel_rect)
    return nullptr;
  if (auto* image = panel.Get<unity::GameObject>()->AddComponent<unity::Image>()) {
    image->SetColor({0.04f, 0.07f, 0.12f, 0.9f});
    image->SetRaycastTarget(false);
  }

  auto* text_rect = unity::CreateUiObject("BattleSummaryText", panel_rect, text);
  auto* label     = text_rect ? text.Get<unity::GameObject>()->AddComponent<unity::TextMeshProUGUI>() : nullptr;
  if (!label)
    return nullptr;
  label->SetFont(font);
  label->SetAlignment(unity::TMP_Text::Alignment::Left);
  label->SetColor({0.89f, 0.93f, 0.96f, 1});
  label->SetRaycastTarget(false);
  label->SetEnableWordWrapping(false);
  label->SetRichText(true);
  text.Get<unity::GameObject>()->SetActive(true);

#if _WIN32
  // The copy button: a box that takes the click from the game's UI, with a label.
  il2cpp::GcHandle button, button_text;
  auto*            button_rect = unity::CreateUiObject("BattleSummaryCopy", panel_rect, button);
  if (auto* image = button_rect ? button.Get<unity::GameObject>()->AddComponent<unity::Image>() : nullptr) {
    image->SetColor({0.12f, 0.25f, 0.42f, 1});
    image->SetRaycastTarget(true);
  }
  auto* button_label_rect = button_rect ? unity::CreateUiObject("BattleSummaryCopyText", button_rect, button_text)
                                        : nullptr;
  auto* button_label      = button_label_rect
                                ? button_text.Get<unity::GameObject>()->AddComponent<unity::TextMeshProUGUI>()
                                : nullptr;
  if (button_label) {
    button_label->SetFont(font);
    button_label->SetAlignment(unity::TMP_Text::Alignment::Center);
    button_label->SetColor({0.89f, 0.93f, 0.96f, 1});
    button_label->SetRaycastTarget(false);
    button_label->SetEnableWordWrapping(false);
    button_label->SetText(kBattleSummaryCopyText);
    button_label_rect->Place({0, 0}, {1, 1}, {0.5f, 0.5f}, {0, 0}, {0, 0});
    button_text.Get<unity::GameObject>()->SetActive(true);
    button.Get<unity::GameObject>()->SetActive(true);
    battle_summary_button       = il2cpp::GcHandle::Strong(button_rect);
    battle_summary_button_label = il2cpp::GcHandle::Strong(button_label);
    battle_summary_button_text  = kBattleSummaryCopyText;
  }
#endif
  battle_summary_label = il2cpp::GcHandle::Strong(label);
  battle_summary_text  = il2cpp::GcHandle::Strong(text_rect);
  battle_summary_panel = il2cpp::GcHandle::Strong(panel_rect);
  battle_summary_shown_text.clear();
  battle_summary_placed_lines = -1;
  return label;
}

void PlaceBattleSummaryPanel(unity::RectTransform* panel, unity::TMP_Text* label, int32_t lines, float scale)
{
  if (lines == battle_summary_placed_lines && scale == battle_summary_placed_scale)
    return;
  battle_summary_placed_lines = lines;
  battle_summary_placed_scale = scale;
  const float padding         = kBattleSummaryPadding * scale;
  // The headline is 30 % larger than the other lines.
  const float row    = kBattleSummaryButtonRow * scale;
  const float height = (static_cast<float>(lines) + 0.3f) * kBattleSummaryLineHeight * scale + 2 * padding + row;
  const unity::Vector2 size{kBattleSummaryWidth * scale, height};
  panel->Place({0.5f, 0}, {0.5f, 0}, {0.5f, 0}, size, {0, kBattleSummaryBottom});
  // The text above the button's row.
  if (auto* text = battle_summary_text.Get<unity::RectTransform>(); unity::Alive(text))
    text->Place({0, 0}, {1, 1}, {0.5f, 0.5f}, {-2 * padding, -2 * padding - row}, {0, row / 2});
  label->SetFontSize(kBattleSummaryFontSize * scale);
#if _WIN32
  if (auto* button = battle_summary_button.Get<unity::RectTransform>(); unity::Alive(button))
    button->Place({1, 0}, {1, 0}, {1, 0}, {kBattleSummaryButtonWidth * scale, kBattleSummaryButtonHeight * scale},
                  {-padding, padding});
  if (auto* button_label = battle_summary_button_label.Get<unity::TMP_Text>(); unity::Alive(button_label))
    button_label->SetFontSize(kBattleSummaryFontSize * 0.85f * scale);
#endif
}

#if _WIN32
// UnityEngine.RectTransformUtility, for the button's hit test.
struct BattleSummaryRectUtility {
  using Class = il2cpp::Class<"UnityEngine.UIModule", "UnityEngine", "RectTransformUtility">;
};

// UnityEngine.RenderMode: an overlay canvas takes no camera for the hit test.
enum class BattleSummaryRenderMode : int32_t {
  ScreenSpaceOverlay = 0,
};

// The mouse is over the button, on the button's canvas.
bool BattleSummaryButtonHit(unity::RectTransform* button)
{
  // A nested canvas renders as its root canvas does.
  auto*          nearest = button->FindInParents<unity::Canvas>();
  auto*          canvas  = nearest ? nearest->Get<"rootCanvas", unity::Canvas*>() : nullptr;
  unity::Camera* camera  = nullptr;
  if (canvas && canvas->Get<"renderMode", BattleSummaryRenderMode>() != BattleSummaryRenderMode::ScreenSpaceOverlay)
    camera = canvas->Get<"worldCamera", unity::Camera*>();
  const auto mouse = unity::Input::mousePosition();
  return BattleSummaryRectUtility::Class::Call<"RectangleContainsScreenPoint",
                                               bool(unity::RectTransform*, unity::Vector2, unity::Camera*)>(
      button, unity::Vector2{mouse.x, mouse.y}, camera);
}

std::wstring BattleSummaryWide(std::string_view text)
{
  if (text.empty())
    return {};
  const int    size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

// "#RRGGBB", or `fallback` when empty or malformed.
COLORREF BattleSummaryColor(const std::string& hex, COLORREF fallback)
{
  if (hex.size() != 7 || hex[0] != '#')
    return fallback;
  unsigned rgb = 0;
  for (size_t i = 1; i < 7; ++i) {
    const char c = hex[i];
    const int  digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (digit < 0)
      return fallback;
    rgb = rgb * 16 + static_cast<unsigned>(digit);
  }
  return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// Draws the summary as the panel lays it out, on a dark background, and puts it on the clipboard as a bitmap
// (CF_DIB), which Discord and other programs paste as an image. Nothing is written to disk.
bool CopyBattleSummaryImage(const battle_summary::Summary& summary)
{
  constexpr int kWidth    = 1000;
  constexpr int kPadding  = 24;
  constexpr int kLine     = 26;
  constexpr int kHeadline = 38;
  constexpr int kFooter   = 28;
  const auto    lines     = battle_summary::Layout(summary);
  const int     height    = 2 * kPadding + kHeadline + (static_cast<int>(lines.size()) - 1) * kLine + kFooter;

  BITMAPINFO info{};
  info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth       = kWidth;
  info.bmiHeader.biHeight      = height;
  info.bmiHeader.biPlanes      = 1;
  info.bmiHeader.biBitCount    = 24;
  info.bmiHeader.biCompression = BI_RGB;
  void*   bits                 = nullptr;
  HDC     dc                   = CreateCompatibleDC(nullptr);
  HBITMAP bitmap               = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
  if (!bitmap || !bits) {
    if (bitmap)
      DeleteObject(bitmap);
    if (dc)
      DeleteDC(dc);
    return false;
  }
  const auto font_of = [](int size, int weight) {
    return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  };
  HFONT text_font     = font_of(18, FW_NORMAL);
  HFONT headline_font = font_of(28, FW_BOLD);
  HFONT footer_font   = font_of(14, FW_NORMAL);
  auto* old_bitmap    = SelectObject(dc, bitmap);
  auto* old_font      = SelectObject(dc, text_font);

  RECT   all{0, 0, kWidth, height};
  HBRUSH background = CreateSolidBrush(RGB(10, 18, 31));
  FillRect(dc, &all, background);
  DeleteObject(background);
  SetBkMode(dc, TRANSPARENT);

  const int inner = kWidth - 2 * kPadding;
  int       y     = kPadding;
  for (const auto& line : lines) {
    SelectObject(dc, line.headline ? headline_font : text_font);
    int x = kPadding;
    for (const auto& segment : line.segments) {
      if (segment.at > 0)
        x = kPadding + inner * segment.at / 100;
      const auto wide = BattleSummaryWide(segment.text);
      SetTextColor(dc, BattleSummaryColor(segment.color, RGB(227, 237, 245)));
      TextOutW(dc, x, y, wide.c_str(), static_cast<int>(wide.size()));
      SIZE extent{};
      GetTextExtentPoint32W(dc, wide.c_str(), static_cast<int>(wide.size()), &extent);
      x += extent.cx;
    }
    y += line.headline ? kHeadline : kLine;
  }
  SelectObject(dc, footer_font);
  SetTextColor(dc, RGB(0x8F, 0xA3, 0xB8));
  const std::wstring footer = L"NGO Community Mod";
  SIZE               footer_extent{};
  GetTextExtentPoint32W(dc, footer.c_str(), static_cast<int>(footer.size()), &footer_extent);
  TextOutW(dc, kWidth - kPadding - footer_extent.cx, height - kPadding / 2 - footer_extent.cy, footer.c_str(),
           static_cast<int>(footer.size()));
  GdiFlush();

  // CF_DIB: the header, then the rows bottom-up, each padded to four bytes, as the DIB section holds them.
  const size_t stride = (static_cast<size_t>(kWidth) * 3 + 3) & ~static_cast<size_t>(3);
  const size_t bytes  = stride * static_cast<size_t>(height);
  bool         copied = false;
  if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bytes)) {
    if (auto* target = static_cast<unsigned char*>(GlobalLock(memory))) {
      auto header        = info.bmiHeader;
      header.biSizeImage = static_cast<DWORD>(bytes);
      std::memcpy(target, &header, sizeof header);
      std::memcpy(target + sizeof header, bits, bytes);
      GlobalUnlock(memory);
      HWND owner = GetActiveWindow();
      if (!owner)
        owner = GetForegroundWindow();
      if (OpenClipboard(owner)) {
        EmptyClipboard();
        copied = SetClipboardData(CF_DIB, memory) != nullptr;
        CloseClipboard();
      }
    }
    if (!copied)
      GlobalFree(memory);
  }

  SelectObject(dc, old_font);
  SelectObject(dc, old_bitmap);
  DeleteObject(text_font);
  DeleteObject(headline_font);
  DeleteObject(footer_font);
  DeleteObject(bitmap);
  DeleteDC(dc);
  return copied;
}

void SetBattleSummaryButtonText(const char* text)
{
  auto* label = battle_summary_button_label.Get<unity::TMP_Text>();
  if (!unity::Alive(label) || battle_summary_button_text == text)
    return;
  battle_summary_button_text = text;
  label->SetText(text);
}

// Every frame while the panel shows, as a click is shorter than the panel's tick: copies on the frame the left
// button goes down over the copy button.
void BattleSummaryCopyClick()
{
  if (!battle_summary_visible || !battle_summary_current) {
    battle_summary_mouse_down = false;
    return;
  }
  const bool down = unity::Input::GetMouseButton(0);
  const bool click = down && !battle_summary_mouse_down;
  battle_summary_mouse_down = down;
  auto* button = click ? battle_summary_button.Get<unity::RectTransform>() : nullptr;
  if (!unity::Alive(button) || !BattleSummaryButtonHit(button))
    return;
  const bool copied = CopyBattleSummaryImage(*battle_summary_current);
  spdlog::info("[BattleSummary] {}", copied ? "copied the summary to the clipboard as an image"
                                            : "could not copy the summary to the clipboard");
  SetBattleSummaryButtonText(copied ? "Kopiert: in Discord mit Strg+V einfügen" : "Kopieren fehlgeschlagen");
  battle_summary_copied_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}
#endif

void BattleSummaryFrame()
{
#if _WIN32
  BattleSummaryCopyClick();
#endif
  static Throttle throttle(std::chrono::milliseconds(100));
  if (!throttle.Due())
    return;
  const auto& config = Config::Get();
  auto*       view   = config.ui.battle_summary ? unity::Newest<BattleReportViewController>() : nullptr;
  const bool  shown  = view && CanvasController::OnScreen(view);
  if (!shown && battle_summary_view_shown)
    battle_summary_current.reset();
  battle_summary_view_shown = shown;
  // The game's detailed log covers the report and says it all: the panel steps aside until it closes.
  const bool log_open = shown && view->logShowing();
  auto* parent = shown && !log_open && battle_summary_current ? unity::CanvasRoot(view->transform()) : nullptr;

  auto* label = battle_summary_label.Get<unity::TMP_Text>();
  auto* panel = battle_summary_panel.Get<unity::RectTransform>();
  if (!unity::Alive(label) || !unity::Alive(panel))
    label = nullptr;
  // Hidden without a report, and left behind when the report builds a new canvas.
  if (label && (!parent || panel->parent() != parent)) {
    if (auto* object = panel->gameObject())
      object->SetActive(false);
    battle_summary_visible = false;
    if (parent)
      label = nullptr;
  }
  if (!parent)
    return;
  if (!label && !(label = CreateBattleSummaryPanel(view, parent)))
    return;
  panel = battle_summary_panel.Get<unity::RectTransform>();

  if (auto text = battle_summary::PanelText(*battle_summary_current); text != battle_summary_shown_text) {
    battle_summary_shown_text = std::move(text);
    label->SetText(battle_summary_shown_text);
  }
  PlaceBattleSummaryPanel(panel, label, battle_summary::PanelLines(battle_summary_shown_text),
                          config.ui.fleet_bar_scale);
  if (auto* object = panel->gameObject())
    object->SetActive(true);
  battle_summary_visible = true;
#if _WIN32
  if (std::chrono::steady_clock::now() >= battle_summary_copied_until)
    SetBattleSummaryButtonText(kBattleSummaryCopyText);
#endif
}

void ResetBattleSummary()
{
  battle_summary_pending = 0;
  battle_summary_current.reset();
  battle_summary_view_shown = false;
  battle_summary_visible    = false;
}
} // namespace

const Feature kBattleSummary{"BattleSummary", nullptr, InstallBattleSummary, BattleSummaryFrame, ResetBattleSummary};

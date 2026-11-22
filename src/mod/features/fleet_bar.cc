// [ui] fleet_bar: a compact list of the fleet bar's ships, with status and remaining time, at the side of the system
// and galaxy views. [shortcuts] toggle_fleet_bar hides and shows it. [ui] fleet_notifications sends a Windows
// notification when one of those ships finishes a repair, arrives, fills its cargo hold, stops mining or is
// destroyed. It only reads what the fleet bar already holds; all Unity work runs on the frame tick, and there are no
// detours of its own.

#include "config.h"
#include "features/features.h"
#include "features/fleet_bar.h"
#include "shortcuts.h"

#include "game/app.h"
#include "game/fleet.h"
#include "game/hud.h"
#include "game/ui.h"

#include <il2cpp/il2cpp.h>
#include <spdlog/spdlog.h>
#include <unity/input.h>
#include <unity/ui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
// The panel and its label, under the HUD's canvas, so they follow its scale and visibility. Unity destroys them with
// the HUD; they are then built again. Never destroyed: freeing a GC handle in a static destructor can run after the
// runtime has shut down.
auto&       fleet_bar_label = *new il2cpp::GcHandle;
auto&       fleet_bar_text  = *new il2cpp::GcHandle;
auto&       fleet_bar_panel = *new il2cpp::GcHandle;
std::string fleet_bar_shown_text;
// The layout last placed: line count, compact, right side, scale. -1 lines forces the first placement.
int32_t fleet_bar_placed_lines   = -1;
bool    fleet_bar_placed_compact = false;
bool    fleet_bar_placed_right   = false;
float   fleet_bar_placed_scale   = 0;
// Hidden with toggle_fleet_bar until it is pressed again or the game restarts.
bool fleet_bar_hidden = false;
// Ship names by localisation id: each lookup allocates a managed object.
auto& fleet_bar_names = *new std::unordered_map<int64_t, std::string>;
// Each fleet's row at the last tick, by fleet id, to see what changed.
auto& fleet_bar_previous = *new std::unordered_map<int64_t, fleet_bar::Row>;

// The sizes at [ui] fleet_bar_scale 1; the scale multiplies them all.
constexpr std::size_t kFleetBarShips       = 8;
constexpr float       kFleetBarFontSize    = 20;
constexpr float       kFleetBarLineHeight  = 28;
constexpr float       kFleetBarPadding     = 16;
constexpr float       kFleetBarWidth       = 480;
constexpr float       kFleetBarCompactWide = 160;
// From the screen's side, and the top edge at this fraction of its height from the bottom.
constexpr float kFleetBarMargin = 12;
constexpr float kFleetBarTop    = 0.62f;

fleet_bar::Status FleetBarStatus(FleetState state)
{
  using fleet_bar::Status;
  switch (state) {
  case FleetState::IdleInSpace:
    return Status::kIdle;
  case FleetState::Docked:
    return Status::kDocked;
  case FleetState::Mining:
    return Status::kMining;
  case FleetState::Destroyed:
    return Status::kDestroyed;
  case FleetState::Repairing:
    return Status::kRepairing;
  case FleetState::Battling:
    return Status::kBattling;
  case FleetState::WarpCharging:
    return Status::kWarpCharging;
  case FleetState::Warping:
    return Status::kWarping;
  case FleetState::Impulsing:
    return Status::kImpulsing;
  case FleetState::Capturing:
    return Status::kCapturing;
  case FleetState::AutoHunting:
    return Status::kAutoHunting;
  case FleetState::TieringUp:
    return Status::kTieringUp;
  case FleetState::Outposting:
    return Status::kOutposting;
  default:
    return Status::kUnknown;
  }
}

// The timer the state runs on, as fleet sync reads it: the mining slot's while mining, the fleet's own while it
// repairs or travels. Empty without one.
std::optional<int64_t> FleetBarRemaining(FleetPlayerData* fleet, FleetState state, int64_t now)
{
  ITimerData* timer = nullptr;
  if (state == FleetState::Mining)
    timer = fleet->miningSlotTimer();
  else if (state == FleetState::Repairing || state == FleetState::WarpCharging || state == FleetState::Warping
           || state == FleetState::Impulsing)
    timer = fleet->Timer();
  const auto due = timer ? timer->DueTime().UnixSeconds() : std::nullopt;
  if (!due)
    return std::nullopt;
  return std::max<int64_t>(*due - now, 0);
}

std::optional<int32_t> FleetBarCargo(FleetPlayerData* fleet)
{
  auto* hold    = fleet->CargoHoldData();
  auto* current = hold ? hold->CurrentCargo() : nullptr;
  if (!current || current->MaxValue() <= 0)
    return std::nullopt;
  return static_cast<int32_t>(std::clamp(current->CurrentValue() / current->MaxValue(), 0.0, 1.0) * 100);
}

const std::string& FleetBarName(FleetPlayerData* fleet)
{
  const int64_t loca = fleet->Hull() ? fleet->GetLocaId() : 0;
  auto [entry, added] = fleet_bar_names.try_emplace(loca);
  if (added)
    entry->second = LocalizedShipName(loca);
  return entry->second;
}

// The ships in the order of the fleet bar, which is the order of the select-ship keys.
std::vector<fleet_bar::Row> FleetBarRows()
{
  auto* bar     = unity::Newest<FleetBarViewController>();
  auto* context = bar ? bar->CanvasContext() : nullptr;
  if (!context)
    return {};
  const int64_t               now = std::time(nullptr);
  std::vector<fleet_bar::Row> rows;
  int32_t                     key = 0;
  for (auto* fleet : il2cpp::Items(context->FleetData())) {
    if (++key > static_cast<int32_t>(kFleetBarShips))
      break;
    if (!fleet || !fleet->HasShip())
      continue;
    const auto state = fleet->CurrentState();
    rows.push_back({
        .key           = key,
        .name          = FleetBarName(fleet),
        .status        = FleetBarStatus(state),
        .remaining     = FleetBarRemaining(fleet, state, now),
        .cargo_percent = state == FleetState::Mining ? FleetBarCargo(fleet) : std::nullopt,
        .fleet_id      = fleet->Id(),
    });
  }
  return rows;
}

// The rounded key-hint background of the HUD, as the anomaly panel uses it; null leaves a plain rectangle.
unity::Sprite* FleetBarSprite(HudNavigationViewController* nav)
{
  auto* button     = nav->galacticAnomaliesButtonWidget();
  auto* transform  = button ? button->transform() : nullptr;
  auto* parent     = transform ? transform->parent() : nullptr;
  auto* background = parent ? parent->Find("ShortcutKeybindHint/Background") : nullptr;
  auto* object     = background ? background->gameObject() : nullptr;
  auto* image      = object ? object->GetComponent<unity::Image>() : nullptr;
  if (!image)
    return nullptr;
  auto* sprite = image->overrideSprite();
  return sprite ? sprite : image->sprite();
}

// Builds the panel in the HUD's own font. Null while the HUD's font is not up yet; the next tick tries again.
unity::TMP_Text* CreateFleetBar(HudNavigationViewController* nav, unity::Transform* parent)
{
  auto* helper = nav->factionInformationHelper();
  auto* name   = il2cpp::At(helper ? helper->systemName() : nullptr, 0);
  auto* object = unity::Alive(name) ? name->gameObject() : nullptr;
  auto* source = object ? object->GetComponent<unity::TextMeshProUGUI>() : nullptr;
  auto* font   = source ? source->font() : nullptr;
  if (!font)
    return nullptr;

  il2cpp::GcHandle panel, text;
  auto*            panel_rect = unity::CreateUiObject("CommunityMod_FleetBar", parent, panel);
  if (!panel_rect)
    return nullptr;
  if (auto* image = panel.Get<unity::GameObject>()->AddComponent<unity::Image>()) {
    image->SetColor({0.04f, 0.07f, 0.12f, 0.78f});
    image->SetRaycastTarget(false);
    if (auto* sprite = FleetBarSprite(nav); sprite && sprite->pixelsPerUnit() > 0) {
      image->SetSprite(sprite);
      image->SetType(unity::Image::Type::Sliced);
      image->SetPixelsPerUnitMultiplier(100 / sprite->pixelsPerUnit());
    }
  }

  auto* text_rect = unity::CreateUiObject("FleetBarText", panel_rect, text);
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
  fleet_bar_label = il2cpp::GcHandle::Strong(label);
  fleet_bar_text  = il2cpp::GcHandle::Strong(text_rect);
  fleet_bar_panel = il2cpp::GcHandle::Strong(panel_rect);
  fleet_bar_shown_text.clear();
  fleet_bar_placed_lines = -1;
  return label;
}

void PlaceFleetBar(unity::RectTransform* panel, unity::TMP_Text* label, int32_t lines, bool compact, bool right,
                   float scale)
{
  if (lines == fleet_bar_placed_lines && compact == fleet_bar_placed_compact && right == fleet_bar_placed_right
      && scale == fleet_bar_placed_scale)
    return;
  fleet_bar_placed_lines   = lines;
  fleet_bar_placed_compact = compact;
  fleet_bar_placed_right   = right;
  fleet_bar_placed_scale   = scale;
  const float          padding = kFleetBarPadding * scale;
  const unity::Vector2 size{(compact ? kFleetBarCompactWide : kFleetBarWidth) * scale,
                            static_cast<float>(lines) * kFleetBarLineHeight * scale + padding};
  if (right)
    panel->Place({1, kFleetBarTop}, {1, kFleetBarTop}, {1, 1}, size, {-kFleetBarMargin, 0});
  else
    panel->Place({0, kFleetBarTop}, {0, kFleetBarTop}, {0, 1}, size, {kFleetBarMargin, 0});
  // The text is stretched over the panel, inset by half the padding.
  if (auto* text = fleet_bar_text.Get<unity::RectTransform>(); unity::Alive(text))
    text->Place({0, 0}, {1, 1}, {0.5f, 0.5f}, {-padding, -padding / 2}, {0, 0});
  label->SetFontSize(kFleetBarFontSize * scale);
}

#if _WIN32
bool FleetNotificationWanted(const FleetNotifications& wanted, fleet_bar::Event event)
{
  using fleet_bar::Event;
  switch (event) {
  case Event::kRepaired:
    return wanted.repaired;
  case Event::kArrived:
    return wanted.arrived;
  case Event::kCargoFull:
    return wanted.cargo_full;
  case Event::kMiningDone:
    return wanted.mining_done;
  case Event::kDestroyed:
    return wanted.destroyed;
  }
  return false;
}

// Compares each ship with the last tick. A ship seen for the first time only sets the baseline.
void FleetBarNotify(const std::vector<fleet_bar::Row>& rows)
{
  const auto& ui = Config::Get().ui;
  for (const auto& row : rows) {
    auto [entry, added] = fleet_bar_previous.try_emplace(row.fleet_id, row);
    if (added)
      continue;
    const auto event = fleet_bar::Change(entry->second, row);
    entry->second    = row;
    if (!event || !FleetNotificationWanted(ui.fleet_notifications, *event))
      continue;
    const auto title = fleet_bar::NotificationTitle(row);
    const auto text  = std::string(fleet_bar::EventText(*event));
    const bool quiet = ui.notify_only_in_background && GameWindowActive();
    spdlog::info("[FleetBar] {}: {}{}", title, text, quiet ? " (game in front, no notification)" : "");
    if (!quiet)
      ShowDesktopNotification(title.c_str(), text);
  }
}
#endif

void FleetBarFrame()
{
  const auto& config = Config::Get();
  unity::Key::Poll();
  if (config.ui.fleet_bar && config.control.hotkeys_enabled && !unity::Key::IsInputFocused()
      && Shortcut::IsDown(Action::kToggleFleetBar)) {
    fleet_bar_hidden = !fleet_bar_hidden;
    spdlog::info("[FleetBar] {}", fleet_bar_hidden ? "hidden" : "shown");
  }

  static Throttle throttle(std::chrono::milliseconds(500));
  if (!throttle.Due())
    return;
  const auto section = Hub::CurrentSection();
  auto*      nav     = config.ui.fleet_bar && !fleet_bar_hidden
                          && (section == SectionID::Navigation_System || section == SectionID::Navigation_Galaxy)
                              ? ScreenManager::View<HudNavigationViewController>()
                              : nullptr;
  auto*      parent  = nav ? unity::CanvasRoot(nav->transform()) : nullptr;
#if _WIN32
  const bool notify = config.ui.fleet_notifications.Any();
#else
  const bool notify = false;
#endif
  auto rows = parent || notify ? FleetBarRows() : std::vector<fleet_bar::Row>{};
#if _WIN32
  if (notify)
    FleetBarNotify(rows);
#endif
  if (!parent)
    rows.clear();

  auto* label = fleet_bar_label.Get<unity::TMP_Text>();
  auto* panel = fleet_bar_panel.Get<unity::RectTransform>();
  if (!unity::Alive(label) || !unity::Alive(panel))
    label = nullptr;
  // Hidden without ships or a HUD, and left behind when the HUD builds a new canvas.
  if (label && (rows.empty() || panel->parent() != parent)) {
    if (auto* object = label->ParentObject())
      object->SetActive(false);
    if (parent && panel->parent() != parent)
      label = nullptr;
  }
  if (rows.empty())
    return;
  if (!label && !(label = CreateFleetBar(nav, parent)))
    return;
  panel = fleet_bar_panel.Get<unity::RectTransform>();

  const bool compact = config.ui.fleet_bar_compact;
  if (auto text = fleet_bar::Text(rows, compact); text != fleet_bar_shown_text) {
    fleet_bar_shown_text = std::move(text);
    label->SetText(fleet_bar_shown_text);
  }
  PlaceFleetBar(panel, label, static_cast<int32_t>(rows.size()), compact,
                config.ui.fleet_bar_side == FleetBarSide::kRight, config.ui.fleet_bar_scale);
  if (auto* object = label->ParentObject())
    object->SetActive(true);
}
} // namespace

const Feature kFleetBar{"FleetBar", nullptr, nullptr, FleetBarFrame};

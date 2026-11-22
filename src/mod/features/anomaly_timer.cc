// [graphics] galactic_anomaly_timer: a countdown for the active galactic anomaly in the viewed system, under the HUD's
// Help All / Engage / Claim drawer. [graphics] mirror_space_timer shows the earliest fleet countdown in the viewed
// Mirror system in the same panel. [graphics] defense_timer shows the next of loader_defenses in a panel below it, and
// [ui] defense_reminders sends Windows notifications before each defense. All Unity work runs on the frame tick;
// there are no detours of its own.

#include "config.h"
#include "features/defense_schedule.h"
#include "features/features.h"

#include "game/app.h"
#include "game/hud.h"
#include "game/ui.h"

#include <il2cpp/il2cpp.h>
#include <spdlog/spdlog.h>
#include <unity/ui.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <limits>
#include <map>
#include <optional>
#include <string>

namespace
{
// The countdown label, under a panel parented to the upper-right drawer, so it follows the drawer's scale and
// visibility without moving any native button. Unity destroys it with the drawer; it is then built again. Never
// destroyed: freeing a GC handle in a static destructor can run after the runtime has shut down.
auto&   anomaly_label         = *new il2cpp::GcHandle;
int64_t anomaly_shown_seconds = -1;
// Whether the anomaly panel is showing, so the defense panel can sit below it.
bool anomaly_panel_shown = false;

// The rounded key-hint background next to the native anomaly button, reused for the panel.
unity::Sprite* NativeBackgroundSprite(HudNavigationViewController* nav)
{
  auto* button     = nav->galacticAnomaliesButtonWidget();
  auto* transform  = button ? button->transform() : nullptr;
  auto* parent     = transform ? transform->parent() : nullptr;
  auto* background = parent ? parent->Find("ShortcutKeybindHint/Background") : nullptr;
  auto* object     = background ? background->gameObject() : nullptr;
  auto* image      = object ? object->GetComponent<unity::Image>() : nullptr;
  if (!image)
    return nullptr;
  // ImageSelector applies localized artwork through overrideSprite.
  auto* sprite = image->overrideSprite();
  return sprite ? sprite : image->sprite();
}

void AddPanelImage(unity::GameObject* object, unity::Color tint, unity::Sprite* sprite)
{
  auto* image = object->AddComponent<unity::Image>();
  if (!image)
    return;
  image->SetColor(tint);
  image->SetRaycastTarget(false);
  if (sprite && sprite->pixelsPerUnit() > 0) {
    image->SetSprite(sprite);
    image->SetType(unity::Image::Type::Sliced);
    // Sliced borders are drawn at the sprite's pixel density; normalizing it to 100 keeps the corners the same size
    // whatever the localized artwork's density is.
    image->SetPixelsPerUnitMultiplier(100 / sprite->pixelsPerUnit());
  }
}

int64_t AnomalyRemainingTicks(HudNavigationViewController* nav)
{
  auto*   context = nav->CanvasContext();
  auto*   address = context ? context->ViewingAddress() : nullptr;
  int64_t system  = address ? address->System() : -1;
  auto*   manager = system > 0 ? GalacticAnomaliesManager::Current() : nullptr;
  auto*   anomaly = manager ? manager->GetSystemGalacticAnomalies(system) : nullptr;
  if (!anomaly || !anomaly->IsActive())
    return 0;
  auto* timer = anomaly->EndTimeTimerDataContext();
  return timer ? timer->RemainingTime().ticks : 0;
}

int64_t MirrorRemainingTicks(HudNavigationViewController* nav)
{
  auto* context = nav->CanvasContext();
  auto* address = context ? context->ViewingAddress() : nullptr;
  auto* bar     = unity::Newest<FleetBarViewController>();
  auto* fleets  = bar ? bar->CanvasContext() : nullptr;
  if (!address || !fleets)
    return 0;

  int64_t remaining = 0;
  for (auto* fleet : il2cpp::Items(fleets->FleetData())) {
    if (!fleet || !fleet->IsInMirrorUniverse())
      continue;
    auto*         location = fleet->Address();
    StatusEffect* effect   = nullptr;
    if (!location || location->System() != address->System()
        || !fleet->TryGetStatusEffectData(StatusEffectType::MirrorUniverseTimer, effect) || !effect
        || !effect->IsStatusActive())
      continue;
    const int64_t ticks = effect->GetRemainingTimespan().ticks;
    if (ticks > 0 && (remaining == 0 || ticks < remaining))
      remaining = ticks;
  }
  return remaining;
}

// Builds the panel: the native rounded sprite twice, with a narrow inset making the border, and a countdown in the
// system-name font. A missing sprite still gives a visible frame. Null while the drawer or the HUD's font is not up
// yet; the next tick tries again.
unity::TMP_Text* CreateAnomalyPanel(HudNavigationViewController* nav)
{
  auto* helper = nav->factionInformationHelper();
  auto* name   = il2cpp::At(helper ? helper->systemName() : nullptr, 0);
  if (!unity::Alive(name))
    return nullptr;
  auto* drawer    = ScreenManager::View<HudAllianceAndNewsViewController>();
  auto* promotion = drawer ? drawer->chestPromotion() : nullptr;
  auto* chest     = promotion ? promotion->transform() : nullptr;
  auto* parent    = chest ? chest->parent() : nullptr;
  auto* object    = name->gameObject();
  auto* source    = object ? object->GetComponent<unity::TextMeshProUGUI>() : nullptr;
  auto* font      = source ? source->font() : nullptr;
  if (!parent || !font)
    return nullptr;

  // The children are owned by the hierarchy once parented; only the label is kept. The objects below are null only
  // when a Unity class is missing, which the library reports; a half-built panel then stays inactive.
  il2cpp::GcHandle panel, fill, text;
  auto*            panel_rect = unity::CreateUiObject("CommunityMod_AnomalyTimer", parent, panel);
  if (!panel_rect)
    return nullptr;
  panel_rect->Place({0.5f, 0}, {0.5f, 0}, {0.5f, 1}, {198, 38}, {0, -40});
  auto* rounded = NativeBackgroundSprite(nav);
  AddPanelImage(panel.Get<unity::GameObject>(), {0.64f, 0.34f, 0.46f, 0.85f}, rounded);

  auto* fill_rect = unity::CreateUiObject("AnomalyFill", panel_rect, fill);
  if (!fill_rect)
    return nullptr;
  AddPanelImage(fill.Get<unity::GameObject>(), {0.06f, 0.10f, 0.14f, 0.96f}, rounded);
  fill_rect->Place({0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {194, 34}, {0, 0});
  fill.Get<unity::GameObject>()->SetActive(true);

  auto* text_rect = unity::CreateUiObject("Countdown", panel_rect, text);
  auto* label     = text_rect ? text.Get<unity::GameObject>()->AddComponent<unity::TextMeshProUGUI>() : nullptr;
  if (!label)
    return nullptr;
  text_rect->Place({0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {190, 28}, {0, 0});
  label->SetFont(font);
  label->SetFontSize(22);
  label->SetAlignment(unity::TMP_Text::Alignment::Center);
  label->SetColor({0.95f, 0.83f, 0.89f, 1});
  label->SetRaycastTarget(false);
  label->SetEnableWordWrapping(false);
  text.Get<unity::GameObject>()->SetActive(true);
  anomaly_label         = il2cpp::GcHandle::Strong(label);
  anomaly_shown_seconds = -1;
  return label;
}

void AnomalyPanelFrame()
{
  anomaly_panel_shown = false;
  // View only returns a HUD that is up on a visible canvas, so nothing about it needs holding between ticks.
  auto*   nav   = (Config::Get().graphics.galactic_anomaly_timer || Config::Get().graphics.mirror_space_timer)
                          && Hub::CurrentSection() == SectionID::Navigation_System
                      ? ScreenManager::View<HudNavigationViewController>()
                      : nullptr;
  int64_t ticks = nav && Config::Get().graphics.mirror_space_timer ? MirrorRemainingTicks(nav) : 0;
  if (ticks <= 0 && nav && Config::Get().graphics.galactic_anomaly_timer && nav->CanShowGalacticAnomaly())
    ticks = AnomalyRemainingTicks(nav);
  auto* label = anomaly_label.Get<unity::TMP_Text>();
  if (!unity::Alive(label))
    label = nullptr;
  if (ticks <= 0) {
    if (auto* panel = label ? label->ParentObject() : nullptr)
      panel->SetActive(false);
    return;
  }
  if (!label && !(label = CreateAnomalyPanel(nav)))
    return;

  // TimeSpan ticks are 100 ns. Round up, so the last second shows as 1s rather than 0s.
  const auto seconds = ticks / 10'000'000 + (ticks % 10'000'000 != 0);
  if (seconds != anomaly_shown_seconds) {
    anomaly_shown_seconds = seconds;
    label->SetText(seconds >= 3600 ? fmt::format("{}h {:02}m {:02}s", seconds / 3600, seconds / 60 % 60, seconds % 60)
                                   : fmt::format("{:02}m {:02}s", seconds / 60, seconds % 60));
  }
  if (auto* panel = label->ParentObject())
    panel->SetActive(true);
  anomaly_panel_shown = true;
}

// --- Territory defense ---

// The defense panel, built like the anomaly panel under the same drawer and moved below it while that one shows.
// Never destroyed, for the same reason as the anomaly label.
auto&       defense_label = *new il2cpp::GcHandle;
auto&       defense_panel = *new il2cpp::GcHandle;
std::string defense_shown_text;
bool        defense_below_anomaly = false;
float       defense_placed_scale  = 0;

constexpr float kDefensePanelWidth  = 270;
constexpr float kDefensePanelHeight = 52;

// The drawer holding Help All / Engage / Claim, which both panels hang under. Null while it is not up.
unity::Transform* DefenseDrawerParent()
{
  auto* drawer    = ScreenManager::View<HudAllianceAndNewsViewController>();
  auto* promotion = drawer ? drawer->chestPromotion() : nullptr;
  auto* chest     = promotion ? promotion->transform() : nullptr;
  return chest ? chest->parent() : nullptr;
}

// Builds the defense panel in the drawer's own font, with the native rounded sprite when the system HUD is up to
// lend it. Null while no text in the drawer has a font yet; the next tick tries again.
unity::TMP_Text* CreateDefensePanel(unity::Transform* parent)
{
  auto* object = parent->gameObject();
  auto* source = object ? object->GetComponentInChildren<unity::TextMeshProUGUI>() : nullptr;
  auto* font   = source ? source->font() : nullptr;
  if (!font)
    return nullptr;
  auto* nav     = ScreenManager::View<HudNavigationViewController>();
  auto* rounded = nav ? NativeBackgroundSprite(nav) : nullptr;

  il2cpp::GcHandle panel, fill, text;
  auto*            panel_rect = unity::CreateUiObject("CommunityMod_DefenseTimer", parent, panel);
  if (!panel_rect)
    return nullptr;
  AddPanelImage(panel.Get<unity::GameObject>(), {0.30f, 0.52f, 0.78f, 0.85f}, rounded);

  auto* fill_rect = unity::CreateUiObject("DefenseFill", panel_rect, fill);
  if (!fill_rect)
    return nullptr;
  AddPanelImage(fill.Get<unity::GameObject>(), {0.06f, 0.10f, 0.14f, 0.96f}, rounded);
  fill_rect->Place({0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {kDefensePanelWidth - 4, kDefensePanelHeight - 4}, {0, 0});
  fill.Get<unity::GameObject>()->SetActive(true);

  auto* text_rect = unity::CreateUiObject("DefenseCountdown", panel_rect, text);
  auto* label     = text_rect ? text.Get<unity::GameObject>()->AddComponent<unity::TextMeshProUGUI>() : nullptr;
  if (!label)
    return nullptr;
  text_rect->Place({0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {kDefensePanelWidth - 10, kDefensePanelHeight - 6},
                   {0, 0});
  label->SetFont(font);
  label->SetFontSize(16);
  label->SetAlignment(unity::TMP_Text::Alignment::Center);
  label->SetColor({0.86f, 0.92f, 1, 1});
  label->SetRaycastTarget(false);
  label->SetEnableWordWrapping(false);
  text.Get<unity::GameObject>()->SetActive(true);
  defense_label = il2cpp::GcHandle::Strong(label);
  defense_panel = il2cpp::GcHandle::Strong(panel_rect);
  defense_shown_text.clear();
  // Forces the first placement below.
  defense_below_anomaly = !anomaly_panel_shown;
  defense_placed_scale  = 0;
  return label;
}

void DefensePanelFrame()
{
  const auto& graphics = Config::Get().graphics;
  const auto  text     = graphics.defense_timer
                             ? defense_schedule::DefensePanelText(graphics.loader_defenses, std::time(nullptr))
                             : std::nullopt;
  auto*       parent   = text ? DefenseDrawerParent() : nullptr;
  auto*       label    = defense_label.Get<unity::TMP_Text>();
  auto*       panel    = defense_panel.Get<unity::RectTransform>();
  if (!unity::Alive(label) || !unity::Alive(panel))
    label = nullptr;
  // Hidden without a defense or a drawer, and left behind when the HUD builds a new drawer.
  if (label && (!parent || panel->parent() != parent)) {
    if (auto* object = label->ParentObject())
      object->SetActive(false);
    if (parent)
      label = nullptr;
  }
  if (!parent)
    return;
  if (!label && !(label = CreateDefensePanel(parent)))
    return;
  panel = defense_panel.Get<unity::RectTransform>();

  if (*text != defense_shown_text) {
    defense_shown_text = *text;
    label->SetText(defense_shown_text);
  }
  if (defense_below_anomaly != anomaly_panel_shown) {
    defense_below_anomaly = anomaly_panel_shown;
    panel->Place({0.5f, 0}, {0.5f, 0}, {0.5f, 1}, {kDefensePanelWidth, kDefensePanelHeight},
                 {0, anomaly_panel_shown ? -84.0f : -40.0f});
  }
  // Scaled around its top edge, so the gap to the panel above stays.
  if (const float scale = graphics.defense_timer_scale; scale != defense_placed_scale) {
    defense_placed_scale = scale;
    panel->SetLocalScale({scale, scale, 1});
  }
  if (auto* object = label->ParentObject())
    object->SetActive(true);
}

#if _WIN32
// The smallest reminder lead already sent for each defense, keyed by system and start time.
auto& defense_reminders_sent = *new std::map<std::string, int32_t>;

void DefenseRemindersFrame()
{
  static Throttle throttle(std::chrono::seconds(5));
  const auto&     config = Config::Get();
  if (config.ui.defense_reminders.empty() || !throttle.Due())
    return;
  const auto now = std::time(nullptr);
  for (const auto& [start, defense] : defense_schedule::Upcoming(config.graphics.loader_defenses, now)) {
    const auto remaining = static_cast<int64_t>(start - now);
    const auto key       = defense->system + "@" + std::to_string(start);
    auto& sent = defense_reminders_sent.try_emplace(key, std::numeric_limits<int32_t>::max()).first->second;
    if (!defense_schedule::DueReminder(remaining, config.ui.defense_reminders, sent))
      continue;
    const auto title = "Gebietsverteidigung " + defense_schedule::Countdown(remaining);
    ShowDesktopNotification(title.c_str(),
                            defense_schedule::DefenseName(*defense) + " um " + defense_schedule::Clock(start));
    spdlog::info("[AnomalyTimer] reminded of the defense of {} {}", defense->system,
                 defense_schedule::Countdown(remaining));
  }
}
#endif

void AnomalyTimerFrame()
{
  static Throttle throttle(std::chrono::milliseconds(250));
  if (!throttle.Due())
    return;
  AnomalyPanelFrame();
  DefensePanelFrame();
#if _WIN32
  DefenseRemindersFrame();
#endif
}
} // namespace

const Feature kAnomalyTimer{"AnomalyTimer", &Config::Patches::galacticanomalytimerhooks, nullptr, AnomalyTimerFrame};

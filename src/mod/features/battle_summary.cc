// [ngo] battle_summary: when a battle report is opened, the game's own export of it is summed up in a panel next to
// the report, and once in the log ("[BattleSummary] ..."). The panel closes with the report. Unity work runs on the
// frame tick; the detours only note which report was asked for.

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

// The sizes at [ngo] fleet_bar_scale 1; the scale multiplies them all.
constexpr float kBattleSummaryFontSize   = 20;
constexpr float kBattleSummaryLineHeight = 28;
constexpr float kBattleSummaryPadding    = 18;
constexpr float kBattleSummaryWidth      = 560;
// From the screen's right edge.
constexpr float kBattleSummaryMargin = 32;

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
  const float          height = (static_cast<float>(lines) + 0.3f) * kBattleSummaryLineHeight * scale + 2 * padding;
  const unity::Vector2 size{kBattleSummaryWidth * scale, height};
  panel->Place({1, 0.5f}, {1, 0.5f}, {1, 0.5f}, size, {-kBattleSummaryMargin, 0});
  if (auto* text = battle_summary_text.Get<unity::RectTransform>(); unity::Alive(text))
    text->Place({0, 0}, {1, 1}, {0.5f, 0.5f}, {-2 * padding, -2 * padding}, {0, 0});
  label->SetFontSize(kBattleSummaryFontSize * scale);
}

void BattleSummaryFrame()
{
  static Throttle throttle(std::chrono::milliseconds(100));
  if (!throttle.Due())
    return;
  const auto& config = Config::Get();
  auto*       view   = config.ui.battle_summary ? unity::Newest<BattleReportViewController>() : nullptr;
  const bool  shown  = view && CanvasController::OnScreen(view);
  if (!shown && battle_summary_view_shown)
    battle_summary_current.reset();
  battle_summary_view_shown = shown;
  auto* parent              = shown && battle_summary_current ? unity::CanvasRoot(view->transform()) : nullptr;

  auto* label = battle_summary_label.Get<unity::TMP_Text>();
  auto* panel = battle_summary_panel.Get<unity::RectTransform>();
  if (!unity::Alive(label) || !unity::Alive(panel))
    label = nullptr;
  // Hidden without a report, and left behind when the report builds a new canvas.
  if (label && (!parent || panel->parent() != parent)) {
    if (auto* object = panel->gameObject())
      object->SetActive(false);
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
}
void ResetBattleSummary()
{
  battle_summary_pending = 0;
  battle_summary_current.reset();
  battle_summary_view_shown = false;
}
} // namespace

const Feature kBattleSummary{"BattleSummary", nullptr, InstallBattleSummary, BattleSummaryFrame, ResetBattleSummary};

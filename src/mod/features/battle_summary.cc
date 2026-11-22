// [ngo] battle_summary, first step: when a battle report is opened, the game's own export of it is summed up in the
// log ("[BattleSummary] ..."), to compare with the report before the panel is built.

#include "config.h"
#include "features/battle_summary.h"
#include "features/features.h"

#include "game/battle.h"

#include <il2cpp/hook.h>
#include <il2cpp/il2cpp.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>
#include <unordered_set>

namespace
{
// Reports already summed up this session.
auto& battle_summary_done = *new std::unordered_set<int64_t>;

void SummarizeBattleReport(BattleResultsManager* manager, int64_t id, const char* trigger)
{
  if (!manager || battle_summary_done.contains(id))
    return;
  auto* report = manager->Report(id);
  if (!report) {
    spdlog::info("[BattleSummary] report {} ({}): not fetched yet", id, trigger);
    return;
  }
  battle_summary_done.insert(id);
  auto*      target = manager->TargetData(report);
  auto*      export_text = target ? manager->ExportText(target) : nullptr;
  const auto text        = export_text ? il2cpp::ToUtf8(export_text) : std::string();
  if (text.empty()) {
    spdlog::info("[BattleSummary] report {} ({}): the export is empty (target data {})", id, trigger,
                 target ? "found" : "missing");
    return;
  }
  const auto summary = battle_summary::Parse(text);
  if (!summary) {
    spdlog::info("[BattleSummary] report {} ({}): the export does not parse; it starts with: {}", id, trigger,
                 text.substr(0, 200));
    return;
  }
  spdlog::info("[BattleSummary] report {} ({}): {}", id, trigger, battle_summary::Describe(*summary));
  for (const auto& section : summary->unread)
    spdlog::info("[BattleSummary] report {}: a section not read yet: {}", id, section);
}

void BattleResultsManager_ShowBattleReport_Hook(auto original, BattleResultsManager* _this, il2cpp::Object* header)
{
  original(_this, header);
  if (header)
    SummarizeBattleReport(_this, header->Get<"ID", int64_t>(), "opened");
}

void BattleResultsManager_OnSuccessFetchBattleReport_Hook(auto original, BattleResultsManager* _this, int64_t id)
{
  original(_this, id);
  SummarizeBattleReport(_this, id, "fetched");
}

bool InstallBattleSummary()
{
  il2cpp::HookSet hooks("BattleSummary");
  IL2CPP_STAGE(hooks, BattleResultsManager::Class::Method("ShowBattleReport", 1),
               BattleResultsManager_ShowBattleReport_Hook);
  IL2CPP_STAGE(hooks, BattleResultsManager::Class::Method("OnSuccessFetchBattleReport", 1),
               BattleResultsManager_OnSuccessFetchBattleReport_Hook);
  return hooks.Commit();
}
} // namespace

const Feature kBattleSummary{"BattleSummary", nullptr, InstallBattleSummary};

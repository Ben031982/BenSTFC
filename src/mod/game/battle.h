#pragma once

// Battle reports: the manager that fetches and keeps them, and the game's own export of one as tab-separated text.

#include <il2cpp/il2cpp.h>

#include <cstdint>
#include <functional>

struct BattleResultsManager : il2cpp::Object {
  using Class = il2cpp::Class<"Assembly-CSharp", "Digit.Prime.BattleResults", "BattleResultsManager">;

  // The fetched report (BattleResultData), or null while it is not fetched.
  il2cpp::Object* Report(int64_t id)
  {
    il2cpp::Object* report = nullptr;
    return Call<"TryGetBattleReport", bool>(id, std::ref(report)) ? report : nullptr;
  }
  // What the export reads (BattleTargetData).
  il2cpp::Object* TargetData(il2cpp::Object* report)
  { return Call<"RetrieveBattleTargetDataFromBattleResult", il2cpp::Object*>(report); }
  // The report's export as tab-separated text, in the game's language, as the export button saves it.
  Il2CppString* ExportText(il2cpp::Object* target_data)
  {
    auto* utility = Field<"_exportUtility", il2cpp::Object*>();
    return utility ? utility->Call<"ExportToCSV", Il2CppString*>(target_data) : nullptr;
  }
};

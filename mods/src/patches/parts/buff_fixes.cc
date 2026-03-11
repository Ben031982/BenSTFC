#include "config.h"
#include "errormsg.h"
#include "prime_types.h"

#include <il2cpp/il2cpp_helper.h>
#include <prime/IBuffComparer.h>
#include <prime/IBuffData.h>

#include <spud/detour.h>

static bool BuffService_IsBuffConditionMet(auto original, void* _this, BuffCondition currentCondition,
                                           IBuffComparer *buffComparer, IBuffData *buffToCompare,
                                           bool excludeFactionBuffs, bool isAllianceLoyalty)
{
  switch (currentCondition) {
    case BuffCondition::CondSelfAtStation:
      return false;

    default:
      break;
  }

  return original(_this, currentCondition, buffComparer, buffToCompare, excludeFactionBuffs, isAllianceLoyalty);
}

void InstallBuffFixHooks()
{
  if (Config::Get().use_out_of_dock_power) {
    auto buffHelper =
        il2cpp_get_class_helper("Digit.Client.PrimeLib.Runtime", "Digit.PrimeServer.Services", "BuffService");
    if (!buffHelper.isValidHelper()) {
      ErrorMsg::MissingHelper("Services", "BuffService");
    } else {
      if (const auto ptr = buffHelper.GetMethod("IsBuffConditionMet"); ptr == nullptr) {
        ErrorMsg::MissingMethod("BuffService", "IsBuffConditionMet");
      } else {
        SPUD_STATIC_DETOUR(ptr, BuffService_IsBuffConditionMet);
      }
    }
  }
}

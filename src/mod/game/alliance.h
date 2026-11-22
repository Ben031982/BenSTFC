#pragma once

// The player's own alliance: its profile, its admiral and its members' ranks.

#include "app.h"

#include <il2cpp/il2cpp.h>

#include <cstdint>
#include <functional>

// A member of the alliance with their rank.
struct AllianceRank : il2cpp::Object {
  using Class = il2cpp::Class<"Digit.Client.PrimeLib.Runtime", "Digit.PrimeServer.Models", "AllianceRank">;

  int32_t Rank()
  { return Get<"Rank", int32_t>(); }
};

struct AllianceProfile : il2cpp::Object {
  using Class = il2cpp::Class<"Digit.Client.PrimeLib.Runtime", "Digit.PrimeServer.Models", "AllianceProfile">;

  // The admiral.
  Il2CppString* LeaderUserId()
  { return Get<"LeaderUserId", Il2CppString*>(); }
  // Null when the user is no member, or the members are not known yet.
  AllianceRank* Member(Il2CppString* user_id)
  {
    AllianceRank* member = nullptr;
    return Call<"TryGetMember", bool>(user_id, std::ref(member)) ? member : nullptr;
  }
};

struct AllianceManager : MonoSingleton<AllianceManager> {
  using Class = il2cpp::Class<"Assembly-CSharp", "Digit.Prime.Alliances", "AllianceManager">;

  AllianceProfile* GetLocalAllianceProfile()
  { return Call<"GetLocalAllianceProfile", AllianceProfile*>(); }
};

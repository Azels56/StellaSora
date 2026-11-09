#pragma once
// Masked accessors for the game class and method names the payload looks up.
//
// Every call site used to repeat the literal, so .rdata carried "ILRuntimeAPI",
// "HitActors", "AdventureModuleHelper" and friends in the clear — enough to name
// both the game and the cheat from a string scan.
//
// The Enc has to be a named constexpr object. Passed inline to the decoder it is
// a temporary in a non-constant context, and MSVC then emits the plaintext and
// masks it at run time, which defeats the point.
#include "str_obf.h"

namespace names {

#define NAME_FN(fn, lit)                     \
    inline const char* fn() {                \
        static constexpr auto e = OBF(lit);  \
        static const auto d = obf::Decoded(e); \
        return d;                            \
    }

NAME_FN(ILRuntimeAPI,          "ILRuntimeAPI")
NAME_FN(AdventureModuleHelper, "AdventureModuleHelper")
NAME_FN(AdventureActor,        "AdventureActor")
NAME_FN(PlayerActor,           "PlayerAdventureActor")
NAME_FN(PlayerSkillCd,         "PlayerSkillCd")
NAME_FN(CanUseCD,              "CanUseCD")
NAME_FN(HitActors,             "HitActors")
NAME_FN(HitActor,              "HitActor")
NAME_FN(DirectDamage,          "DirectDamage")
NAME_FN(ConfrontActors,        "GetConfrontFactionActors")
NAME_FN(PlayerActorList,       "GetAllPlayerActorAndMinion")
NAME_FN(CurrentMonsters,       "GetCurrentMonsters")
NAME_FN(ActorEffectManage,     "ActorEffectManage")
NAME_FN(MonsterActor,          "MonsterAdventureActor")

#undef NAME_FN

} // namespace names

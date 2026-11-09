// Gameplay features: actor lists, attributes, skills, aura, cooldowns, config.
#include "features.h"
#include "il2cpp_api.h"
#include "luau_bridge.h"
#include "overlay.h"
#include "str_obf.h"
#include "names.h"
#include "MinHook.h"
#include <cstdio>
#include <unordered_map>
#include <unordered_set>


// ----------------------------------------------------------------- game state
struct PlayerInfo {
    void* actor = nullptr;
    void* health = nullptr;
    int64_t hp = 0;
    int64_t hpMax = 0;
    bool alive = false;
};

static bool GetHealth(void* actor, void*& health) {
    void* klass = g_i2.object_get_class(actor);
    size_t off = FieldOffset(klass, "actorHealthInfo");
    if (off == SIZE_MAX) return false;
    health = *(void**)((uint8_t*)actor + off);
    return health != nullptr;
}

static bool IsMainPlayer(void* actor) {
    void* klass = g_i2.object_get_class(actor);
    size_t off = FieldOffset(klass, "isAssist");
    if (off == SIZE_MAX) return true;
    return *(uint8_t*)((uint8_t*)actor + off) == 0;
}

static PlayerInfo ReadPlayer(void* actor) {
    PlayerInfo p{};
    p.actor = actor;
    if (!GetHealth(actor, p.health)) return p;
    void* hk = g_i2.object_get_class(p.health);
    Invoke(hk, "get_hp", 0, p.health, nullptr, &p.hp);
    Invoke(hk, "get_hpMax", 0, p.health, nullptr, &p.hpMax);
    bool alive = false;
    Invoke(hk, "get_isAlive", 0, p.health, nullptr, nullptr, &alive);
    p.alive = alive;
    return p;
}


CheatState g_cheat;
volatile LONG g_stage = 0;

// Every pass inside one tick asks the same two questions ("are we in a fight",
// "who is the main player"). Both are interpreter round-trips, and each one
// allocates a managed List on the way, so the answers are memoised for the
// duration of a single tick rather than recomputed per pass.
static ULONGLONG g_tickGen = 1;
static ULONGLONG g_battleGen = 0;
static bool      g_battleCached = false;
static ULONGLONG g_mainGen = 0;
static void*     g_mainCached = nullptr;


static std::unordered_map<void*, int64_t> g_runSpdBase;
static std::unordered_map<void*, int64_t> g_walkSpdBase;
static std::unordered_map<void*, int64_t> g_atkSpdBase;
// ActorInfo keeps a paired origin/current speed. The game moves the actor from
// the current value and does not derive it from the origin, so writing only
// the origin changed nothing (origin=35 while cur stayed 7).
static std::unordered_map<void*, int64_t> g_runSpdCur;
static std::unordered_map<void*, int64_t> g_walkSpdCur;
// True while a speed field is scaled. Without this the tick stops calling
// ApplyStats as soon as game speed returns to 1, so the restore never runs and
// the character keeps moving at the old multiplier.
static bool g_speedDirty = false;
float g_appliedTimeScale = 1.0f;
int64_t g_lastEnergy = 0;
double g_dbgRunSpeed = 0.0;
int64_t g_auraHpBefore = 0, g_auraHpAfter = 0;
int g_auraFallbackHits = 0;
int g_auraHitsLast = 0;
int g_lastPlayers = 0;
static uint64_t g_playerSig = 0;
int64_t g_lastHp = 0;
int64_t g_lastMax = 0;
bool g_lastAlive = false;


static void* AttrEntries(void* actor) {
    void* attr = FieldPtr(actor, "attributeList");
    if (!attr) return nullptr;
    void* entries = FieldPtr(attr, "entries");
    if (!entries || ArrayLength(entries) < 47) return nullptr;
    return entries;
}

static int AttrElemSize(void* entries) {
    int elem = g_i2.array_element_size ? g_i2.array_element_size(g_i2.object_get_class(entries)) : 0;
    return elem > 0 ? elem : 0x30;
}

// Multipliers go through AttributeList.SetAmendValue(effectAttributeType,
// parameterType, FDP). The raw entries array is NOT indexed by enum and never
// affected damage, which is why the old write did nothing.

static bool AmendList(void* listObj, int attr, int paramType, double value) {
    if (!listObj) return false;
    int32_t t = attr;
    int32_t p = paramType;
    int64_t raw = (int64_t)(value * 16777216.0);
    void* args[3] = { &t, &p, &raw };
    return Invoke(g_i2.object_get_class(listObj), "SetAmendValue", 3, listObj, args);
}

static bool ReadAttrValue(void* listObj, void* klass, int attr, int64_t* out) {
    if (!listObj || !klass) return false;
    int32_t t = attr;
    void* a1[1] = { &t };
    return Invoke(klass, "GetAttributeValue", 1, listObj, a1, out);
}

// Raw writer: pushes one amend onto the actor's main list and onto every
// SkillInfo snapshot. Skill damage reads its own copy, not the live one.
static void SetAmend(void* actor, int attr, int paramType, double value) {
    void* main = FieldPtr(actor, "attributeList");
    if (!main) {
        static bool once = false;
        if (!once) { once = true; LogF("amend: actor %p has no attributeList", actor); }
        return;
    }
    if (!AmendList(main, attr, paramType, value)) {
        static bool once = false;
        if (!once) { once = true; LogF("amend: SetAmendValue(%d,%d) failed", attr, paramType); }
    }
    void* psc = FieldPtr(actor, "playerSkillCd");
    if (!psc) return;
    void* infos = FieldPtr(psc, "<skillInfos>k__BackingField");
    int n = ListSize(infos);
    for (int i = 0; i < n; i++) {
        void* si = ListItem(infos, i);
        if (!si) continue;
        AmendList(FieldPtr(si, "attributeList"), attr, paramType, value);
        AmendList(FieldPtr(si, "specialAttributeList"), attr, paramType, value);
    }
}

// SetAmendValue is additive, not a scale factor. The calibration probe on
// ATTR_MAXHP read back exactly `before + written`, so writing (mult - 1) as a
// "percentage" landed as a flat +2 ATK against a pool in the thousands — about
// 0.1%, which is why the slider looked dead. Convert the multiplier into a
// delta against the value a list actually holds; AmendListMult does that from
// the delta we wrote last tick.

// Per list object: the value it held before we ever touched it. The target is
// always `base * mult`, so the amend can never be derived from a value that
// already contains a previous amend - that feedback is what compounded x2 into
// hundreds of billions and left 1x still multiplied.
struct AmendRec { int64_t base; int tries; int lastMult; };
static std::unordered_map<void*, AmendRec> g_amendRec;

// SetAmendValue's write semantics are not documented and the two plausible
// models need opposite writes: replacing the amend means writing the target
// directly, accumulating means writing the difference. Rather than guess, drive
// the value and read it back - the observed change says which model is live,
// and the correction below fixes it either way.
static bool DriveAmend(void* listObj, void* lk, int attr, int64_t base, int64_t want) {
    int64_t cur = 0;
    if (!ReadAttrValue(listObj, lk, attr, &cur)) return true;
    if (cur == base + want) return true;        // already at target

    const int64_t amend = cur - base;           // live amend, as observed
    const int64_t x = want - amend;             // what an accumulating setter needs
    if (x == 0) return true;

    if (!AmendList(listObj, attr, PARAM_PERCENTAGE, (double)x / 16777216.0)) return true;
    int64_t after = 0;
    if (!ReadAttrValue(listObj, lk, attr, &after)) return true;
    if (after == base + want) return true;      // accumulate model (or amend was 0)

    // Replace model: the write became the whole amend. Write the target instead.
    AmendList(listObj, attr, PARAM_PERCENTAGE, (double)want / 16777216.0);
    return false;
}

static void AmendListMult(void* listObj, int attr, int mult, std::unordered_set<void*>* seen) {
    if (!listObj) return;
    // The skill snapshot can alias the actor's own list (or another skill's).
    // Writing twice on the same object doubles the result: x2 landed as 3x.
    if (seen && !seen->insert(listObj).second) return;

    void* lk = g_i2.object_get_class(listObj);
    auto it = g_amendRec.find(listObj);

    if (mult <= 1) {
        if (it == g_amendRec.end()) return;
        DriveAmend(listObj, lk, attr, it->second.base, 0);
        // Only forget the base once the value is actually back. Forgetting it
        // while the amend is still on the list would make the next enable read
        // an already-inflated value as the base.
        int64_t cur = 0;
        if (ReadAttrValue(listObj, lk, attr, &cur) && cur == it->second.base) {
            g_amendRec.erase(it);
        }
        return;
    }

    if (it == g_amendRec.end()) {
        int64_t cur = 0;
        if (!ReadAttrValue(listObj, lk, attr, &cur)) return;
        if (cur <= 0) return;
        it = g_amendRec.emplace(listObj, AmendRec{ cur, 0, mult }).first;
    }

    // A new slider value deserves a fresh attempt: the give-up counter exists
    // to stop a write that never lands, not to lock the list out for the rest
    // of the session.
    if (it->second.lastMult != mult) {
        it->second.lastMult = mult;
        it->second.tries = 0;
    }

    // If a write never takes effect the value cannot be driven, and retrying
    // every tick against a setter we do not understand is how a multiplier
    // runs away. Give up on that list instead.
    if (it->second.tries >= 8) return;

    const int64_t want = (int64_t)((double)it->second.base * (double)(mult - 1));
    if (DriveAmend(listObj, lk, attr, it->second.base, want)) it->second.tries = 0;
    else it->second.tries++;
}

static void SetAmendMult(void* actor, int attr, int mult) {
    if (mult < 1) mult = 1;
    void* main = FieldPtr(actor, "attributeList");
    if (!main) return;

    std::unordered_set<void*> seen;
    AmendListMult(main, attr, mult, &seen);

    // Each SkillInfo keeps its own attribute snapshot, and damage reads that
    // copy — so it gets its own captured base and its own delta.
    void* psc = FieldPtr(actor, "playerSkillCd");
    void* infos = psc ? FieldPtr(psc, "<skillInfos>k__BackingField") : nullptr;
    const int n = ListSize(infos);
    for (int i = 0; i < n; i++) {
        void* si = ListItem(infos, i);
        if (!si) continue;
        void* a = FieldPtr(si, "attributeList");
        void* b = FieldPtr(si, "specialAttributeList");
        AmendListMult(a, attr, mult, &seen);
        AmendListMult(b, attr, mult, &seen);
    }
}

static void ApplyAttrMult(void* actor, int idx, int mult) {
    SetAmendMult(actor, idx, mult);
}

static void ApplyAttrAbsolute(void* actor, int idx, double value) {
    SetAmend(actor, idx, PARAM_ABSOLUTE, value);
}


static int64_t FdpFromInt(int64_t v) { return v << 24; }

// PlayerSkillCd: dicSkillEnergy-less ultimate gauge. GetTotalEnergy() is the cap.
static int64_t FillEnergy(void* psc) {
    if (!psc) return 0;
    void* k = g_i2.object_get_class(psc);
    int64_t total = 0;
    if (!Invoke(k, "GetTotalEnergy", 0, psc, nullptr, &total)) return 0;
    if (total <= 0) total = 1000;
    if (total > 1000000) total = 1000000;
    int64_t raw = FdpFromInt(total);
    void* par[1] = { &raw };
    Invoke(k, "SetEnergy", 1, psc, par);
    g_lastEnergy = total;
    return total;
}

// SkillInfo state that gates re-use: charges, resume timer, lock flags.
static void ForceSkillReadyList(void* psc) {
    if (!psc) return;
    void* infos = FieldPtr(psc, "<skillInfos>k__BackingField");
    int n = ListSize(infos);
    for (int i = 0; i < n; i++) {
        void* si = ListItem(infos, i);
        if (!si) continue;
        uint8_t* b = (uint8_t*)si;
        b[0x32] = 0;                 // beginResume
        b[0x33] = 0;                 // resuming
        b[0x40] = 0;                 // _locking
        *(int64_t*)(b + 0x48) = 0;   // currentUseInterval (FP)
        int32_t total = *(int32_t*)(b + 0x60);
        if (total > 0 && total < 1000) *(int32_t*)(b + 0x58) = total; // currentSectionAmount
        *(int64_t*)(b + 0x78) = 0;   // currentResumeTime (FP)
    }
}

static void ResetPlayerSkills(void* psc, bool energy) {
    if (!psc) return;
    void* k = g_i2.object_get_class(psc);
    uint8_t yes = 1;
    void* par[1] = { &yes };
    Invoke(k, "SetFastResume", 1, psc, par);
    Invoke(k, "ResetAllSkillCD", 0, psc, nullptr);
    for (int slot = 1; slot <= 5; slot++) {
        int32_t s = slot;
        void* p[1] = { &s };
        Invoke(k, "UnLockSkillCD", 1, psc, p);
        Invoke(k, "ResetSkillCD", 1, psc, p);
    }
    ForceSkillReadyList(psc);
    if (energy) FillEnergy(psc);
}

// Menace Arena and other modes only expose hostiles through the faction API,
// never through a fixed "MonsterAdventureActor" class.
static void* FactionActors(void* attacker, const char* method) {
    void* api = FindClassByName(names::ILRuntimeAPI());
    if (!api || !attacker) return nullptr;
    uint8_t flag = 0;
    void* params[2] = { attacker, &flag };
    return InvokeStaticList(api, method, 2, params);
}

static bool IsPlayerActorClass(void* actor) {
    if (!actor) return false;
    void* k = g_i2.object_get_class(actor);
    if (!k) return false;
    const char* n = g_i2.class_get_name(k);
    return n && strstr(n, names::PlayerActor()) != nullptr;
}

// FindObjectsOfType crashes under the hotfix runtime; the game's own actor list
// is the reliable source in every mode, including Menace Arena.
static void* PlayerActorList() {
    void* api = FindClassByName(names::ILRuntimeAPI());
    if (!api) return nullptr;
    uint8_t flag = 0;
    void* params[1] = { &flag };
    return InvokeStaticList(api, names::PlayerActorList(), 1, params);
}

static void* FindMainPlayer() {
    if (g_mainGen == g_tickGen) return g_mainCached;
    void* found = nullptr;
    void* list = PlayerActorList();
    int n = ListSize(list);
    for (int i = 0; i < n; i++) {
        void* a = ListItem(list, i);
        if (!IsPlayerActorClass(a)) continue;
        if (IsMainPlayer(a)) { found = a; break; }
    }
    g_mainCached = found;
    g_mainGen = g_tickGen;
    return found;
}

static int64_t ReadHp(void* health) {
    int64_t hp = 0;
    Invoke(g_i2.object_get_class(health), "get_hp", 0, health, nullptr, &hp);
    return hp;
}


static bool InBattle();

// Some arena enemies keep an immunity/lock state that refuses normal damage.
// The game's own GM order is the only path that removes them cleanly.
static bool KillViaGM(void* actor) {
    if (!actor || g_cheat.safeMode) return false;
    int64_t id = 0;
    void* ak = g_i2.object_get_class(actor);
    if (!Invoke(ak, "get_Id", 0, actor, nullptr, &id) || id == 0) {
        size_t off = FieldOffset(ak, "Id");
        if (off == SIZE_MAX) return false;
        id = *(int64_t*)((uint8_t*)actor + off);
    }
    if (id == 0) return false;
    void* helper = FindClassByName(names::AdventureModuleHelper());
    if (!helper) return false;
    void* arg[1] = { &id };
    if (Invoke(helper, "Lua2CSharp_GMOrder_KillMonstersByActorId", 1, nullptr, arg)) return true;
    return Invoke(helper, "Lua2CSharp_GMOrder_KillMonstersById", 1, nullptr, arg);
}


static bool GMKillMonsters(int maxKills = 3) {
    if (g_cheat.safeMode) return false;
    void* helper = FindClassByName(names::AdventureModuleHelper());
    if (!helper) return false;
    uint8_t flag = 0;
    void* params[1] = { &flag };
    void* list = InvokeStaticList(helper, names::CurrentMonsters(), 0, nullptr);
    int n = ListSize(list);
    bool any = false;
    int done = 0;
    for (int i = 0; i < n && done < maxKills; i++) {
        int64_t id = ListLong(list, i);
        if (id == 0) continue;
        void* arg[1] = { &id };
        if (Invoke(helper, "Lua2CSharp_GMOrder_KillMonstersById", 1, nullptr, arg)) { any = true; done++; }
    }
    return any;
}

// Per-actor cooldown: killing ten enemies in one tick makes the game hitch, and
// re-hitting a corpse that is mid-death keeps the wave busy.
static std::unordered_map<void*, ULONGLONG> g_auraUntil;

static bool AuraReady(void* target, ULONGLONG now) {
    auto it = g_auraUntil.find(target);
    if (it != g_auraUntil.end() && it->second > now) return false;
    if (g_auraUntil.size() > 256) g_auraUntil.clear();
    g_auraUntil[target] = now + 350;
    return true;
}

// floater=false skips the floating damage numbers: each one costs a Camera
// lookup plus a WorldToScreenPoint, which dominated a mass sweep. cooldown=false
// bypasses the aura's 350 ms per-target lock, so an explicit Kill All does not
// silently skip everything the aura touched in the last third of a second.
static int DamageActorList(void* attacker, void* list, int damage, bool lethal, int maxTargets,
                           bool floater = true, bool cooldown = true) {
    if (!attacker || !list) return 0;
    void* api = FindClassByName(names::ILRuntimeAPI());
    if (!api) return 0;
    void* empty = g_i2.string_new ? g_i2.string_new("") : nullptr;
    ULONGLONG now = GetTickCount64();
    int hits = 0;
    int n = ListSize(list);
    for (int i = 0; i < n && hits < maxTargets; i++) {
        void* target = ListItem(list, i);
        if (!target || target == attacker) continue;
        void* health = nullptr;
        if (!GetHealth(target, health)) continue;
        void* hk = g_i2.object_get_class(health);
        bool alive = false;
        Invoke(hk, "get_isAlive", 0, health, nullptr, nullptr, &alive);
        if (!alive) continue;
        int64_t before = ReadHp(health);
        if (before <= 0) continue;
        if (cooldown && !AuraReady(target, now)) continue;

        // DirectDamage is int32; the cap is more than enough for one hit.
        int32_t dmg = lethal ? 2000000000 : (damage > 2000000000 ? 2000000000 : damage);
        uint8_t f0 = 0, f1 = 0, f2 = 0;
        Invoke8(api, names::DirectDamage(), nullptr, attacker, target, &dmg, empty, empty, &f0, &f1, &f2);
        int64_t after = ReadHp(health);
        if (i == 0) { g_auraHpBefore = before; g_auraHpAfter = after; }
        if (floater) ShowDamageNumber(target, (long long)(lethal ? before : dmg));
        if (lethal && after > 0) {
            // Survived an int32 hit: subtract a multiple of Max HP instead.
            int64_t pct = -100;
            void* pctArg[1] = { &pct };
            Invoke(hk, "ChangeHpByMaxHp", 1, health, pctArg);
            after = ReadHp(health);
            g_auraFallbackHits++;
            if (after > 0) KillViaGM(target);
        } else if (!lethal && after >= before) {
            int64_t next = 0;
            void* hpArg[1] = { &next };
            Invoke(hk, "ChangeHpByMaxHp", 1, health, hpArg);
        }
        hits++;
    }
    return hits;
}

static void ArenaAuraTick() {
    if (!g_cheat.aura || !g_i2.ready || !InBattle()) return;
    EnsureThread();
    g_stage = 110; void* attacker = FindMainPlayer();
    if (!attacker) return;
    g_stage = 120; void* list = FactionActors(attacker, names::ConfrontActors());
    g_stage = 130; int hits = DamageActorList(attacker, list, g_cheat.auraDmg, g_cheat.auraLethal, g_cheat.auraLethal ? 3 : 12);
    if (g_cheat.auraLethal) {
        static ULONGLONG lastSweep = 0;
        ULONGLONG sweepNow = GetTickCount64();
        if (hits == 0 && sweepNow - lastSweep >= 1500) { lastSweep = sweepNow; GMKillMonsters(); }
    }
    static ULONGLONG lastLog = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastLog >= 2000) {
        lastLog = now;
        LogF("aura hits=%d targets=%d hp %lld -> %lld fallbacks=%d dmg=%d",
            hits, ListSize(list), (long long)g_auraHpBefore, (long long)g_auraHpAfter,
            g_auraFallbackHits, g_cheat.auraLethal ? -1 : g_cheat.auraDmg);
    }
}

bool KillMobs() {
    if (!g_i2.ready) { if (!ResolveExports()) return false; }
    EnsureThread();

    // The GM order is one invoke per monster and clears immune or locked actors
    // the damage sweep bounces off. It was capped at three kills because the
    // aura fallback only ever wanted a trickle; for an explicit Kill All that
    // cap is why a full wave took seconds to fall over. 4096 means "all of
    // them" without an unbounded loop if the game ever returns a bad list.
    const bool gmAny = GMKillMonsters(4096);

    void* attacker = FindMainPlayer();
    if (!attacker) return gmAny;
    void* list = FactionActors(attacker, names::ConfrontActors());
    if (!list) return gmAny;

    // Fallback sweep for the arena actors the GM order does not own. Numbers
    // and the aura cooldown are both off here: the floaters cost a camera
    // projection per target, and the cooldown would skip anything the aura hit
    // within the last 350 ms, which made the click look like it did nothing.
    bool any = DamageActorList(attacker, list, 2000000000, true, 256, false, false) > 0;

    // Per-actor GM only for survivors of the sweep, and only when safe mode is
    // off — KillViaGM returns false immediately in safe mode, so the pass would
    // be pure cost.
    if (!g_cheat.safeMode) {
        const int n = ListSize(list);
        for (int i = 0; i < n; i++) {
            void* t = ListItem(list, i);
            if (!t || t == attacker) continue;
            void* health = nullptr;
            if (GetHealth(t, health)) {
                bool alive = false;
                Invoke(g_i2.object_get_class(health), "get_isAlive", 0, health, nullptr, nullptr, &alive);
                if (!alive) continue;
            }
            any = KillViaGM(t) || any;
        }
    }
    return gmAny || any;
}


// ------------------------------------------------------- guard and toughness
// MonsterToughnessInfo backs the yellow guard bar. Its value is not a plain
// field (only _isToughnessBroken / _isShowToughness are), it lives in the
// component's attribute list, so set_toughness is the only write path.
int g_toughMonsters = 0;

static void* InvokeObj(void* klass, const char* name, int argc, void* obj, void** params) {
    void* m = Method(klass, name, argc);
    if (!m) return nullptr;
    void* exc = nullptr;
    void* ret = g_i2.runtime_invoke(m, obj, params, &exc);
    return exc ? nullptr : ret;
}

// Width of a value type from its metadata name. TrueSync.FP is a single long
// behind a struct, so it shares the 8-byte branch.
static int ValueSize(void* type) {
    if (!type || !g_i2.type_get_name) return 0;
    const char* n = g_i2.type_get_name(type);
    if (!n) return 0;
    if (strstr(n, "Int64") || strstr(n, "UInt64") || strstr(n, "Double") || strstr(n, "FP")) return 8;
    if (strstr(n, "Int32") || strstr(n, "UInt32") || strstr(n, "Single")) return 4;
    if (strstr(n, "Int16") || strstr(n, "UInt16")) return 2;
    return 1;
}

static int ParamSize(void* m, int i) {
    if (!m || !g_i2.method_get_param) return 0;
    return ValueSize(g_i2.method_get_param(m, (uint32_t)i));
}

// runtime_invoke boxes value returns: the payload sits past the object header.
static int64_t ReadBoxed(void* ret, int size) {
    if (!ret) return 0;
    uint8_t* v = (uint8_t*)ret + 0x10;
    if (size == 8) return *(int64_t*)v;
    if (size == 4) return *(int32_t*)v;
    if (size == 2) return *(int16_t*)v;
    return *v;
}

static void SetToughness(void* tough, int64_t value) {
    void* m = Method(g_i2.object_get_class(tough), "set_toughness", 1);
    if (!m) return;
    void* exc = nullptr;
    if (ParamSize(m, 0) == 4) {
        int32_t v = (int32_t)value;
        void* p[1] = { &v };
        g_i2.runtime_invoke(m, tough, p, &exc);
    } else {
        int64_t v = value;
        void* p[1] = { &v };
        g_i2.runtime_invoke(m, tough, p, &exc);
    }
}

static int64_t GetToughness(void* tough) {
    void* m = Method(g_i2.object_get_class(tough), "get_toughness", 0);
    if (!m) return 0;
    void* exc = nullptr;
    void* ret = g_i2.runtime_invoke(m, tough, nullptr, &exc);
    if (exc) return 0;
    return ReadBoxed(ret, ValueSize(g_i2.method_get_return_type ? g_i2.method_get_return_type(m) : nullptr));
}

static void StripShield(void* actor) {
    void* sh = FieldPtr(actor, "actorShield");
    if (!sh) return;
    // int32 slot: a bool read of a 4-byte zero is still false, while a 1-byte
    // slot read as int32 can pick up neighbour bytes and read as true.
    int32_t fx = 0;
    void* p[1] = { &fx };
    Invoke(g_i2.object_get_class(sh), "RemoveAllShield", 1, sh, p);
}

// Runs every tick while either flag is on: new waves spawn constantly, so a
// one-shot pass would only cover the first enemies.
//
// Two rules keep this from breaking the enemy:
//   * never call CheckToughnessBroken(). Forcing the break state by hand left
//     the monster stuck inside the break timeline and it could no longer die.
//     Zeroing the guard is enough: the next real hit runs the game's own
//     DamageToToughness -> CheckToughnessBroken path, unchanged.
//   * act on an actor at most every 2s. Re-stripping the shield every tick
//     spams shieldChangedEvent and the enemy's damage gate never settles.
static std::unordered_map<void*, ULONGLONG> g_guardUntil;

static bool GuardReady(void* target, ULONGLONG now) {
    auto it = g_guardUntil.find(target);
    if (it != g_guardUntil.end() && it->second > now) return false;
    if (g_guardUntil.size() > 256) g_guardUntil.clear();
    g_guardUntil[target] = now + 2000;
    return true;
}

static void ToughnessPass() {
    if (!InBattle()) return;
    void* attacker = FindMainPlayer();
    if (!attacker) return;
    void* list = FactionActors(attacker, names::ConfrontActors());
    int n = ListSize(list);
    const ULONGLONG now = GetTickCount64();
    int seen = 0;
    for (int i = 0; i < n; i++) {
        void* t = ListItem(list, i);
        if (!t || t == attacker) continue;
        seen++;
        if (!GuardReady(t, now)) continue;

        // Shields are a separate component from the guard bar: stripping them
        // does not touch toughness, so the enemy keeps its normal behaviour.
        if (g_cheat.noShield) StripShield(t);
        if (!g_cheat.instantBreak) continue;

        void* tough = InvokeObj(g_i2.object_get_class(t), "get_monsterToughnessInfo", 0, t, nullptr);
        if (!tough) continue;
        // One FP unit, not zero: toughness feeds a percentage that the game
        // divides by, and an exact zero threw out of the enemy's update loop.
        if (GetToughness(tough) <= 1) continue;
        SetToughness(tough, 1);
    }
    g_toughMonsters = seen;
}

// ---------------------------------------------------------------- multi-hit
// Repeating the hit at HitActors is not possible: its detour kills the process
// even when it only forwards (see the note above the old install site). Instead
// a landed hit is detected from the enemy's own HP and replayed through the
// game's direct-damage entry.
//
// A drop in an enemy's HP is the player's hit landing, and the size of that drop
// is exactly the damage the hit dealt, so replaying the same number is a true
// repeat rather than a guess. DirectDamage runs through normal mitigation, so
// the replay does not land for exactly what was asked; each target is therefore
// muted for a short window after a replay and re-baselined, which stops the
// replay from being read as a new hit and multiplying without bound.
int g_multiHitExtra = 0;

struct HitMark {
    int64_t   hp = 0;
    bool      seen = false;
    ULONGLONG quietUntil = 0;
};
static std::unordered_map<void*, HitMark> g_hitMarks;

// Both of the calls below reach into game code with objects we did not create,
// and a wrong assumption there is an access violation rather than a managed
// exception. Each is fenced separately so one bad call disables the replay
// instead of faulting on every tick for the rest of the session.
static bool g_multiHitBroken = false;

// Only the HP number is used here. Reading hpMax and isAlive as well would cost
// two more interpreter round-trips per enemy per tick for values this pass
// throws away, so the read is narrowed to the one field it actually compares.
static bool __declspec(noinline) ReadEnemyHpSafe(void* actor, int64_t* hp) {
    __try {
        void* health = nullptr;
        if (!GetHealth(actor, health)) return false;
        return Invoke(g_i2.object_get_class(health), "get_hp", 0, health, nullptr, hp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool __declspec(noinline) Invoke8Safe(void* api, void* attacker, void* target, int32_t* dmg,
                                             void* empty, uint8_t* f0, uint8_t* f1, uint8_t* f2) {
    __try {
        return Invoke8(api, names::DirectDamage(), nullptr, attacker, target, dmg, empty, empty, f0, f1, f2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void DirectDamage(void* attacker, void* target, int32_t dmg) {
    void* api = FindClassByName(names::ILRuntimeAPI());
    if (!api) return;

    // Exactly the argument shape the aura path uses: object parameters go in as
    // the pointer itself, only value types go in by address. Passing &attacker
    // hands the interpreter a pointer-to-pointer and it faults inside the call.
    void*   empty = g_i2.string_new ? g_i2.string_new("") : nullptr;
    uint8_t f0 = 0, f1 = 0, f2 = 0;
    if (!Invoke8Safe(api, attacker, target, &dmg, empty, &f0, &f1, &f2)) {
        g_multiHitBroken = true;
        LogF("multihit: replay faulted, disabled");
    }
}

// Detects a landed hit from the enemy's HP and replays it. Serves both the
// multi-hit count and the damage multiplier: they are the same observation, and
// running two passes over one HP delta would read each other's replay as a new
// hit and compound.
static void MultiHitPass() {
    if (g_multiHitBroken) return;
    if (!InBattle()) { g_hitMarks.clear(); return; }
    void* attacker = FindMainPlayer();
    if (!attacker) return;

    void* list = FactionActors(attacker, names::ConfrontActors());
    const int n = ListSize(list);
    const ULONGLONG now = GetTickCount64();
    if (n <= 0) { g_hitMarks.clear(); return; }

    // A stage can hold far more hostiles than the player can be swinging at.
    // Past the first dozen the extra reads only cost ticks.
    static constexpr int kMultiHitTargets = 12;
    for (int i = 0; i < n && i < kMultiHitTargets; i++) {
        void* t = ListItem(list, i);
        if (!t || t == attacker) continue;

        int64_t hp = 0;
        g_stage = 26;
        if (!ReadEnemyHpSafe(t, &hp)) {
            g_multiHitBroken = true;
            g_hitMarks.clear();
            LogF("multihit: enemy HP read faulted, replay off");
            g_stage = 25;
            return;
        }
        if (hp <= 0) continue;

        HitMark& mark = g_hitMarks[t];
        if (!mark.seen) { mark.seen = true; mark.hp = hp; continue; }

        // Replays settle over a few logic ticks; while they do, the HP moving
        // is ours, not a new swing, so the baseline just follows it.
        if (now < mark.quietUntil) { mark.hp = hp; continue; }

        const int64_t drop = mark.hp - hp;
        mark.hp = hp;
        if (drop <= 0) continue;

        g_stage = 27;
        // Damage multiplier first, and sized to the remaining HP so the request
        // is always a plausible hit rather than an absurd number the entry can
        // discard. When that is already lethal the multi-hit replays are
        // skipped outright: replaying into a dead target was pure cost.
        const int64_t dmgExtra = drop * (int64_t)(g_cheat.dmgMult - 1);
        const bool lethal = dmgExtra >= hp;
        if (!lethal) {
            const int replays = g_cheat.multiHit - 1;
            for (int k = 0; k < replays && !g_multiHitBroken; k++) {
                DirectDamage(attacker, t, (int32_t)drop);
            }
            if (replays > 0) {
                const int total = InterlockedAdd((volatile LONG*)&g_multiHitExtra, replays);
                if (total == replays) LogF("multihit: replaying hits (%d per hit)", g_cheat.multiHit);
            }
        }

        int64_t after = hp;
        if (dmgExtra > 0) {
            DirectDamage(attacker, t, (int32_t)(lethal ? hp : dmgExtra));
            if (lethal) {
                // The entry can refuse a hit (shields, phase). One read tells
                // us, and the remaining HP is then written directly so a
                // multiplier that should kill always does.
                int64_t left = hp;
                if (ReadEnemyHpSafe(t, &left)) {
                    after = left;
                    if (left > 0) {
                        void* health = nullptr;
                        if (GetHealth(t, health) && health) {
                            int64_t one = 1;
                            void* par[1] = { &one };
                            Invoke(g_i2.object_get_class(health), "set_hp", 1, health, par);
                        }
                    }
                }
            }
        }

        static ULONGLONG lastDmgLog = 0;
        const ULONGLONG dn = GetTickCount64();
        if (dmgExtra > 0 && dn - lastDmgLog >= 3000) {
            lastDmgLog = dn;
            // before - after must be about dmgExtra, not about drop.
            LogF("dmg x%d hit=%lld extra=%lld before=%lld after=%lld", g_cheat.dmgMult,
                 (long long)drop, (long long)dmgExtra, (long long)hp, (long long)after);
        }
        mark.quietUntil = now + 300;
    }

    g_stage = 25;
    if (g_hitMarks.size() > 128) g_hitMarks.clear();
}

// --------------------------------------------------------------- multi-hit
// HitActors sits behind a shared native thunk: detouring the address that the
// MethodInfo reports takes the whole family of callers down with it, so the
// repeat is done in the embedded Lua chunk instead (see lua_bridge.cpp), where
// it only touches the binding the combat scripts actually call.

// ------------------------------------------------------------ speed and hooks
static int g_prevDmg = 1, g_prevDef = 1;
static float g_prevAtkSpd = 1.0f, g_prevMove = 1.0f;
static bool g_prevCrit = false;
static bool g_prevGod = false;

static void ScaleFpField(std::unordered_map<void*, int64_t>& cache, void* owner, uint8_t* p, float mult) {
    int64_t* v = (int64_t*)p;
    auto it = cache.find(owner);
    if (mult <= 1.0f) {
        // Drop the entry once it is restored. Keeping it left the cache
        // non-empty forever, which pinned the "speed is scaled" flag on and
        // made the cheat pass run for the rest of the session.
        if (it != cache.end()) { *v = it->second; cache.erase(it); }
        return;
    }
    if (it == cache.end()) it = cache.emplace(owner, *v).first;
    *v = (int64_t)((double)it->second * (double)mult);
}

// ActorInfo holds the real movement/attack rates (TrueSync.FP, 32 fractional bits).
static void ApplyActorSpeeds(void* actor) {
    void* info = FieldPtr(actor, "actorInfo");
    if (!info) return;
    uint8_t* b = (uint8_t*)info;
    // Game speed drives animation playback and timers, but it does NOT scale
    // the actor's translation: at 5x the walk cycle plays five times as fast
    // while the character still covers ground at 1x, which reads as running on
    // the spot. Movement therefore has to carry the game speed itself.
    float effMove = g_cheat.moveSpdMult * (g_cheat.gameSpeed > 1.0f ? g_cheat.gameSpeed : 1.0f);
    if (effMove < 0.1f) effMove = 0.1f;
    if (effMove > 20.0f) effMove = 20.0f;
    if (effMove != 1.0f || g_cheat.atkSpdMult != 1.0f) g_speedDirty = true;
    ScaleFpField(g_runSpdBase, actor, b + 0xe0, effMove);    // originRunSpeed
    ScaleFpField(g_runSpdCur,  actor, b + 0xe8, effMove);    // runSpeed (what movement reads)
    ScaleFpField(g_atkSpdBase, actor, b + 0x138, g_cheat.atkSpdMult);    // originAttackSpeed
    g_dbgRunSpeed = (double)(*(int64_t*)(b + 0xe8)) / 4294967296.0;
    const double originRun = (double)(*(int64_t*)(b + 0xe0)) / 4294967296.0;
    static ULONGLONG lastRunLog = 0;
    ULONGLONG rnow = GetTickCount64();
    if ((g_cheat.moveSpdMult != 1.0f || g_cheat.atkSpdMult != 1.0f || g_cheat.gameSpeed != 1.0f) && rnow - lastRunLog >= 2000) {
        lastRunLog = rnow;
        LogF("runSpeed cur=%.2f origin=%.2f gs=%.2f eff=%.2f", g_dbgRunSpeed,
             originRun, g_cheat.gameSpeed, effMove);
    }

    ScaleFpField(g_walkSpdBase, actor, b + 0x100, effMove);  // originWalkSpeed
    ScaleFpField(g_walkSpdCur,  actor, b + 0x108, effMove);  // walkSpeed (live)
}

// The damage slider used to push a multiple into ATK through SetAmendValue.
// That write has no dependable inverse: turning it off left the bonus in place
// and repeated writes drove ATK negative, which the game clamps to the damage
// floor (the "1 and 2 damage" report). It is now a replay in MultiHitPass.
static void ApplyStats(void* actor) {
    if (g_cheat.defMult > 1) ApplyAttrMult(actor, ATTR_DEF, g_cheat.defMult);
    else if (g_prevDef > 1) ApplyAttrMult(actor, ATTR_DEF, 1);
    if (g_cheat.alwaysCrit) ApplyAttrAbsolute(actor, ATTR_CRITRATE, 10.0);
    else if (g_prevCrit) ApplyAttrAbsolute(actor, ATTR_CRITRATE, 0.0);
    ApplyActorSpeeds(actor);
}

static void ApplyTimeScale(float v) {
    static void* timeClass = nullptr;
    if (!timeClass && g_i2.ready) {
        void* img = ImageByName("UnityEngine.CoreModule.dll");
        if (img && g_i2.class_from_name) timeClass = g_i2.class_from_name(img, "UnityEngine", "Time");
    }
    if (!timeClass) return;
    // Leaving or finishing a stage resets Unity's clock without telling us, and
    // the cached value then blocks every later write, so the speed silently
    // drops back to 1x. Re-assert while a multiplier is active.
    static ULONGLONG lastPush = 0;
    const ULONGLONG now = GetTickCount64();
    const bool changed = (v != g_appliedTimeScale);
    const bool keepAlive = (v > 1.0f && now - lastPush >= 400);
    if (!changed && !keepAlive) return;
    void* par[1] = { &v };
    if (Invoke(timeClass, "set_timeScale", 1, nullptr, par)) {
        g_appliedTimeScale = v;
        lastPush = now;
    }
}

// Battle speed is the game's own logic clock, not UnityEngine.Time. Every
// battle scene runs on AdventureModuleHelper's logic time scale, and the speed-up
// button routes through Lua2CSharp_GMOrder_SetTimeScale. Driving Unity's
// timeScale alone no longer moves a stage, which is why the old path went dead.
// Applied on change and re-armed every 2s: each stage/scene transition resets it.
static void ApplyLogicTimeScale(float v) {
    static void* helper = nullptr;
    static float lastV = -1.0f;
    static ULONGLONG lastPush = 0;
    if (!helper && g_i2.ready) helper = FindClassByName(names::AdventureModuleHelper());
    if (!helper) return;

    // Stage end tears the battle clock down and the next stage builds a fresh one
    // at 1x, so a one-shot set does not survive. While a multiplier is active the
    // order is re-pushed every 400ms; that is cheap and beats every reset path.
    ULONGLONG now = GetTickCount64();
    const bool changed = (v != lastV);
    const bool keepAlive = (v > 1.0f && now - lastPush >= 400);
    if (!changed && !keepAlive) return;

    void* par[1] = { &v };
    if (Invoke(helper, "Lua2CSharp_GMOrder_SetTimeScale", 1, nullptr, par)) {
        if (changed) LogF("logic timeScale %.2f", v);
        lastV = v;
        lastPush = now;
    }
}

static void ApplyGameSpeed() {
    static void* timeClass = nullptr;
    if (!timeClass && g_i2.ready) {
        void* img = ImageByName("UnityEngine.CoreModule.dll");
        if (img && g_i2.class_from_name) timeClass = g_i2.class_from_name(img, "UnityEngine", "Time");
    }
    const float spd = g_cheat.gameSpeed < 1.0f ? 1.0f : g_cheat.gameSpeed;
    if (timeClass) ApplyTimeScale(spd); // FX and animation rate
    ApplyLogicTimeScale(spd);           // battle logic clock
}

// ---------------------------------------------------------------- cooldown
// No-CD used to be a detour on PlayerSkillCd.CanUseCD, which meant patching
// .text inside GameAssembly.dll — the one thing an integrity scan looks for
// first. ResetPlayerSkills already forces every SkillInfo back to ready from
// the polling tick, so the detour bought nothing but a modified code page.

// ---------------------------------------------------------------- multi-hit
// HitActors (0x7FFD217639E0) is the one combat entry with its own native body,
// which made it look like the right place to repeat a hit. It is not hookable:
// a detour that only forwards the call still kills the process, so the fault is
// in MinHook's trampoline for that prologue, not in the repeat. Verified twice,
// the second time with the counter incremented before the forward so the trace
// distinguishes the two. Nothing is installed here now.

// Menus, Ascension and other UI scenes also contain actors; running world
// manipulation there corrupts state and crashes the game.
static bool InBattle() {
    if (g_battleGen == g_tickGen) return g_battleCached;
    g_battleCached = false;
    g_battleGen = g_tickGen;
    if (!g_i2.ready) return false;
    void* helper = FindClassByName(names::AdventureModuleHelper());
    if (!helper) return false;
    void* m = Method(helper, names::CurrentMonsters(), 0);
    if (!m) return false;
    void* exc = nullptr;
    void* list = g_i2.runtime_invoke(m, nullptr, nullptr, &exc);
    if (exc || !list) return false;
    g_battleCached = ListSize(list) > 0;
    return g_battleCached;
}


static void TickCheats() {
    // This runs from the xLua pcall hook, which fires far more often than the
    // simulation advances - dozens to hundreds of times a second. Everything
    // here is polled state, so capping the pass at ~60 Hz costs nothing and
    // removes the bulk of the per-frame interpreter work.
    static ULONGLONG lastPass = 0;
    const ULONGLONG passNow = GetTickCount64();
    if (passNow - lastPass < 16) return;
    lastPass = passNow;

    const bool speedDirty = !g_runSpdBase.empty() || !g_walkSpdBase.empty() || !g_atkSpdBase.empty();
    const bool stats = g_cheat.dmgMult > 1 || g_cheat.defMult > 1 || g_cheat.atkSpdMult > 1.0f ||
        g_cheat.moveSpdMult > 1.0f || g_cheat.alwaysCrit || speedDirty ||
        g_prevDmg > 1 || g_prevDef > 1 || g_prevCrit || g_speedDirty;
    const bool skills = g_cheat.noCd || g_cheat.infEnergy || g_cheat.unlimitedDash;
    const bool guard = g_cheat.instantBreak || g_cheat.noShield;
    if (!g_cheat.god && !stats && !skills && !guard && g_cheat.multiHit <= 1 &&
        g_cheat.gameSpeed == 1.0f) {
        if (g_appliedTimeScale != 1.0f) { ApplyTimeScale(1.0f); }
        ApplyLogicTimeScale(1.0f);
        g_toughMonsters = 0;
        return;
    }
    if (!g_i2.ready) { if (!ResolveExports()) return; }
    EnsureThread();

    if (g_cheat.atkSpdMult < 1.0f) g_cheat.atkSpdMult = 1.0f;
    if (g_cheat.atkSpdMult > 3.0f) g_cheat.atkSpdMult = 3.0f;
    if (g_cheat.moveSpdMult < 1.0f) g_cheat.moveSpdMult = 1.0f;
    if (g_cheat.moveSpdMult > 2.5f) g_cheat.moveSpdMult = 2.5f;
    if (g_cheat.dmgMult < 1) g_cheat.dmgMult = 1;
    if (g_cheat.defMult < 1) g_cheat.defMult = 1;

    g_stage = 10; ApplyGameSpeed();

    g_stage = 24;
    if (guard) ToughnessPass();
    else g_toughMonsters = 0;
    if (g_cheat.multiHit < 1) g_cheat.multiHit = 1;
    if (g_cheat.multiHit > 8) g_cheat.multiHit = 8;
    g_stage = 25;
    if (g_cheat.multiHit > 1 || g_cheat.dmgMult > 1) MultiHitPass();
    else g_hitMarks.clear();

    g_stage = 30; void* players = PlayerActorList();
    int pn = ListSize(players);
    g_lastPlayers = pn;
    {
        static bool loggedPn = false;
        if (!loggedPn && pn > 0) { loggedPn = true; LogF("tick: %d actors in list", pn); }
    }
    // Ascension and other transitions swap characters or recycle actor objects.
    // Drop every cached base when the player set changes so nothing stale is
    // re-applied to the wrong character.
    uint64_t sig = 1469598103934665603ull;
    for (int i = 0; i < pn; i++) {
        void* a = ListItem(players, i);
        if (!IsPlayerActorClass(a)) continue;
        sig = (sig ^ (uint64_t)(uintptr_t)a) * 1099511628211ull;
    }
    if (sig != g_playerSig) {
        g_playerSig = sig;
        // The amend record is deliberately NOT cleared here: dropping it while
        // the amend is still on the list makes the next pass read a value that
        // already contains the delta and write a bigger one on top. The speed
        // caches are only safe to drop when nothing is currently scaled -
        // clearing them mid-multiplier loses the original and makes the value
        // unrestorable.
        if (!g_speedDirty) {
            g_runSpdBase.clear();
            g_walkSpdBase.clear();
            g_atkSpdBase.clear();
            g_runSpdCur.clear();
            g_walkSpdCur.clear();
        }
        g_prevAtkSpd = 1.0f; g_prevMove = 1.0f; g_prevCrit = false;
    }
    bool anyMain = false;
    for (int i = 0; i < pn; i++) {
        void* actor = ListItem(players, i);
        if (!IsPlayerActorClass(actor)) continue;
        anyMain = true;
        g_stage = 40; PlayerInfo pi = ReadPlayer(actor);
        g_lastHp = pi.hp; g_lastMax = pi.hpMax; g_lastAlive = pi.alive;
        if (g_cheat.god && pi.health) {
            // One lethal hit can land between ticks, so also inflate Max HP.
            SetAmend(actor, ATTR_MAXHP, PARAM_PERCENTAGE, 99.0);
            int64_t maxNow = pi.hpMax, hpNow = pi.hp;
            void* hk = g_i2.object_get_class(pi.health);
            Invoke(hk, "get_hpMax", 0, pi.health, nullptr, &maxNow);
            Invoke(hk, "get_hp", 0, pi.health, nullptr, &hpNow);
            if (!pi.alive) {
                uint8_t t = 1;
                void* par[1] = { &t };
                Invoke(hk, "set_isAlive", 1, pi.health, par);
            }
            if (hpNow < maxNow) {
                int64_t v = maxNow;
                void* par[1] = { &v };
                Invoke(hk, "set_hp", 1, pi.health, par);
            }
        } else if (g_prevGod) {
            SetAmend(actor, ATTR_MAXHP, PARAM_PERCENTAGE, 0.0);
        }
        if (skills) {
            g_stage = 50; void* psc = FieldPtr(actor, "playerSkillCd");
            if (psc) ResetPlayerSkills(psc, g_cheat.infEnergy);
        }
        if (stats || g_cheat.gameSpeed > 1.0f) { g_stage = 60; ApplyStats(actor); }
    }
    g_stage = 70;
    if (!anyMain) {
        // Only drop the speed caches when nothing is scaled: they hold the
        // originals the restore needs, and a gap between waves must not erase
        // them (that left movement stuck at the old multiplier).
        if (!g_speedDirty) {
            g_runSpdBase.clear();
            g_walkSpdBase.clear();
            g_atkSpdBase.clear();
            g_runSpdCur.clear();
            g_walkSpdCur.clear();
        }
    }
    if (anyMain) {
        // Every live actor went through ApplyStats this tick, so with all three
        // multipliers back at 1 there is nothing left scaled.
        if (g_cheat.gameSpeed <= 1.0f && g_cheat.moveSpdMult <= 1.0f &&
            g_cheat.atkSpdMult <= 1.0f) {
            g_speedDirty = false;
        }
        g_prevGod = g_cheat.god;
        g_prevDmg = g_cheat.dmgMult;
        g_prevDef = g_cheat.defMult;
        g_prevAtkSpd = g_cheat.atkSpdMult;
        g_prevMove = g_cheat.moveSpdMult;
        g_prevCrit = g_cheat.alwaysCrit;
    }
}


static int LogFault(EXCEPTION_POINTERS* ep, const char* tag) {    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    uintptr_t addr = (uintptr_t)(ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr);
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    static constexpr auto kFault = OBF("C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\fault.txt");
    FILE* f = nullptr;
    if (fopen_s(&f, obf::Decoded(kFault), "a") == 0 && f) {
        fprintf(f, "%s stage=%ld code=0x%08lX addr=%p ga=%p off=%lld\n",
            tag, g_stage, code, (void*)addr, (void*)ga,
            ga ? (long long)(addr - (uintptr_t)ga) : -1LL);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------- automation
//
// The story and card flows are not in the C# hotfix image - they are scripted
// in the game's own Lua layer (Game.UI.Avg.*). The driving happens in
// lua_bridge.cpp, on the game thread, through the game's own event channel.
// What is left here is the discovery dump: it lists every class that carries a
// story/card noun so the vocabulary can be checked against reality instead of
// trusted.
static bool NameHas(const char* s, const char* const* keys, int n) {
    if (!s) return false;
    for (int i = 0; i < n; ++i)
        if (keys[i] && strstr(s, keys[i])) return true;
    return false;
}

// Everything that matched the vocabulary, with every zero-arg method, so the
// resolved pair can be confirmed against the real list instead of trusted.
void DumpAutomationApi(const char* path) {
    if (!g_i2.ready || !path) return;
    if (!g_i2.domain_get_assemblies || !g_i2.image_get_class_count || !g_i2.image_get_class ||
        !g_i2.class_get_methods || !g_i2.method_get_name)
        return;

    size_t nAsm = 0;
    void** asms = g_i2.domain_get_assemblies(g_domain, &nAsm);
    if (!asms) return;

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) return;

    static const char* const kCls[] = {
        "Story", "Dialog", "Dialogue", "Plot", "Talk",
        "Card", "Perk", "Reward", "Select", "Choice", "Chapter", "Section"
    };
    fprintf(f, "domain=%p assemblies=%zu\n", g_domain, nAsm);

    int classes = 0, matched = 0;
    for (size_t a = 0; a < nAsm; ++a) {
        void* img = g_i2.assembly_get_image(asms[a]);
        if (!img) continue;
        const char* imgName = g_i2.image_get_name ? g_i2.image_get_name(img) : "?";
        const size_t count = g_i2.image_get_class_count(img);
        for (size_t k = 0; k < count; ++k) {
            void* c = g_i2.image_get_class(img, k);
            if (!c) continue;
            ++classes;
            const char* cn = g_i2.class_get_name(c);
            if (!NameHas(cn, kCls, 12)) continue;
            ++matched;

            const char* ns = g_i2.class_get_namespace ? g_i2.class_get_namespace(c) : "";
            fprintf(f, "\n[C] %s :: %s%s%s\n", imgName ? imgName : "?",
                    (ns && *ns) ? ns : "", (ns && *ns) ? "." : "", cn);
            void* it = nullptr;
            while (void* m = g_i2.class_get_methods(c, &it)) {
                const char* mn = g_i2.method_get_name(m);
                const unsigned argc = g_i2.method_get_param_count ? g_i2.method_get_param_count(m) : 0;
                fprintf(f, "    m %s/%u\n", mn ? mn : "?", argc);
            }
        }
    }
    fprintf(f, "\nscanned=%d matched=%d\n", classes, matched);
    fclose(f);
    LogF("automation dump: %d/%d classes -> %s", matched, classes, path);
}

// Runs only from the game thread (hooked lua_pcall): IL2CPP allocations from
// any other thread trip Unity's "Collecting from unknown thread" GC abort.
void MaybeRunCheats() {
    static ULONGLONG lastTick = 0, lastAura = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastTick >= 120) {
        lastTick = now;
        if (!g_i2.ready) ResolveExports();
        if (g_i2.ready) {
            // Remote trigger: create work\probe.on to dump the Hotfix API once.
            static constexpr auto kProbePath = OBF("C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\probe.on");
            static constexpr auto kApiDump   = OBF("C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\hotfix_api.txt");
            if (GetFileAttributesA(obf::Decoded(kProbePath)) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileA(obf::Decoded(kProbePath));
                DumpHotfixApi(obf::Decoded(kApiDump));
            }
            // Same trigger for the story/card vocabulary: drop the file, get the
            // real class and method names back without touching the menu.
            static constexpr auto kProbeAuto = OBF("C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\probe_auto.on");
            static constexpr auto kAutoDump  = OBF("C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\automation_api.txt");
            if (GetFileAttributesA(obf::Decoded(kProbeAuto)) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileA(obf::Decoded(kProbeAuto));
                DumpAutomationApi(obf::Decoded(kAutoDump));
            }
            __try { ++g_tickGen; TickCheats(); }
            __except (LogFault(GetExceptionInformation(), "tick")) { }
            // Script console: runs on this thread, which is already attached.
            __try { LuauTick(); }
            __except (LogFault(GetExceptionInformation(), "luau")) { LuauAbandon(); }
        }
    }
    if (g_cheat.aura && now - lastAura >= 400) {
        lastAura = now;
        __try { ++g_tickGen; ArenaAuraTick(); }
        __except (LogFault(GetExceptionInformation(), "aura")) { }
    }
}


void ResetSkillsNow() {
    if (!g_i2.ready) { if (!ResolveExports()) return; }
    EnsureThread();
    void* players = PlayerActorList();
    int n = ListSize(players);
    int hits = 0;
    for (int i = 0; i < n; i++) {
        void* actor = ListItem(players, i);
        if (!IsPlayerActorClass(actor) || !IsMainPlayer(actor)) continue;
        void* psc = FieldPtr(actor, "playerSkillCd");
        if (!psc) continue;
        ResetPlayerSkills(psc, true);
        hits++;
    }
    LogF("native skill reset: %d players", hits);
}

void ResetModifiers() {
    if (!g_i2.ready) { if (!ResolveExports()) return; }
    EnsureThread();
    void* list = PlayerActorList();
    int n = ListSize(list);
    for (int i = 0; i < n; i++) {
        void* actor = ListItem(list, i);
        if (!IsPlayerActorClass(actor)) continue;
        // Defence is amend-based, so undo it through the tracked path rather
        // than writing 0 blindly: that keeps the bookkeeping and the game's
        // value in step.
        ApplyAttrMult(actor, ATTR_DEF, 1);
        SetAmend(actor, ATTR_ATK, PARAM_PERCENTAGE, 0.0);
        SetAmend(actor, ATTR_ATKSPD_P, PARAM_PERCENTAGE, 0.0);
        SetAmend(actor, ATTR_CRITRATE, PARAM_ABSOLUTE, 0.0);
        void* info = FieldPtr(actor, "actorInfo");
        if (info) {
            auto restore = [&](std::unordered_map<void*, int64_t>& cache, size_t off) {
                auto it = cache.find(actor);
                if (it != cache.end()) { *(int64_t*)((uint8_t*)info + off) = it->second; cache.erase(it); }
            };
            // Both halves of each pair: origin and the live value the game
            // actually moves the actor with.
            restore(g_runSpdBase, 0xe0);
            restore(g_runSpdCur,  0xe8);
            restore(g_walkSpdBase, 0x100);
            restore(g_walkSpdCur,  0x108);
            restore(g_atkSpdBase, 0x138);
        }
    }
    g_amendRec.clear();
    g_speedDirty = false;
    g_cheat.dmgMult = 1; g_cheat.defMult = 1;
    g_cheat.atkSpdMult = 1.0f; g_cheat.moveSpdMult = 1.0f; g_cheat.gameSpeed = 1.0f;
    g_cheat.alwaysCrit = false;
    g_cheat.instantBreak = false; g_cheat.noShield = false; g_cheat.multiHit = 1;
    g_toughMonsters = 0;
    ApplyTimeScale(1.0f);
    g_prevDmg = 1; g_prevDef = 1; g_prevAtkSpd = 1.0f; g_prevMove = 1.0f; g_prevCrit = false;
    LogF("modifiers reset");
}


// ------------------------------------------------------------ action queue
// The action bar draws inside the Present hook. Anything that walks the IL2CPP
// heap has to run on the game thread, so a click only records the request and
// the pcall hook drains it on the next tick.
static volatile LONG g_pendingAction = 0;

void QueueAction(int action) {
    InterlockedExchange(&g_pendingAction, action);
}

static void RunPendingActionInner() {
    const LONG action = InterlockedExchange(&g_pendingAction, 0);
    switch (action) {
    case ACT_RESET_SKILLS: ResetSkillsNow(); break;
    case ACT_RESET_STATS:  ResetModifiers(); break;
    case ACT_KILL_MOBS:    KillMobs();       break;
    case ACT_SCAN_AUTOMATION: {
        static constexpr auto kAutoDump = OBF("C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\automation_api.txt");
        static const auto decoded = obf::Decoded(kAutoDump);
        DumpAutomationApi(decoded);
        break;
    }
    default: break;
    }
}

// These walk the IL2CPP heap from a click, and one bad pointer used to take
// the whole process down because nothing guarded the call. The tick already
// has this guard; the action path needs the same one.
void RunPendingAction() {
    __try { RunPendingActionInner(); }
    __except (LogFault(GetExceptionInformation(), "action")) { }
}


static constexpr auto kCfgPath = OBF("C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\stella_menu.ini");
static constexpr auto kWorkDir = OBF("C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work");

// Decoded once into static storage so callers get a stable pointer; a Decoded
// temporary would dangle the moment the expression ended.
const char* CfgPath() {
    static const auto decoded = obf::Decoded(kCfgPath);
    return decoded;
}
float g_cfgX = -1, g_cfgY = -1, g_cfgW = 900, g_cfgH = 600;
int g_wallAlpha = 150;
int g_dimAlpha = 145;
float g_fontScale = 1.0f;

// Autosave fires every two seconds while the menu is open. Without a change
// check that meant an fopen/fprintf/fflush cycle every two seconds for the
// whole session whether or not anything moved. Compare against the snapshot
// written last and skip the file work entirely when nothing differs.
static CheatState g_savedCheat{};
static float g_savedX, g_savedY, g_savedW, g_savedH, g_savedFont;
static int   g_savedWall, g_savedDim;
static bool  g_savedValid = false;

static void SnapshotConfig() {
    g_savedCheat = g_cheat;
    g_savedX = g_cfgX; g_savedY = g_cfgY; g_savedW = g_cfgW; g_savedH = g_cfgH;
    g_savedWall = g_wallAlpha; g_savedDim = g_dimAlpha; g_savedFont = g_fontScale;
    g_savedValid = true;
}

static bool ConfigDirty() {
    if (!g_savedValid) return true;
    return memcmp(&g_savedCheat, &g_cheat, sizeof(CheatState)) != 0
        || g_savedX != g_cfgX || g_savedY != g_cfgY
        || g_savedW != g_cfgW || g_savedH != g_cfgH
        || g_savedWall != g_wallAlpha || g_savedDim != g_dimAlpha
        || g_savedFont != g_fontScale;
}

void LoadConfig() {
    FILE* f = nullptr;
    if (fopen_s(&f, CfgPath(), "r") != 0 || !f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = { 0 };
        double v = 0;
        if (sscanf_s(line, "%63[^=]=%lf", key, (unsigned)_countof(key), &v) != 2) continue;
        if (!strcmp(key, "pos_x")) g_cfgX = (float)v;
        else if (!strcmp(key, "pos_y")) g_cfgY = (float)v;
        else if (!strcmp(key, "width")) g_cfgW = (float)v;
        else if (!strcmp(key, "height")) g_cfgH = (float)v;
        else if (!strcmp(key, "god")) g_cheat.god = v != 0;
        else if (!strcmp(key, "no_cd")) g_cheat.noCd = v != 0;
        else if (!strcmp(key, "energy")) g_cheat.infEnergy = v != 0;
        else if (!strcmp(key, "dash")) g_cheat.unlimitedDash = v != 0;
        else if (!strcmp(key, "crit")) g_cheat.alwaysCrit = v != 0;
        else if (!strcmp(key, "aura")) g_cheat.aura = v != 0;
        else if (!strcmp(key, "aura_lethal")) g_cheat.auraLethal = v != 0;
        else if (!strcmp(key, "safe_mode")) g_cheat.safeMode = v != 0;
        else if (!strcmp(key, "damage")) g_cheat.dmgMult = (int)v;
        else if (!strcmp(key, "defense")) g_cheat.defMult = (int)v;
        else if (!strcmp(key, "atk_speed")) g_cheat.atkSpdMult = (float)v;
        else if (!strcmp(key, "move_speed")) g_cheat.moveSpdMult = (float)v;
        else if (!strcmp(key, "game_speed")) g_cheat.gameSpeed = (float)v;
        else if (!strcmp(key, "aura_damage")) g_cheat.auraDmg = (int)v;
        else if (!strcmp(key, "instant_break")) g_cheat.instantBreak = v != 0;
        else if (!strcmp(key, "no_shield")) g_cheat.noShield = v != 0;
        else if (!strcmp(key, "multi_hit")) g_cheat.multiHit = (int)v;
        else if (!strcmp(key, "auto_skip")) g_cheat.autoSkipStory = v != 0;
        else if (!strcmp(key, "auto_cards")) g_cheat.autoSelectCards = v != 0;
        else if (!strcmp(key, "wallpaper")) g_wallAlpha = (int)v;
        else if (!strcmp(key, "backdrop")) g_dimAlpha = (int)v;
        else if (!strcmp(key, "font_scale")) g_fontScale = (float)v;
    }
    fclose(f);
    auto clampi = [](int& v, int lo, int hi) { if (v < lo) v = lo; if (v > hi) v = hi; };
    auto clampf = [](float& v, float lo, float hi) { if (v < lo) v = lo; if (v > hi) v = hi; };
    clampi(g_cheat.dmgMult, 1, 50);
    clampi(g_cheat.defMult, 1, 100);
    clampf(g_cheat.atkSpdMult, 1.0f, 2.0f);
    clampf(g_cheat.moveSpdMult, 1.0f, 2.5f);
    clampf(g_cheat.gameSpeed, 1.0f, 5.0f);
    clampi(g_cheat.auraDmg, 10, 2000000000);
    clampi(g_cheat.multiHit, 1, 8);
    clampf(g_fontScale, 0.8f, 1.6f);
    clampf(g_cfgW, 800.0f, 1600.0f);
    clampf(g_cfgH, 520.0f, 1000.0f);
    // Baseline for the autosave change check: what is on disk is what we hold.
    SnapshotConfig();
    LogF("config loaded");
}

void SaveConfig() {
    if (!ConfigDirty()) return;
    FILE* f = nullptr;
    if (fopen_s(&f, CfgPath(), "w") != 0 || !f) {
        // First run on a clean tree: the folder does not exist yet.
        CreateDirectoryA(obf::Decoded(kWorkDir), nullptr);
        if (fopen_s(&f, CfgPath(), "w") != 0 || !f) return;
    }
    fprintf(f, "pos_x=%.1f\npos_y=%.1f\nwidth=%.1f\nheight=%.1f\n", g_cfgX, g_cfgY, g_cfgW, g_cfgH);
    fprintf(f, "god=%d\nno_cd=%d\nenergy=%d\ndash=%d\ncrit=%d\naura=%d\naura_lethal=%d\nsafe_mode=%d\n",
        g_cheat.god, g_cheat.noCd, g_cheat.infEnergy, g_cheat.unlimitedDash,
        g_cheat.alwaysCrit, g_cheat.aura, g_cheat.auraLethal, g_cheat.safeMode);
    fprintf(f, "damage=%d\ndefense=%d\natk_speed=%.2f\nmove_speed=%.2f\ngame_speed=%.2f\naura_damage=%d\n",
        g_cheat.dmgMult, g_cheat.defMult, g_cheat.atkSpdMult, g_cheat.moveSpdMult,
        g_cheat.gameSpeed, g_cheat.auraDmg);
    fprintf(f, "instant_break=%d\nno_shield=%d\nmulti_hit=%d\n",
        g_cheat.instantBreak, g_cheat.noShield, g_cheat.multiHit);
    fprintf(f, "auto_skip=%d\nauto_cards=%d\n",
        g_cheat.autoSkipStory, g_cheat.autoSelectCards);
    fprintf(f, "wallpaper=%d\nbackdrop=%d\nfont_scale=%.2f\n", g_wallAlpha, g_dimAlpha, g_fontScale);
    fclose(f);
    SnapshotConfig();
}

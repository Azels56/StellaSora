// Gameplay features: actor lists, attributes, skills, aura, cooldowns, config.
#include "features.h"
#include "il2cpp_api.h"
#include "overlay.h"
#include "MinHook.h"
#include <cstdio>
#include <unordered_map>


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


static std::unordered_map<uint64_t, int64_t> g_attrBase;
static std::unordered_map<void*, int64_t> g_runSpdBase;
static std::unordered_map<void*, int64_t> g_walkSpdBase;
static std::unordered_map<void*, int64_t> g_atkSpdBase;
void* g_canUseCdFn = nullptr;
static bool (*g_origCanUseCd)(void*, int32_t) = nullptr;
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

static void AmendList(void* listObj, int attr, int paramType, double value) {
    if (!listObj) return;
    int32_t t = attr;
    int32_t p = paramType;
    int64_t raw = (int64_t)(value * 16777216.0);
    void* args[3] = { &t, &p, &raw };
    Invoke(g_i2.object_get_class(listObj), "SetAmendValue", 3, listObj, args);
}

static void SetAmend(void* actor, int attr, int paramType, double value) {
    AmendList(FieldPtr(actor, "attributeList"), attr, paramType, value);
    // Each skill keeps its own attribute list snapshot; damage reads that copy.
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

static void ApplyAttrMult(void* actor, int idx, int mult) {
    SetAmend(actor, idx, PARAM_PERCENTAGE, mult > 1 ? (double)(mult - 1) : 0.0);
}

static void ApplyAttrAbsolute(void* actor, int idx, double value) {
    SetAmend(actor, idx, PARAM_ABSOLUTE, value);
}

static void ApplyAttrMultF(void* actor, int idx, float mult) {
    SetAmend(actor, idx, PARAM_PERCENTAGE, mult > 1.0f ? (double)(mult - 1.0f) : 0.0);
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
    void* api = FindClassByName("ILRuntimeAPI");
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
    return n && strstr(n, "PlayerAdventureActor") != nullptr;
}

// FindObjectsOfType crashes under the hotfix runtime; the game's own actor list
// is the reliable source in every mode, including Menace Arena.
static void* PlayerActorList() {
    void* api = FindClassByName("ILRuntimeAPI");
    if (!api) return nullptr;
    uint8_t flag = 0;
    void* params[1] = { &flag };
    return InvokeStaticList(api, "GetAllPlayerActorAndMinion", 1, params);
}

static void* FindMainPlayer() {
    void* list = PlayerActorList();
    int n = ListSize(list);
    for (int i = 0; i < n; i++) {
        void* a = ListItem(list, i);
        if (!IsPlayerActorClass(a)) continue;
        if (IsMainPlayer(a)) return a;
    }
    return nullptr;
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
    void* helper = FindClassByName("AdventureModuleHelper");
    if (!helper) return false;
    void* arg[1] = { &id };
    if (Invoke(helper, "Lua2CSharp_GMOrder_KillMonstersByActorId", 1, nullptr, arg)) return true;
    return Invoke(helper, "Lua2CSharp_GMOrder_KillMonstersById", 1, nullptr, arg);
}


static bool GMKillMonsters() {
    if (g_cheat.safeMode) return false;
    void* helper = FindClassByName("AdventureModuleHelper");
    if (!helper) return false;
    uint8_t flag = 0;
    void* params[1] = { &flag };
    void* list = InvokeStaticList(helper, "GetCurrentMonsters", 0, nullptr);
    int n = ListSize(list);
    bool any = false;
    int done = 0;
    for (int i = 0; i < n && done < 3; i++) {
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

static int DamageActorList(void* attacker, void* list, int damage, bool lethal, int maxTargets) {
    if (!attacker || !list) return 0;
    void* api = FindClassByName("ILRuntimeAPI");
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
        if (!AuraReady(target, now)) continue;

        // DirectDamage is int32; the cap is more than enough for one hit.
        int32_t dmg = lethal ? 2000000000 : (damage > 2000000000 ? 2000000000 : damage);
        uint8_t f0 = 0, f1 = 0, f2 = 0;
        Invoke8(api, "DirectDamage", nullptr, attacker, target, &dmg, empty, empty, &f0, &f1, &f2);
        int64_t after = ReadHp(health);
        if (i == 0) { g_auraHpBefore = before; g_auraHpAfter = after; }
        ShowDamageNumber(target, (long long)(lethal ? before : dmg));
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
    g_stage = 120; void* list = FactionActors(attacker, "GetConfrontFactionActors");
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
    void* attacker = FindMainPlayer();
    if (!attacker) return false;
    void* list = FactionActors(attacker, "GetConfrontFactionActors");
    bool any = DamageActorList(attacker, list, 1000000000, true, 32) > 0;
    int n = ListSize(list);
    for (int i = 0; i < n; i++) {
        void* t = ListItem(list, i);
        if (t) any = KillViaGM(t) || any;
    }
    any = GMKillMonsters() || any;
    return any;
}


// ------------------------------------------------------------ speed and hooks
static int g_prevDmg = 1, g_prevDef = 1;
static float g_prevAtkSpd = 1.0f, g_prevMove = 1.0f;
static bool g_prevCrit = false;
static bool g_prevGod = false;

static void ScaleFpField(std::unordered_map<void*, int64_t>& cache, void* owner, uint8_t* p, float mult) {
    int64_t* v = (int64_t*)p;
    auto it = cache.find(owner);
    if (mult <= 1.0f) { if (it != cache.end()) *v = it->second; return; }
    if (it == cache.end()) it = cache.emplace(owner, *v).first;
    *v = (int64_t)((double)it->second * (double)mult);
}

// ActorInfo holds the real movement/attack rates (TrueSync.FP, 32 fractional bits).
static void ApplyActorSpeeds(void* actor) {
    void* info = FieldPtr(actor, "actorInfo");
    if (!info) return;
    uint8_t* b = (uint8_t*)info;
    // Unity timeScale speeds up everything, movement included. Divide the player's
    // base speed by it so 5x game speed does not turn into 5x movement.
    float effMove = g_cheat.moveSpdMult / (g_cheat.gameSpeed > 1.0f ? g_cheat.gameSpeed : 1.0f);
    if (effMove < 0.1f) effMove = 0.1f;
    ScaleFpField(g_runSpdBase, actor, b + 0xe0, effMove);    // originRunSpeed
    ScaleFpField(g_atkSpdBase, actor, b + 0x138, g_cheat.atkSpdMult);    // originAttackSpeed
    g_dbgRunSpeed = (double)(*(int64_t*)(b + 0xe8)) / 4294967296.0;
    static ULONGLONG lastRunLog = 0;
    ULONGLONG rnow = GetTickCount64();
    if ((g_cheat.moveSpdMult != 1.0f || g_cheat.atkSpdMult != 1.0f || g_cheat.gameSpeed != 1.0f) && rnow - lastRunLog >= 2000) {
        lastRunLog = rnow;
        LogF("runSpeed %.2f x%.2f", g_dbgRunSpeed, g_cheat.moveSpdMult);
    }

    ScaleFpField(g_walkSpdBase, actor, b + 0x100, effMove);  // originWalkSpeed
}

static const int kDamageAttrs[] = { ATTR_ATK, 48 /*GENDMG*/, 49 /*DMGPLUS*/, 50 /*FINALDMG*/,
    51 /*FINALDMGPLUS*/, 55 /*NORMALDMG*/, 56 /*SKILLDMG*/, 57 /*ULTRADMG*/, 8 /*CRITPOWER*/ };

static void ApplyDamageAmends(void* actor, int mult) {
    for (int idx : kDamageAttrs) ApplyAttrMult(actor, idx, mult);
}

static void ApplyStats(void* actor) {
    if (g_cheat.dmgMult > 1) ApplyDamageAmends(actor, g_cheat.dmgMult);
    else if (g_prevDmg > 1) ApplyDamageAmends(actor, 1);
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
    if (v == g_appliedTimeScale) return;
    void* par[1] = { &v };
    if (Invoke(timeClass, "set_timeScale", 1, nullptr, par)) g_appliedTimeScale = v;
}

static void ApplyGameSpeed() {
    static void* timeClass = nullptr;
    if (!timeClass && g_i2.ready) {
        void* img = ImageByName("UnityEngine.CoreModule.dll");
        if (img && g_i2.class_from_name) timeClass = g_i2.class_from_name(img, "UnityEngine", "Time");
    }
    if (!timeClass) return;
    ApplyTimeScale(g_cheat.gameSpeed < 1.0f ? 1.0f : g_cheat.gameSpeed);
}

static bool __fastcall HookCanUseCD(void* self, int32_t skillId) {
    if (g_cheat.noCd) return true;
    return g_origCanUseCd(self, skillId);
}

static bool IsInGameAssembly(void* fn) {
    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    if (!ga || !fn) return false;
    uint8_t* base = (uint8_t*)ga;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    uint8_t* p = (uint8_t*)fn;
    return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
}

static bool EnsureCdHook() {
    if (g_cheat.safeMode) return false;
    if (g_canUseCdFn) return true;
    void* k = FindClassByName("PlayerSkillCd");
    if (!k) return false;
    void* m = Method(k, "CanUseCD", 1);
    if (!m) return false;
    void* fn = *(void**)m; // Il2CppMethodInfo::methodPointer
    if (!IsInGameAssembly(fn)) { LogF("CanUseCD pointer rejected: %p", fn); return false; }
    if (MH_CreateHook(fn, (void*)&HookCanUseCD, (void**)&g_origCanUseCd) != MH_OK) { LogF("CanUseCD hook create failed"); return false; }
    if (MH_EnableHook(fn) != MH_OK) { LogF("CanUseCD hook enable failed"); return false; }
    g_canUseCdFn = fn;
    LogF("CanUseCD hooked %p", fn);
    return true;
}

static void RemoveCdHook() {
    if (!g_canUseCdFn) return;
    MH_DisableHook(g_canUseCdFn);
    MH_RemoveHook(g_canUseCdFn);
    g_canUseCdFn = nullptr;
    g_origCanUseCd = nullptr;
    LogF("CanUseCD unhooked");
}

// Menus, Ascension and other UI scenes also contain actors; running world
// manipulation there corrupts state and crashes the game.
static bool InBattle() {
    if (!g_i2.ready) return false;
    void* helper = FindClassByName("AdventureModuleHelper");
    if (!helper) return false;
    void* m = Method(helper, "GetCurrentMonsters", 0);
    if (!m) return false;
    void* exc = nullptr;
    void* list = g_i2.runtime_invoke(m, nullptr, nullptr, &exc);
    if (exc || !list) return false;
    return ListSize(list) > 0;
}


static void TickCheats() {
    const bool speedDirty = !g_runSpdBase.empty() || !g_walkSpdBase.empty() || !g_atkSpdBase.empty();
    const bool stats = g_cheat.dmgMult > 1 || g_cheat.defMult > 1 || g_cheat.atkSpdMult > 1.0f ||
        g_cheat.moveSpdMult > 1.0f || g_cheat.alwaysCrit || speedDirty ||
        g_prevDmg > 1 || g_prevDef > 1 || g_prevCrit;
    const bool skills = g_cheat.noCd || g_cheat.infEnergy || g_cheat.unlimitedDash;
    if (!g_cheat.god && !stats && !skills && g_cheat.gameSpeed == 1.0f) {
        if (g_appliedTimeScale != 1.0f) { ApplyTimeScale(1.0f); }
        if (g_canUseCdFn) RemoveCdHook();
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
    g_stage = 20; if (g_cheat.noCd) EnsureCdHook();
    else if (g_canUseCdFn) RemoveCdHook();

    g_stage = 30; void* players = PlayerActorList();
    int pn = ListSize(players);
    g_lastPlayers = pn;
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
        g_attrBase.clear();
        g_runSpdBase.clear();
        g_walkSpdBase.clear();
        g_atkSpdBase.clear();
        g_prevDmg = 1; g_prevDef = 1;
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
        // Scene/stage transition: forget stale bases so a recycled actor pointer
        // cannot inherit another character's values.
        g_attrBase.clear();
        g_runSpdBase.clear();
        g_walkSpdBase.clear();
        g_atkSpdBase.clear();
    }
    if (anyMain) {
        g_prevGod = g_cheat.god;
        g_prevDmg = g_cheat.dmgMult;
        g_prevDef = g_cheat.defMult;
        g_prevAtkSpd = g_cheat.atkSpdMult;
        g_prevMove = g_cheat.moveSpdMult;
        g_prevCrit = g_cheat.alwaysCrit;
    }
}


static int LogFault(EXCEPTION_POINTERS* ep, const char* tag) {
    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    uintptr_t addr = (uintptr_t)(ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr);
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\fault.txt", "a") == 0 && f) {
        fprintf(f, "%s stage=%ld code=0x%08lX addr=%p ga=%p off=%lld\n",
            tag, g_stage, code, (void*)addr, (void*)ga,
            ga ? (long long)(addr - (uintptr_t)ga) : -1LL);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
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
            __try { TickCheats(); }
            __except (LogFault(GetExceptionInformation(), "tick")) { }
        }
    }
    if (g_cheat.aura && now - lastAura >= 400) {
        lastAura = now;
        __try { ArenaAuraTick(); }
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
    if (!g_i2.ready) return;
    EnsureThread();
    void* list = PlayerActorList();
    int n = ListSize(list);
    for (int i = 0; i < n; i++) {
        void* actor = ListItem(list, i);
        if (!IsPlayerActorClass(actor)) continue;
        SetAmend(actor, ATTR_ATK, PARAM_PERCENTAGE, 0.0);
        SetAmend(actor, ATTR_DEF, PARAM_PERCENTAGE, 0.0);
        SetAmend(actor, ATTR_ATKSPD_P, PARAM_PERCENTAGE, 0.0);
        SetAmend(actor, ATTR_CRITRATE, PARAM_ABSOLUTE, 0.0);
        void* info = FieldPtr(actor, "actorInfo");
        if (info) {
            auto restore = [&](std::unordered_map<void*, int64_t>& cache, size_t off) {
                auto it = cache.find(actor);
                if (it != cache.end()) { *(int64_t*)((uint8_t*)info + off) = it->second; cache.erase(it); }
            };
            restore(g_runSpdBase, 0xe0);
            restore(g_walkSpdBase, 0x100);
            restore(g_atkSpdBase, 0x138);
        }
    }
    g_cheat.dmgMult = 1; g_cheat.defMult = 1;
    g_cheat.atkSpdMult = 1.0f; g_cheat.moveSpdMult = 1.0f; g_cheat.gameSpeed = 1.0f;
    g_cheat.alwaysCrit = false;
    ApplyTimeScale(1.0f);
    g_prevDmg = 1; g_prevDef = 1; g_prevAtkSpd = 1.0f; g_prevMove = 1.0f; g_prevCrit = false;
    LogF("modifiers reset");
}


const char* CFG_PATH = "C:\\Users\\Ejemm\\OneDrive\\Desktop\\Stella Sora\\work\\stella_menu.ini";
float g_cfgX = -1, g_cfgY = -1, g_cfgW = 780, g_cfgH = 500;
int g_wallAlpha = 150;
int g_dimAlpha = 145;
float g_fontScale = 1.0f;

void LoadConfig() {
    FILE* f = nullptr;
    if (fopen_s(&f, CFG_PATH, "r") != 0 || !f) return;
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
        else if (!strcmp(key, "wallpaper")) g_wallAlpha = (int)v;
        else if (!strcmp(key, "backdrop")) g_dimAlpha = (int)v;
        else if (!strcmp(key, "font_scale")) g_fontScale = (float)v;
    }
    fclose(f);
    auto clampi = [](int& v, int lo, int hi) { if (v < lo) v = lo; if (v > hi) v = hi; };
    auto clampf = [](float& v, float lo, float hi) { if (v < lo) v = lo; if (v > hi) v = hi; };
    clampi(g_cheat.dmgMult, 1, 100);
    clampi(g_cheat.defMult, 1, 100);
    clampf(g_cheat.atkSpdMult, 1.0f, 2.0f);
    clampf(g_cheat.moveSpdMult, 1.0f, 2.5f);
    clampf(g_cheat.gameSpeed, 1.0f, 5.0f);
    clampi(g_cheat.auraDmg, 10, 2000000000);
    clampf(g_fontScale, 0.8f, 1.6f);
    clampf(g_cfgW, 640.0f, 1600.0f);
    clampf(g_cfgH, 360.0f, 1000.0f);
    LogF("config loaded");
}

void SaveConfig() {
    FILE* f = nullptr;
    if (fopen_s(&f, CFG_PATH, "w") != 0 || !f) return;
    fprintf(f, "pos_x=%.1f\npos_y=%.1f\nwidth=%.1f\nheight=%.1f\n", g_cfgX, g_cfgY, g_cfgW, g_cfgH);
    fprintf(f, "god=%d\nno_cd=%d\nenergy=%d\ndash=%d\ncrit=%d\naura=%d\naura_lethal=%d\nsafe_mode=%d\n",
        g_cheat.god, g_cheat.noCd, g_cheat.infEnergy, g_cheat.unlimitedDash,
        g_cheat.alwaysCrit, g_cheat.aura, g_cheat.auraLethal, g_cheat.safeMode);
    fprintf(f, "damage=%d\ndefense=%d\natk_speed=%.2f\nmove_speed=%.2f\ngame_speed=%.2f\naura_damage=%d\n",
        g_cheat.dmgMult, g_cheat.defMult, g_cheat.atkSpdMult, g_cheat.moveSpdMult,
        g_cheat.gameSpeed, g_cheat.auraDmg);
    fprintf(f, "wallpaper=%d\nbackdrop=%d\nfont_scale=%.2f\n", g_wallAlpha, g_dimAlpha, g_fontScale);
    fclose(f);
}

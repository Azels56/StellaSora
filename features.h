#pragma once
// Gameplay features: attributes, skills, aura, cooldowns, config.
#include <windows.h>
#include <cstdint>
#include <unordered_map>

struct CheatState {
    bool god = false;
    bool noCd = false;
    bool infEnergy = false;
    bool unlimitedDash = false;
    bool alwaysCrit = false;
    bool aura = false;
    bool auraLethal = true;
    bool safeMode = true;
    bool instantBreak = false;   // drop toughness to zero and break the guard
    bool noShield = false;       // strips the shield layer off every enemy
    bool autoSkipStory = false;  // drive the story/dialog panel's skip path
    bool autoSelectCards = false;// take a card as soon as the choice opens
    int dmgMult = 1;
    int defMult = 1;
    int multiHit = 1;            // damage instances per hit, 1 = off
    float atkSpdMult = 1.0f;
    float moveSpdMult = 1.0f;
    float gameSpeed = 1.0f;
    int auraDmg = 5000;
};

extern CheatState g_cheat;
extern volatile LONG g_stage;

enum : int {
    PARAM_BASE = 0,
    PARAM_PERCENTAGE = 1,
    PARAM_ABSOLUTE = 2,
    ATTR_ATK = 1,
    ATTR_DEF = 2,
    ATTR_MAXHP = 3,
    ATTR_CRITRATE = 6,
    ATTR_MOVESPEED = 45,
    ATTR_ATKSPD_P = 46,
};

// Live status shown by the overlay.
extern int g_lastPlayers;
extern int64_t g_lastHp, g_lastMax, g_lastEnergy;
extern bool g_lastAlive;
extern int g_auraHitsLast, g_auraFallbackHits;
extern int64_t g_auraHpBefore, g_auraHpAfter;
extern double g_dbgRunSpeed;
extern float g_appliedTimeScale;

// Appearance / window settings (persisted).
extern float g_cfgX, g_cfgY, g_cfgW, g_cfgH;
extern int g_wallAlpha, g_dimAlpha;
extern float g_fontScale;
// Config file path, decoded on first use (the literal is masked in .rdata).
const char* CfgPath();

void LoadConfig();
void SaveConfig();
void ResetSkillsNow();
void ResetModifiers();
bool KillMobs();
void MaybeRunCheats();

// Overlay actions are queued and drained on the game thread. The action bar
// runs inside the Present hook, and IL2CPP objects may only be touched from
// the thread that owns them; calling ResetSkillsNow() from the render thread
// raced Unity's own updates and the click looked dead (or took the process
// with it).
enum : int { ACT_NONE = 0, ACT_RESET_SKILLS = 1, ACT_RESET_STATS = 2, ACT_KILL_MOBS = 3,
             ACT_SCAN_AUTOMATION = 4 };
void QueueAction(int action);
void RunPendingAction();

// ---------------------------------------------------------------- automation
// Story skip and card pick are driven through the game's own Lua layer: the
// AVG (visual novel) system is scripted there, not in the C# hotfix image, and
// it exposes EventManager:Hit(EventId.AvgSkip) as the skip path. The chunk in
// lua_bridge.cpp owns the calls; this side only carries the toggles.

// Writes every class whose name matches the story/card vocabulary, with its
// zero-arg methods, so the resolved names can be read back and locked in.
// Safe to call repeatedly; triggered by the Scan button or work\probe_auto.on.
void DumpAutomationApi(const char* path);

// Enemies seen by the last guard pass.
extern int g_toughMonsters;

// Extra damage instances the multi-hit replay has fired.
extern int g_multiHitExtra;

// True once the embedded Lua chunk has been loaded into a live Lua state.
extern bool g_luaScriptLoaded;

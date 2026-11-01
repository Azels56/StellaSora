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
    int dmgMult = 1;
    int defMult = 1;
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
extern void* g_canUseCdFn;

// Appearance / window settings (persisted).
extern float g_cfgX, g_cfgY, g_cfgW, g_cfgH;
extern int g_wallAlpha, g_dimAlpha;
extern float g_fontScale;
extern const char* CFG_PATH;

void LoadConfig();
void SaveConfig();
void ResetSkillsNow();
void ResetModifiers();
bool KillMobs();
void MaybeRunCheats();

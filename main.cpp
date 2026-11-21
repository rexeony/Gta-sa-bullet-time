// Bullet Time + Dodge for GTA San Andreas Android (AML, arm64-v8a)
// Ide: Max Payne (bullet time + shootdodge). Dibuat ulang dari nol, bukan port dari mod PC.
// Terinspirasi dari "Frnchise Bullet Time" oleh frnchise.
//
// Semua alamat game dicari lewat NAMA SIMBOL di libGTASA.so (tanpa offset hardcode).
// Hook hanya dipasang pada fungsi yang awalnya aman (tanpa instruksi PC-relatif di 5 instruksi pertama):
//   CPlayerPed::ProcessControl, RpAnimBlendClumpUpdateAnimations, CWidgetButtonHydraulics::Update

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

MYMODCFGNAME(net.hafizdms.sabullettime, Bullet Time Dodge, 1.1, hafizdMS/zidanaSQ, BulletTime)
NEEDGAME(com.rockstargames.gtasa)

// ---------------------------------------------------------------- game symbols
struct CVector { float x, y, z; };
typedef void*  (*GetPad_t)(int);
typedef void*  (*FindPlayerPed_t)(int);
typedef void*  (*FindPlayerVehicle_t)(int, bool);
typedef void   (*ApplyMoveForce_t)(void*, CVector);     // CVector lewat register s0-s2
typedef void   (*PedProcess_t)(void*);
typedef void   (*AnimUpdate_t)(void*, float, bool);
typedef void   (*HydraUpdate_t)(void*);
typedef bool   (*WidgetIsTouched_t)(void*, void*);      // virtual CWidget::IsTouched(CVector2D*) (vtable +160)
typedef void*  (*GetAnimationBlock_t)(const char*);
typedef void*  (*RegisterAnimBlock_t)(const char*);
typedef void   (*LoadAnimFile_t)(const char*);
typedef void*  (*GetAnimation_t)(const char*, const void*);
typedef void*  (*BlendAnimation_t)(void*, void*, int, float);

static GetPad_t           GetPad = NULL;
static FindPlayerPed_t    FindPlayerPed = NULL;
static FindPlayerVehicle_t FindPlayerVehicle = NULL;
static ApplyMoveForce_t   ApplyMoveForce = NULL;
static float*             pTimeScale = NULL;      // CTimer::ms_fTimeScale
static float*             pTimeStep = NULL;       // CTimer::ms_fTimeStep
static PedProcess_t       PlayerProcess_orig = NULL;
static AnimUpdate_t       AnimUpdate_orig = NULL;
static HydraUpdate_t      HydraUpdate_orig = NULL;
static GetAnimationBlock_t GetAnimationBlock = NULL;
static RegisterAnimBlock_t RegisterAnimBlock = NULL;
static LoadAnimFile_t     LoadAnimFile = NULL;
static GetAnimation_t     GetAnimationFn = NULL;
static BlendAnimation_t   BlendAnimation = NULL;

// ---------------------------------------------------------------- settings
struct Settings
{
    bool  enabled, debugPad, holdToActivate, autoBulletTime, hydraButton, hydraDodge, useIfp;
    bool  hookPlayer, hookAnim, hookHydra;
    int   activateBtn, diveBtn, animFlags;
    float animBlend, cjSpeed, worldSpeed, blendIn, blendOut, duration, refill, startMeter, activationMin;
    float tapWindow, diveForward, diveUp, diveSlow;
    char  ifpPath[400];
} S;

static const char* kPadNames[] = {
    "LeftStickX", "LeftStickY", "RightStickX", "RightStickY",
    "LeftShoulder1", "LeftShoulder2", "RightShoulder1", "RightShoulder2",
    "DPadUp", "DPadDown", "DPadLeft", "DPadRight",
    "Start", "Select", "ButtonSquare", "ButtonTriangle",
    "ButtonCross", "ButtonCircle", "ShockButtonL", "ShockButtonR"
};
static const int kPadCount = 20;
static const int kBtnSquare = 14;

static int PadIndex(const char* name)
{
    for(int i = 4; i < kPadCount; ++i) if(!strcasecmp(name, kPadNames[i])) return i;
    return -1;
}

static float CF(const char* k, float d, const char* s) { ConfigEntry* e = cfg->Bind(k, d, s); float v = e ? e->GetFloat() : d; cfg->ClearLast(); return v; }
static bool  CB(const char* k, bool d, const char* s)  { ConfigEntry* e = cfg->Bind(k, d, s); bool v = e ? e->GetBool() : d;   cfg->ClearLast(); return v; }
static void  CS(const char* k, const char* d, const char* s, char* out, size_t n)
{
    ConfigEntry* e = cfg->Bind(k, d, s);
    strncpy(out, e ? e->GetString() : d, n - 1); out[n - 1] = 0;
    cfg->ClearLast();
}
static int CPadBtn(const char* k, const char* d, const char* s)
{
    char buf[64]; CS(k, d, s, buf, sizeof(buf));
    return PadIndex(buf);
}

static void LoadSettings()
{
    S.enabled        = CB("Enabled", true, "Main");
    S.debugPad       = CB("DebugPad", false, "Main");
    S.hookPlayer     = CB("HookPlayer", true, "Debug");
    S.hookAnim       = CB("HookAnim", true, "Debug");
    S.hookHydra      = CB("HookHydra", true, "Debug");

    S.hydraButton    = CB("HydraulicsButtonOnFoot", true, "BulletTime");
    S.activateBtn    = CPadBtn("ActivateButton", "None", "BulletTime");
    S.holdToActivate = CB("HoldToActivate", false, "BulletTime");
    S.worldSpeed     = CF("WorldSpeed", 0.35f, "BulletTime");
    S.cjSpeed        = CF("CJSpeed", 0.6f, "BulletTime");
    S.blendIn        = CF("BlendIn", 0.12f, "BulletTime");
    S.blendOut       = CF("BlendOut", 0.25f, "BulletTime");
    S.duration       = CF("Duration", 14.0f, "BulletTime");
    S.refill         = CF("RefillPerSec", 0.04f, "BulletTime");
    S.startMeter     = CF("StartMeter", 1.0f, "BulletTime");
    S.activationMin  = CF("ActivationMinimum", 0.05f, "BulletTime");

    S.hydraDodge     = CB("HydraulicsDoubleTapDodge", true, "Dodge");
    S.tapWindow      = CF("DoubleTapWindow", 0.35f, "Dodge");
    S.diveBtn        = CPadBtn("DiveButton", "None", "Dodge");
    S.diveForward    = CF("DiveSpeed", 0.30f, "Dodge");
    S.diveUp         = CF("DiveUpSpeed", 0.20f, "Dodge");
    S.autoBulletTime = CB("AutoBulletTime", true, "Dodge");
    S.diveSlow       = CF("DiveSlowSeconds", 2.5f, "Dodge");
    S.useIfp         = CB("UseIFP", false, "Dodge");
    CS("IFPPath", "", "Dodge", S.ifpPath, sizeof(S.ifpPath));
    S.animFlags      = (int)CF("AnimFlags", 0.0f, "Dodge");
    S.animBlend      = CF("AnimBlend", 8.0f, "Dodge");
    cfg->Save();

    if(S.worldSpeed < 0.05f) S.worldSpeed = 0.05f;
    if(S.worldSpeed > 1.0f)  S.worldSpeed = 1.0f;
    if(S.cjSpeed < 0.05f) S.cjSpeed = 0.05f;
    if(S.cjSpeed > 1.0f)  S.cjSpeed = 1.0f;
    if(S.blendIn  < 0.01f) S.blendIn  = 0.01f;
    if(S.blendOut < 0.01f) S.blendOut = 0.01f;
    if(S.duration < 1.0f)  S.duration = 1.0f;
}

// ---------------------------------------------------------------- state
static float gMeter = 1.0f;
static float gScale = 1.0f;          // time scale milik mod yang sedang dipakai
static float gFactor = 1.0f;         // pengali kecepatan CJ relatif terhadap dunia (>=1)
static bool  gInPlayerProcess = false;
static bool  gManual = false;
static float gDiveSlow = 0.0f;
static bool  gHydraPrev = false, gHydraTap = false, gHydraLogged = false;
static float gHydraLast = -10.0f, gLastTap = -10.0f;
static bool  gPrevActivate = false, gPrevDive = false;
static int16_t gPrevPad[20];
static bool  gIfpTried = false;
static void* gDiveHier[4] = { NULL, NULL, NULL, NULL };
static const char* kDiveNames[4] = { "sd_dive_fwd", "sd_dive_bkw", "sd_dive_left", "sd_dive_right" };

static double RealNow()
{
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static float RealDelta()
{
    static double last = 0.0;
    double now = RealNow();
    float dt = last > 0.0 ? (float)(now - last) : 0.016f;
    last = now;
    if(dt < 0.0f) dt = 0.0f;
    if(dt > 0.1f) dt = 0.1f;
    return dt;
}

static inline bool BulletTimeOn() { return gManual || gDiveSlow > 0.0f; }

// ---------------------------------------------------------------- dodge
// Layout arm64 (diturunkan dari disassembly libGTASA.so):
//   CEntity: +0x08 m_placement{pos, heading(+0x14)}, +0x18 m_matrix*, +0x20 m_pRwObject (clump)
//   CPhysical: +104 m_vecMoveSpeed, +176 m_fMass
static bool GetPedForward(void* ped, float& fx, float& fy)
{
    uintptr_t base = (uintptr_t)ped;
    void* mat = *(void**)(base + 0x18);
    if(mat)
    {
        const float* m = (const float*)mat;
        fx = m[4]; fy = m[5];
        float len = sqrtf(fx * fx + fy * fy);
        if(len > 0.5f && len < 1.5f) { fx /= len; fy /= len; return true; }
    }
    float h = *(float*)(base + 0x14);
    if(h > -20.0f && h < 20.0f) { fx = -sinf(h); fy = cosf(h); return true; }
    return false;
}

static void LoadIfp()
{
    gIfpTried = true;
    if(!S.useIfp) return;
    if(!LoadAnimFile || !GetAnimationFn || !BlendAnimation || !RegisterAnimBlock) { logger->Error("IFP: fungsi CAnimManager tidak lengkap"); return; }

    char path[500];
    if(S.ifpPath[0]) snprintf(path, sizeof(path), "%s", S.ifpPath);
    else snprintf(path, sizeof(path), "%s/BulletTime/MP3SD.ifp", aml->GetAndroidDataPath());
    if(!aml->FileExists(path)) { logger->Error("IFP: file tidak ada: %s", path); return; }

    void* blk = GetAnimationBlock ? GetAnimationBlock("MP3SD") : NULL;
    if(!blk) blk = RegisterAnimBlock("MP3SD");
    LoadAnimFile(path);
    if(!blk && GetAnimationBlock) blk = GetAnimationBlock("MP3SD");
    if(!blk) { logger->Error("IFP: blok MP3SD tidak terdaftar"); return; }

    int found = 0;
    for(int i = 0; i < 4; ++i) { gDiveHier[i] = GetAnimationFn(kDiveNames[i], blk); if(gDiveHier[i]) ++found; }
    logger->Info("IFP: %d/4 animasi dive ditemukan", found);
}

static bool DoDodge(void* ped, int16_t* pad)
{
    if(!ApplyMoveForce || !ped) return false;
    float fx, fy;
    if(!GetPedForward(ped, fx, fy)) { logger->Error("Dodge: arah hadap ped tidak valid"); return false; }

    int dir = 0;                                 // 0 maju, 1 mundur, 2 kiri, 3 kanan
    int sx = pad[0], sy = pad[1];
    if(abs(sx) > 30 || abs(sy) > 30)
    {
        if(abs(sy) >= abs(sx)) dir = (sy > 0) ? 1 : 0;
        else dir = (sx > 0) ? 3 : 2;
    }
    float dx = fx, dy = fy;
    if(dir == 1) { dx = -fx; dy = -fy; }
    else if(dir == 2) { dx = -fy; dy = fx; }
    else if(dir == 3) { dx = fy; dy = -fx; }

    float mass = *(float*)((uintptr_t)ped + 176);
    if(!(mass > 1.0f && mass < 2000.0f)) mass = 70.0f;
    CVector force = { dx * S.diveForward * mass, dy * S.diveForward * mass, S.diveUp * mass };
    ApplyMoveForce(ped, force);

    bool animPlayed = false;
    if(gDiveHier[dir] && BlendAnimation)
    {
        void* clump = *(void**)((uintptr_t)ped + 0x20);
        if(clump) { BlendAnimation(clump, gDiveHier[dir], S.animFlags, S.animBlend); animPlayed = true; }
    }
    if(!animPlayed) pad[kBtnSquare] = 255;       // tanpa IFP: lompat bawaan supaya CJ terangkat

    if(S.autoBulletTime && gMeter > S.activationMin) gDiveSlow = S.diveSlow;
    return true;
}

// ---------------------------------------------------------------- per-frame logic (dipanggil dari CPlayerPed::ProcessControl)
static void Tick(void* ped)
{
    float dt = RealDelta();
    int16_t* pad = GetPad ? (int16_t*)GetPad(0) : NULL;
    bool inVehicle = FindPlayerVehicle && FindPlayerVehicle(0, false);
    bool canAct = pad && !inVehicle;             // tombol tidak bisa dipicu saat berkendara
    if(!canAct) { gManual = false; gDiveSlow = 0.0f; }

    if(!gIfpTried) LoadIfp();

    if(pad && S.debugPad)
    {
        for(int i = 4; i < kPadCount; ++i)
        {
            if(pad[i] != gPrevPad[i]) logger->Info("Pad %s = %d", kPadNames[i], (int)pad[i]);
            gPrevPad[i] = pad[i];
        }
    }

    // tombol hidrolik di mode jalan: 1x tap = bullet time on/off, 2x tap cepat = dodge
    if(gHydraTap)
    {
        gHydraTap = false;
        if(canAct)
        {
            float now = (float)RealNow();
            if(S.hydraDodge && now - gHydraLast <= S.tapWindow) { DoDodge(ped, pad); gHydraLast = -10.0f; }
            else
            {
                if(gManual) gManual = false;
                else if(gMeter > S.activationMin) gManual = true;
                gHydraLast = now;
            }
        }
    }

    // tombol pad opsional
    if(canAct && S.activateBtn >= 0)
    {
        bool down = pad[S.activateBtn] > 0;
        if(S.holdToActivate) gManual = down && gMeter > S.activationMin;
        else if(down && !gPrevActivate)
        {
            if(gManual) gManual = false;
            else if(gMeter > S.activationMin) gManual = true;
        }
        gPrevActivate = down;
    }
    if(canAct && S.diveBtn >= 0)
    {
        bool down = pad[S.diveBtn] > 0;
        if(down && !gPrevDive)
        {
            float now = (float)RealNow();
            if(now - gLastTap <= S.tapWindow) { DoDodge(ped, pad); gLastTap = -10.0f; }
            else gLastTap = now;
        }
        gPrevDive = down;
    }

    // meter adrenalin
    if(BulletTimeOn())
    {
        gMeter -= dt / S.duration;
        if(gDiveSlow > 0.0f) gDiveSlow -= dt;
        if(gMeter <= 0.0f) { gMeter = 0.0f; gManual = false; gDiveSlow = 0.0f; }
    }
    else
    {
        gMeter += S.refill * dt;
        if(gMeter > 1.0f) gMeter = 1.0f;
    }

    // time scale dunia: hanya disentuh selama bullet time/transisi (supaya tidak mengganggu misi & cutscene)
    if(pTimeScale && (BulletTimeOn() || gScale < 0.999f))
    {
        float target = BulletTimeOn() ? S.worldSpeed : 1.0f;
        float range = 1.0f - S.worldSpeed;
        float rate = range / (target < gScale ? S.blendIn : S.blendOut);
        if(gScale > target) { gScale -= rate * dt; if(gScale < target) gScale = target; }
        else if(gScale < target) { gScale += rate * dt; if(gScale > target) gScale = target; }
        *pTimeScale = gScale;
    }
    gFactor = (gScale < 0.999f && S.cjSpeed > gScale) ? (S.cjSpeed / gScale) : 1.0f;
}

// ---------------------------------------------------------------- hooks
static void PlayerProcess_hook(void* self)
{
    if(S.enabled) Tick(self);

    if(S.enabled && pTimeStep && gFactor > 1.001f)
    {
        float saved = *pTimeStep;
        *pTimeStep = saved * gFactor;            // CJ lebih cepat dari dunia
        gInPlayerProcess = true;
        PlayerProcess_orig(self);
        gInPlayerProcess = false;
        *pTimeStep = saved;
    }
    else PlayerProcess_orig(self);
}

static void AnimUpdate_hook(void* clump, float dt, bool onScreen)
{
    if(S.enabled && gFactor > 1.001f && !gInPlayerProcess && clump && FindPlayerPed)
    {
        void* ped = FindPlayerPed(0);
        if(ped && clump == *(void**)((uintptr_t)ped + 0x20)) dt *= gFactor;
    }
    AnimUpdate_orig(clump, dt, onScreen);
}

// Tombol hidrolik (CWidgetButtonHydraulics): +200 = aktif, +201 = tampil.
// Update() bawaan mengosongkan keduanya saat tidak di kendaraan; di sini diaktifkan lagi saat berjalan kaki.
static void HydraUpdate_hook(void* self)
{
    HydraUpdate_orig(self);
    if(!S.enabled || !S.hydraButton || !FindPlayerPed) return;

    void* ped = FindPlayerPed(0);
    if(!ped)
    {
        // tidak ada pemain (loading/menu): pastikan bullet time mati
        if(gScale < 0.999f && pTimeScale) { gScale = 1.0f; *pTimeScale = 1.0f; }
        gManual = false; gDiveSlow = 0.0f; gHydraPrev = false;
        return;
    }
    if(FindPlayerVehicle && FindPlayerVehicle(0, false)) { gHydraPrev = false; return; }

    uint8_t* w = (uint8_t*)self;
    if(!gHydraLogged)
    {
        gHydraLogged = true;
        logger->Info("Hydra widget (jalan kaki): aktif=%d tampil=%d flag140=0x%x", (int)w[200], (int)w[201], *(uint32_t*)(w + 140));
    }
    w[200] = 1;
    w[201] = 1;

    void** vt = *(void***)self;
    bool touched = ((WidgetIsTouched_t)vt[20])(self, NULL);
    if(touched && !gHydraPrev) gHydraTap = true;
    gHydraPrev = touched;
}

// ---------------------------------------------------------------- init
static uintptr_t Sym(void* lib, const char* name)
{
    uintptr_t a = aml->GetSym(lib, name);
    if(!a) logger->Error("Simbol tidak ditemukan: %s", name);
    return a;
}

ON_MOD_LOAD()
{
    logger->SetTag("BulletTimeSA64");
    LoadSettings();
    gMeter = S.startMeter < 0.0f ? 0.0f : (S.startMeter > 1.0f ? 1.0f : S.startMeter);
    if(!S.enabled) { logger->Info("Dimatikan lewat ini"); return; }

    void* lib = aml->GetLibHandle("libGTASA.so");
    if(!lib) { logger->Error("libGTASA.so tidak ditemukan"); return; }

    pTimeScale        = (float*)Sym(lib, "_ZN6CTimer13ms_fTimeScaleE");
    pTimeStep         = (float*)Sym(lib, "_ZN6CTimer12ms_fTimeStepE");
    GetPad            = (GetPad_t)Sym(lib, "_ZN4CPad6GetPadEi");
    FindPlayerPed     = (FindPlayerPed_t)Sym(lib, "_Z13FindPlayerPedi");
    FindPlayerVehicle = (FindPlayerVehicle_t)Sym(lib, "_Z17FindPlayerVehicleib");
    ApplyMoveForce    = (ApplyMoveForce_t)Sym(lib, "_ZN9CPhysical14ApplyMoveForceE7CVector");
    if(S.useIfp)
    {
        GetAnimationBlock = (GetAnimationBlock_t)Sym(lib, "_ZN12CAnimManager17GetAnimationBlockEPKc");
        RegisterAnimBlock = (RegisterAnimBlock_t)Sym(lib, "_ZN12CAnimManager17RegisterAnimBlockEPKc");
        LoadAnimFile      = (LoadAnimFile_t)Sym(lib, "_ZN12CAnimManager12LoadAnimFileEPKc");
        GetAnimationFn    = (GetAnimation_t)Sym(lib, "_ZN12CAnimManager12GetAnimationEPKcPK10CAnimBlock");
        BlendAnimation    = (BlendAnimation_t)Sym(lib, "_ZN12CAnimManager14BlendAnimationEP7RpClumpP19CAnimBlendHierarchyif");
    }

    uintptr_t playerProcess = Sym(lib, "_ZN10CPlayerPed14ProcessControlEv");
    uintptr_t animUpdate    = Sym(lib, "_Z32RpAnimBlendClumpUpdateAnimationsP7RpClumpfb");
    uintptr_t hydraUpdate   = Sym(lib, "_ZN23CWidgetButtonHydraulics6UpdateEv");

    if(S.hookPlayer && playerProcess && pTimeScale && GetPad)
        aml->Hook((void*)playerProcess, (void*)PlayerProcess_hook, (void**)&PlayerProcess_orig);
    else logger->Error("Hook pemain TIDAK dipasang (bullet time/dodge mati)");

    if(S.hookAnim && animUpdate)
        aml->Hook((void*)animUpdate, (void*)AnimUpdate_hook, (void**)&AnimUpdate_orig);

    if(S.hookHydra && S.hydraButton && hydraUpdate)
        aml->Hook((void*)hydraUpdate, (void*)HydraUpdate_hook, (void**)&HydraUpdate_orig);
    else if(S.hydraButton) logger->Error("Tombol hidrolik di mode jalan TIDAK dipasang");

    logger->Info("Siap v1.1 (hook: pemain=%d anim=%d hidrolik=%d, IFP=%d)",
                 (int)(PlayerProcess_orig != NULL), (int)(AnimUpdate_orig != NULL), (int)(HydraUpdate_orig != NULL), (int)S.useIfp);
}

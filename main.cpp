// Bullet Time for GTA San Andreas Android (AML, arm64-v8a)
// Terinspirasi dari "Frnchise Bullet Time" oleh frnchise. Kode ditulis ulang dari nol.
//
// Fitur: slow motion dunia dengan transisi halus, CJ lebih cepat dari dunia, bar adrenalin,
//        efek layar (tint biru + letterbox), tombol hidrolik yang dimunculkan saat berjalan kaki.
// Semua alamat game dicari lewat NAMA SIMBOL di libGTASA.so (tanpa offset hardcode).

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
#include <stdarg.h>

MYMODCFGNAME(net.hafizdms.sabullettime, Bullet Time, 2.0, hafizdMS/zidanaSQ, BulletTime)
NEEDGAME(com.rockstargames.gtasa)

// ---------------------------------------------------------------- game symbols
struct CRect { float left, bottom, right, top; };      // bottom = y lebih besar (layar y ke bawah)
struct CRGBA { uint8_t r, g, b, a; };
typedef void*  (*GetPad_t)(int);
typedef void*  (*FindPlayerPed_t)(int);
typedef void*  (*FindPlayerVehicle_t)(int, bool);
typedef void   (*PedProcess_t)(void*);
typedef void   (*AnimUpdate_t)(void*, float, bool);
typedef void   (*HydraUpdate_t)(void*);
typedef void   (*HydraDraw_t)(void*);
typedef bool   (*WidgetIsTouched_t)(void*, void*);      // virtual CWidget::IsTouched(CVector2D*) (vtable +160)
typedef void   (*DrawRect_t)(const CRect&, const CRGBA&); // CSprite2d::DrawRect

static GetPad_t            GetPad = NULL;
static FindPlayerPed_t     FindPlayerPed = NULL;
static FindPlayerVehicle_t FindPlayerVehicle = NULL;
static float*              pTimeScale = NULL;      // CTimer::ms_fTimeScale
static float*              pTimeStep = NULL;       // CTimer::ms_fTimeStep
static uint8_t*            pUserPause = NULL;      // CTimer::m_UserPause (menu/pause)
static uint8_t*            pCodePause = NULL;      // CTimer::m_CodePause
static DrawRect_t          DrawRectFn = NULL;
static uint8_t*            pRsGlobal = NULL;       // RsGlobal: +8 lebar layar, +12 tinggi layar
static PedProcess_t        PlayerProcess_orig = NULL;
static AnimUpdate_t        AnimUpdate_orig = NULL;
static HydraUpdate_t       HydraUpdate_orig = NULL;
static HydraDraw_t         HydraDraw_orig = NULL;

static inline bool GamePaused() { return (pUserPause && *pUserPause) || (pCodePause && *pCodePause); }

// ---------------------------------------------------------------- settings
struct Settings
{
    bool  enabled, debugPad, hydraButton, showBar, tint, letterbox;
    bool  hookPlayer, hookAnim, hookHydra;
    int   activateBtn;
    unsigned hydraFlagClear, hydraFlagSet;
    float cjSpeed, worldSpeed, blendIn, blendOut, duration, refill, startMeter, activationMin;
    float barX, barY, barW, barH;
    float tintAlpha, letterboxH, letterboxAlpha;
} S;

static const char* kPadNames[] = {
    "LeftStickX", "LeftStickY", "RightStickX", "RightStickY",
    "LeftShoulder1", "LeftShoulder2", "RightShoulder1", "RightShoulder2",
    "DPadUp", "DPadDown", "DPadLeft", "DPadRight",
    "Start", "Select", "ButtonSquare", "ButtonTriangle",
    "ButtonCross", "ButtonCircle", "ShockButtonL", "ShockButtonR"
};
static const int kPadCount = 20;

static int PadIndex(const char* name)
{
    for(int i = 4; i < kPadCount; ++i) if(!strcasecmp(name, kPadNames[i])) return i;
    return -1;
}

static float CF(const char* k, float d, const char* s) { ConfigEntry* e = cfg->Bind(k, d, s); float v = e ? e->GetFloat() : d; cfg->ClearLast(); return v; }
static bool  CB(const char* k, bool d, const char* s)  { ConfigEntry* e = cfg->Bind(k, d, s); bool v = e ? e->GetBool() : d;   cfg->ClearLast(); return v; }
static int   CPadBtn(const char* k, const char* d, const char* s)
{
    ConfigEntry* e = cfg->Bind(k, d, s);
    char buf[64]; strncpy(buf, e ? e->GetString() : d, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    cfg->ClearLast();
    return PadIndex(buf);
}
static float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void LoadSettings()
{
    S.enabled        = CB("Enabled", true, "Main");
    S.debugPad       = CB("DebugPad", false, "Main");
    S.hookPlayer     = CB("HookPlayer", true, "Debug");
    S.hookAnim       = CB("HookAnim", true, "Debug");
    S.hookHydra      = CB("HookHydra", true, "Debug");
    S.hydraFlagClear = (unsigned)CF("HydraFlagClear", 4.0f, "Debug");
    S.hydraFlagSet   = (unsigned)CF("HydraFlagSet", 0.0f, "Debug");

    S.hydraButton    = CB("HydraulicsButtonOnFoot", true, "BulletTime");
    S.activateBtn    = CPadBtn("ActivateButton", "None", "BulletTime");
    S.worldSpeed     = CF("WorldSpeed", 0.30f, "BulletTime");
    S.cjSpeed        = CF("CJSpeed", 0.65f, "BulletTime");
    S.blendIn        = CF("BlendIn", 0.35f, "BulletTime");
    S.blendOut       = CF("BlendOut", 0.55f, "BulletTime");
    S.duration       = CF("Duration", 14.0f, "BulletTime");
    S.refill         = CF("RefillPerSec", 0.04f, "BulletTime");
    S.startMeter     = CF("StartMeter", 1.0f, "BulletTime");
    S.activationMin  = CF("ActivationMinimum", 0.05f, "BulletTime");

    S.showBar        = CB("ShowBar", true, "Bar");
    S.barX           = CF("BarX", 0.35f, "Bar");
    S.barY           = CF("BarY", 0.04f, "Bar");
    S.barW           = CF("BarWidth", 0.30f, "Bar");
    S.barH           = CF("BarHeight", 0.022f, "Bar");

    S.tint           = CB("ScreenTint", true, "Effect");
    S.tintAlpha      = CF("TintAlpha", 38.0f, "Effect");               // 0..255 saat bullet time penuh
    S.letterbox      = CB("Letterbox", true, "Effect");
    S.letterboxH     = CF("LetterboxHeight", 0.07f, "Effect");        // pecahan tinggi layar
    S.letterboxAlpha = CF("LetterboxAlpha", 150.0f, "Effect");
    cfg->Save();

    S.worldSpeed = Clamp(S.worldSpeed, 0.05f, 1.0f);
    S.cjSpeed    = Clamp(S.cjSpeed, 0.05f, 1.0f);
    S.blendIn    = Clamp(S.blendIn, 0.02f, 5.0f);
    S.blendOut   = Clamp(S.blendOut, 0.02f, 5.0f);
    if(S.duration < 1.0f) S.duration = 1.0f;
}

// log ke file (mudah dikirim): <AndroidData>/BulletTime_log.txt
static void FLog(const char* fmt, ...)
{
    char msg[300];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
    logger->Info("%s", msg);
    char path[500]; snprintf(path, sizeof(path), "%s/BulletTime_log.txt", aml->GetAndroidDataPath());
    FILE* f = fopen(path, "a");
    if(f) { fprintf(f, "%s\n", msg); fclose(f); }
}

// ---------------------------------------------------------------- state
static float gMeter = 1.0f;
static float gBlend = 0.0f;          // 0..1 kemajuan transisi bullet time
static float gScale = 1.0f;          // time scale dunia yang dipakai
static float gFactor = 1.0f;         // pengali kecepatan CJ relatif terhadap dunia (>=1)
static bool  gInPlayerProcess = false;
static bool  gManual = false;
static bool  gWasActive = false;
static bool  gHydraPrev = false, gHydraTap = false, gHydraLogged = false;
static bool  gPrevActivate = false, gOnFootNow = false;
static int16_t gPrevPad[20];

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
    return Clamp(dt, 0.0f, 0.1f);
}

static inline float Smooth(float t) { t = Clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }   // easing halus

// ---------------------------------------------------------------- per-frame logic (dari CPlayerPed::ProcessControl)
static void Tick()
{
    float dt = RealDelta();
    int16_t* pad = GetPad ? (int16_t*)GetPad(0) : NULL;
    bool inVehicle = FindPlayerVehicle && FindPlayerVehicle(0, false);
    bool canAct = !inVehicle;                    // tidak aktif saat berkendara
    if(!canAct) gManual = false;

    if(pad && S.debugPad)
    {
        for(int i = 4; i < kPadCount; ++i)
        {
            if(pad[i] != gPrevPad[i]) logger->Info("Pad %s = %d", kPadNames[i], (int)pad[i]);
            gPrevPad[i] = pad[i];
        }
    }

    // tombol hidrolik di mode jalan: tap sekali = bullet time on, tap lagi = off
    if(canAct && S.hydraButton && gHydraTap)
    {
        if(gManual) gManual = false;
        else if(gMeter > S.activationMin) gManual = true;
    }
    gHydraTap = false;

    // tombol pad opsional
    if(canAct && pad && S.activateBtn >= 0)
    {
        bool down = pad[S.activateBtn] > 0;
        if(down && !gPrevActivate)
        {
            if(gManual) gManual = false;
            else if(gMeter > S.activationMin) gManual = true;
        }
        gPrevActivate = down;
    }

    // meter adrenalin
    if(gManual)
    {
        gMeter -= dt / S.duration;
        if(gMeter <= 0.0f) { gMeter = 0.0f; gManual = false; }
    }
    else gMeter = Clamp(gMeter + S.refill * dt, 0.0f, 1.0f);

    // transisi halus (easing) masuk/keluar bullet time
    float target = gManual ? 1.0f : 0.0f;
    if(gBlend < target) gBlend = Clamp(gBlend + dt / S.blendIn, 0.0f, 1.0f);
    else if(gBlend > target) gBlend = Clamp(gBlend - dt / S.blendOut, 0.0f, 1.0f);
    gScale = 1.0f - (1.0f - S.worldSpeed) * Smooth(gBlend);

    // time scale dunia: hanya disentuh selama bullet time/transisi (tidak mengganggu misi & cutscene)
    if(pTimeScale)
    {
        if(gBlend > 0.0f) { *pTimeScale = gScale; gWasActive = true; }
        else if(gWasActive) { *pTimeScale = 1.0f; gScale = 1.0f; gWasActive = false; }
    }
    gFactor = (gScale < 0.999f && S.cjSpeed > gScale) ? (S.cjSpeed / gScale) : 1.0f;
}

// ---------------------------------------------------------------- hooks
static void PlayerProcess_hook(void* self)
{
    bool run = S.enabled && !GamePaused();
    if(run) Tick();
    else if(gWasActive && pTimeScale && GamePaused()) { /* biarkan: game sedang pause */ }

    if(run && pTimeStep && gFactor > 1.001f)
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

// Tombol hidrolik (CWidgetButtonHydraulics): +200 aktif, +201 tampil, +88 alpha, +89 enabled, +140 flag.
// Update() bawaan mengosongkan nilai itu saat tidak di kendaraan; di sini diaktifkan lagi saat berjalan kaki.
static void HydraUpdate_hook(void* self)
{
    HydraUpdate_orig(self);
    if(!S.enabled || !S.hydraButton || !FindPlayerPed || GamePaused()) return;
    uint8_t* w = (uint8_t*)self;
    uint32_t* fl = (uint32_t*)(w + 140);

    void* ped = FindPlayerPed(0);
    if(!ped)
    {
        // tidak ada pemain (loading/menu): pastikan bullet time mati
        if(gWasActive && pTimeScale) { *pTimeScale = 1.0f; gWasActive = false; }
        gManual = false; gBlend = 0.0f; gScale = 1.0f; gOnFootNow = false; gHydraPrev = false;
        return;
    }
    if(FindPlayerVehicle && FindPlayerVehicle(0, false)) { gOnFootNow = false; gHydraPrev = false; return; }

    gOnFootNow = true;
    if(!gHydraLogged) { gHydraLogged = true; FLog("Tombol hidrolik aktif saat jalan kaki (flag140=0x%x)", *fl); }
    *fl = (*fl & ~S.hydraFlagClear) | S.hydraFlagSet;
    w[200] = 1; w[201] = 1; w[89] = 1; w[88] = 255;

    void** vt = *(void***)self;
    bool touched = ((WidgetIsTouched_t)vt[20])(self, NULL);
    if(touched && !gHydraPrev)
    {
        gHydraTap = true;
        static int taps = 0;
        if(++taps <= 8) FLog("Tombol hidrolik ditekan (#%d) bullet=%d meter=%.2f", taps, (int)gManual, gMeter);
    }
    gHydraPrev = touched;
}

// ---------------------------------------------------------------- tampilan (digambar dari hook Draw tombol hidrolik)
static void Rect(float l, float t, float r, float b, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    CRect rc = { l, b, r, t };                   // CRect(left, bottom, right, top); bottom = y lebih besar
    CRGBA col = { R, G, B, A };
    DrawRectFn(rc, col);
}

static void DrawEffects()
{
    if(!DrawRectFn || !pRsGlobal) return;
    int W = *(int*)(pRsGlobal + 8), H = *(int*)(pRsGlobal + 12);
    if(W < 200 || W > 10000 || H < 200 || H > 10000) return;

    float t = Smooth(gBlend);                    // 0..1 seberapa dalam bullet time
    if(t > 0.01f)
    {
        if(S.tint) Rect(0, 0, (float)W, (float)H, 30, 80, 150, (uint8_t)Clamp(S.tintAlpha * t, 0.0f, 255.0f));
        if(S.letterbox)
        {
            float bh = S.letterboxH * H * t;
            uint8_t a = (uint8_t)Clamp(S.letterboxAlpha * t, 0.0f, 255.0f);
            Rect(0, 0, (float)W, bh, 0, 0, 0, a);
            Rect(0, (float)H - bh, (float)W, (float)H, 0, 0, 0, a);
        }
    }

    if(!S.showBar) return;
    float x = S.barX * W, y = S.barY * H, w = S.barW * W, h = S.barH * H;
    float pad = h * 0.18f; if(pad < 1.5f) pad = 1.5f;
    Rect(x - 1.0f, y - 1.0f, x + w + 1.0f, y + h + 1.0f, 0, 0, 0, 200);
    Rect(x, y, x + w, y + h, 25, 25, 30, 170);
    float m = Clamp(gMeter, 0.0f, 1.0f);
    float fw = (w - 2.0f * pad) * m;
    if(fw > 0.5f)
    {
        uint8_t R, G, B, A;
        if(gManual) { R = 90; G = 210; B = 255; A = 235; }          // aktif
        else if(m < 0.25f) { R = 255; G = 130; B = 60; A = 220; }   // sedang mengisi
        else { R = 235; G = 235; B = 235; A = 210; }
        Rect(x + pad, y + pad, x + pad + fw, y + h - pad, R, G, B, A);
    }
}

static void HydraDraw_hook(void* self)
{
    HydraDraw_orig(self);
    if(S.enabled && gOnFootNow && !GamePaused()) DrawEffects();
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
    gMeter = Clamp(S.startMeter, 0.0f, 1.0f);
    if(!S.enabled) { logger->Info("Dimatikan lewat ini"); return; }

    void* lib = aml->GetLibHandle("libGTASA.so");
    if(!lib) { logger->Error("libGTASA.so tidak ditemukan"); return; }

    pTimeScale        = (float*)Sym(lib, "_ZN6CTimer13ms_fTimeScaleE");
    pTimeStep         = (float*)Sym(lib, "_ZN6CTimer12ms_fTimeStepE");
    pUserPause        = (uint8_t*)Sym(lib, "_ZN6CTimer11m_UserPauseE");
    pCodePause        = (uint8_t*)Sym(lib, "_ZN6CTimer11m_CodePauseE");
    GetPad            = (GetPad_t)Sym(lib, "_ZN4CPad6GetPadEi");
    FindPlayerPed     = (FindPlayerPed_t)Sym(lib, "_Z13FindPlayerPedi");
    FindPlayerVehicle = (FindPlayerVehicle_t)Sym(lib, "_Z17FindPlayerVehicleib");
    DrawRectFn        = (DrawRect_t)Sym(lib, "_ZN9CSprite2d8DrawRectERK5CRectRK5CRGBA");
    pRsGlobal         = (uint8_t*)Sym(lib, "RsGlobal");

    uintptr_t playerProcess = Sym(lib, "_ZN10CPlayerPed14ProcessControlEv");
    uintptr_t animUpdate    = Sym(lib, "_Z32RpAnimBlendClumpUpdateAnimationsP7RpClumpfb");
    uintptr_t hydraUpdate   = Sym(lib, "_ZN23CWidgetButtonHydraulics6UpdateEv");
    uintptr_t hydraDraw     = Sym(lib, "_ZN23CWidgetButtonHydraulics4DrawEv");

    if(S.hookPlayer && playerProcess && pTimeScale)
        aml->Hook((void*)playerProcess, (void*)PlayerProcess_hook, (void**)&PlayerProcess_orig);
    else logger->Error("Hook pemain TIDAK dipasang (bullet time mati)");

    if(S.hookAnim && animUpdate)
        aml->Hook((void*)animUpdate, (void*)AnimUpdate_hook, (void**)&AnimUpdate_orig);

    if(S.hookHydra && S.hydraButton && hydraUpdate)
    {
        aml->Hook((void*)hydraUpdate, (void*)HydraUpdate_hook, (void**)&HydraUpdate_orig);
        if(hydraDraw) aml->Hook((void*)hydraDraw, (void*)HydraDraw_hook, (void**)&HydraDraw_orig);
    }
    else if(S.hydraButton) logger->Error("Tombol hidrolik di mode jalan TIDAK dipasang");

    FLog("=== BulletTimeSA64 v2.0 dimulai (hook: pemain=%d anim=%d hidrolik=%d) ===",
         (int)(PlayerProcess_orig != NULL), (int)(AnimUpdate_orig != NULL), (int)(HydraUpdate_orig != NULL));
}

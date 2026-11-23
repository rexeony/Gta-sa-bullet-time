// Bullet Time for GTA San Andreas Android (AML, arm64-v8a)
// Terinspirasi dari "Frnchise Bullet Time" oleh frnchise. Kode ditulis ulang dari nol.
//
// Fitur: slow motion dunia dengan transisi halus (jalan kaki DAN berkendara), CJ lebih cepat dari dunia,
//        bar adrenalin, efek layar (tint biru + letterbox), tombol bullet time sendiri di layar (tap on/off, tahan lalu seret untuk memindahkan).
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

MYMODCFGNAME(net.hafizdms.sabullettime, Bullet Time, 2.2, hafizdMS/zidanaSQ, BulletTime)
NEEDGAME(com.rockstargames.gtasa)

// ---------------------------------------------------------------- game symbols
struct CRect { float left, bottom, right, top; };      // bottom = y lebih besar (layar y ke bawah)
struct CRGBA { uint8_t r, g, b, a; };
struct Vec2  { float x, y; };
typedef void*  (*GetPad_t)(int);
typedef void*  (*FindPlayerPed_t)(int);
typedef void*  (*FindPlayerVehicle_t)(int, bool);
typedef void   (*PedProcess_t)(void*);
typedef void   (*AnimUpdate_t)(void*, float, bool);
typedef void   (*DrawAll_t)(bool);                      // CTouchInterface::DrawAll(bool)
typedef bool   (*IsTouchDown_t)(int);                   // CTouchInterface::IsTouchDown(int)
typedef Vec2   (*GetTouchPosition_t)(int);              // CTouchInterface::GetTouchPosition(int) -> s0,s1
typedef void   (*DrawRect_t)(const CRect&, const CRGBA&); // CSprite2d::DrawRect

static GetPad_t            GetPad = NULL;
static FindPlayerPed_t     FindPlayerPed = NULL;
static FindPlayerVehicle_t FindPlayerVehicle = NULL;
static IsTouchDown_t       IsTouchDownFn = NULL;
static GetTouchPosition_t  GetTouchPosFn = NULL;
static int*                pMaxTouches = NULL;     // CTouchInterface::m_nMaxTouches
static float*              pTimeScale = NULL;      // CTimer::ms_fTimeScale
static float*              pTimeStep = NULL;       // CTimer::ms_fTimeStep
static uint8_t*            pUserPause = NULL;      // CTimer::m_UserPause (menu/pause)
static uint8_t*            pCodePause = NULL;      // CTimer::m_CodePause
static uint8_t*            pCutscene = NULL;       // CCutsceneMgr::ms_running
static DrawRect_t          DrawRectFn = NULL;
static uint8_t*            pRsGlobal = NULL;       // RsGlobal: +8 lebar layar, +12 tinggi layar
static PedProcess_t        PlayerProcess_orig = NULL;
static AnimUpdate_t        AnimUpdate_orig = NULL;
static DrawAll_t           DrawAll_orig = NULL;

static inline bool GamePaused() { return (pUserPause && *pUserPause) || (pCodePause && *pCodePause); }
static inline bool InCutscene() { return pCutscene && *pCutscene; }

// ---------------------------------------------------------------- settings
struct Settings
{
    bool  enabled, debugPad, debugTouch, showButton, showBar, tint, letterbox, vehicleBoost;
    bool  hookPlayer, hookAnim, hookDraw;
    int   activateBtn;
    float cjSpeed, vehicleSpeed, worldSpeed, blendIn, blendOut, duration, refill, startMeter, activationMin;
    float btnX, btnY, btnSize, btnAlpha, btnHold;
    bool  btnMovable;
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
    S.debugTouch     = CB("DebugTouch", false, "Main");           // catat koordinat sentuhan (untuk menyetel tombol)
    S.hookPlayer     = CB("HookPlayer", true, "Debug");
    S.hookAnim       = CB("HookAnim", true, "Debug");
    S.hookDraw       = CB("HookDraw", true, "Debug");

    S.activateBtn    = CPadBtn("ActivateButton", "None", "BulletTime");   // tombol pad opsional
    S.worldSpeed     = CF("WorldSpeed", 0.30f, "BulletTime");
    S.cjSpeed        = CF("CJSpeed", 0.65f, "BulletTime");
    S.vehicleBoost   = CB("VehicleBoost", false, "BulletTime");           // eksperimen: kendaraan pemain lebih cepat dari dunia
    S.vehicleSpeed   = CF("VehicleSpeed", 0.50f, "BulletTime");
    S.blendIn        = CF("BlendIn", 0.35f, "BulletTime");
    S.blendOut       = CF("BlendOut", 0.55f, "BulletTime");
    S.duration       = CF("Duration", 14.0f, "BulletTime");
    S.refill         = CF("RefillPerSec", 0.04f, "BulletTime");
    S.startMeter     = CF("StartMeter", 1.0f, "BulletTime");
    S.activationMin  = CF("ActivationMinimum", 0.05f, "BulletTime");

    S.showButton     = CB("ShowButton", true, "Button");
    S.btnX           = CF("ButtonX", 0.03f, "Button");        // posisi kiri tombol (pecahan lebar layar)
    S.btnY           = CF("ButtonY", 0.42f, "Button");        // posisi atas tombol (pecahan tinggi layar)
    S.btnSize        = CF("ButtonSize", 0.11f, "Button");     // ukuran tombol (pecahan tinggi layar)
    S.btnAlpha       = CF("ButtonAlpha", 210.0f, "Button");
    S.btnMovable     = CB("ButtonMovable", true, "Button");   // 1 = tahan tombol lalu seret untuk memindahkan
    S.btnHold        = CF("ButtonHoldTime", 0.6f, "Button");  // lama tahan (detik) sebelum mode geser aktif

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

    S.worldSpeed   = Clamp(S.worldSpeed, 0.05f, 1.0f);
    S.cjSpeed      = Clamp(S.cjSpeed, 0.05f, 1.0f);
    S.vehicleSpeed = Clamp(S.vehicleSpeed, 0.05f, 1.0f);
    S.blendIn      = Clamp(S.blendIn, 0.02f, 5.0f);
    S.blendOut     = Clamp(S.blendOut, 0.02f, 5.0f);
    S.btnSize      = Clamp(S.btnSize, 0.04f, 0.4f);
    S.btnHold      = Clamp(S.btnHold, 0.25f, 3.0f);
    S.btnX         = Clamp(S.btnX, 0.0f, 1.0f);
    S.btnY         = Clamp(S.btnY, 0.0f, 1.0f);
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
static float gFactorVeh = 1.0f;      // pengali kecepatan kendaraan pemain (>=1)
static bool  gInPlayerProcess = false;
static bool  gManual = false;
static bool  gWasActive = false;
static bool  gPrevActivate = false;
static int   gBtnTouch = -1;         // indeks jari yang sedang menyentuh tombol (-1 = tidak ada)
static double gBtnT0 = 0.0;          // waktu jari mulai menyentuh tombol
static float gBtnSX = 0, gBtnSY = 0; // posisi awal sentuhan (piksel)
static float gBtnOX = 0, gBtnOY = 0; // selisih titik sentuh terhadap pojok kiri-atas tombol
static bool  gBtnDrag = false;       // mode geser aktif
static bool  gBtnCancel = false;     // jari bergeser sebelum waktunya -> bukan tap, bukan geser
static float gBtnHoldFrac = 0.0f;    // 0..1 kemajuan tahan (untuk indikator)
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

static bool ScreenSize(int& W, int& H)
{
    if(!pRsGlobal) return false;
    W = *(int*)(pRsGlobal + 8); H = *(int*)(pRsGlobal + 12);
    return W >= 200 && W <= 10000 && H >= 200 && H <= 10000;
}

// area tombol dalam piksel layar (sedikit dibesarkan untuk hit-test)
static void ButtonRect(int W, int H, float& x, float& y, float& s)
{
    x = S.btnX * W; y = S.btnY * H; s = S.btnSize * H;
}

// simpan posisi tombol ke BulletTime.ini
static void SaveButtonPos()
{
    ConfigEntry* ex = cfg->Bind("ButtonX", S.btnX, "Button");
    if(ex) ex->SetFloat(S.btnX);
    cfg->ClearLast();
    ConfigEntry* ey = cfg->Bind("ButtonY", S.btnY, "Button");
    if(ey) ey->SetFloat(S.btnY);
    cfg->ClearLast();
    cfg->Save();
    FLog("Posisi tombol disimpan: ButtonX=%.3f ButtonY=%.3f", S.btnX, S.btnY);
}

static bool ReadTouch(int i, int W, int H, float& px, float& py)
{
    Vec2 p = GetTouchPosFn(i);
    px = p.x; py = p.y;
    if(px <= 1.01f && py <= 1.01f) { px *= W; py *= H; }       // koordinat ternormalisasi -> piksel
    return true;
}

// Mengembalikan true saat tombol di-TAP (jari diangkat sebelum waktu tahan, tanpa bergeser).
// Tahan tombol lalu seret = pindahkan tombol; posisi disimpan saat jari diangkat.
static bool PollButtonTap()
{
    if(!S.showButton || !IsTouchDownFn || !GetTouchPosFn) return false;
    int W, H; if(!ScreenSize(W, H)) return false;
    int n = pMaxTouches ? *pMaxTouches : 4;
    if(n < 1) n = 1; if(n > 16) n = 16;

    float bx, by, bs; ButtonRect(W, H, bx, by, bs);
    float m = bs * 0.12f;                                     // margin hit-test
    float moveTol = bs * 0.25f;                               // toleransi goyang jari saat menahan
    double now = RealNow();
    bool tap = false;

    if(gBtnTouch < 0)
    {
        // cari sentuhan baru di dalam tombol
        static bool prevDown[16];
        static int logged = 0;
        for(int i = 0; i < n; ++i)
        {
            bool down = IsTouchDownFn(i);
            if(down && !prevDown[i])
            {
                float px, py; ReadTouch(i, W, H, px, py);
                bool inside = px >= bx - m && px <= bx + bs + m && py >= by - m && py <= by + bs + m;
                if(S.debugTouch && logged < 40) { ++logged; FLog("Sentuh #%d: mentah=(%.1f,%.1f) layar=%dx%d tombol=(%.0f,%.0f,%.0f) dalam=%d", i, px, py, W, H, bx, by, bs, (int)inside); }
                if(inside && gBtnTouch < 0)
                {
                    gBtnTouch = i; gBtnT0 = now; gBtnSX = px; gBtnSY = py;
                    gBtnOX = px - bx; gBtnOY = py - by;
                    gBtnDrag = false; gBtnCancel = false; gBtnHoldFrac = 0.0f;
                }
            }
            prevDown[i] = down;
        }
        return false;
    }

    // ada jari yang sedang dilacak
    int i = gBtnTouch;
    bool down = IsTouchDownFn(i);
    if(down)
    {
        float px, py; ReadTouch(i, W, H, px, py);
        float held = (float)(now - gBtnT0);
        if(!gBtnDrag)
        {
            float dx = px - gBtnSX, dy = py - gBtnSY;
            if(dx * dx + dy * dy > moveTol * moveTol) gBtnCancel = true;   // bergeser sebelum waktunya
            gBtnHoldFrac = (S.btnMovable && !gBtnCancel) ? Clamp(held / S.btnHold, 0.0f, 1.0f) : 0.0f;
            if(S.btnMovable && !gBtnCancel && held >= S.btnHold)
            {
                gBtnDrag = true;
                FLog("Mode geser tombol aktif");
            }
        }
        if(gBtnDrag)
        {
            // tombol mengikuti jari; tetap di dalam layar
            float nx = (px - gBtnOX) / (float)W;
            float ny = (py - gBtnOY) / (float)H;
            float maxX = 1.0f - bs / (float)W, maxY = 1.0f - bs / (float)H;
            S.btnX = Clamp(nx, 0.0f, maxX < 0.0f ? 0.0f : maxX);
            S.btnY = Clamp(ny, 0.0f, maxY < 0.0f ? 0.0f : maxY);
        }
    }
    else
    {
        float held = (float)(now - gBtnT0);
        if(gBtnDrag) SaveButtonPos();
        else if(!gBtnCancel && held < S.btnHold) tap = true;       // tap singkat = toggle
        else if(!S.btnMovable && !gBtnCancel) tap = true;          // geser dimatikan: tahan lama tetap dianggap tap
        gBtnTouch = -1; gBtnDrag = false; gBtnCancel = false; gBtnHoldFrac = 0.0f;
        // jari lain yang masih turun tidak boleh langsung dianggap sentuhan baru
        // (prevDown dalam cabang pencarian akan diperbarui di frame berikutnya)
    }
    return tap;
}

// ---------------------------------------------------------------- per-frame logic (dari CPlayerPed::ProcessControl)
static void Tick()
{
    float dt = RealDelta();
    int16_t* pad = GetPad ? (int16_t*)GetPad(0) : NULL;

    if(pad && S.debugPad)
    {
        for(int i = 4; i < kPadCount; ++i)
        {
            if(pad[i] != gPrevPad[i]) logger->Info("Pad %s = %d", kPadNames[i], (int)pad[i]);
            gPrevPad[i] = pad[i];
        }
    }

    // tombol di layar: tap sekali = bullet time on, tap lagi = off (jalan kaki maupun berkendara)
    if(PollButtonTap())
    {
        static int taps = 0;
        if(++taps <= 8) FLog("Tombol bullet time ditekan (#%d) aktif=%d meter=%.2f kendaraan=%d", taps, (int)gManual, gMeter, (int)(FindPlayerVehicle && FindPlayerVehicle(0, false) != NULL));
        if(gManual) gManual = false;
        else if(gMeter > S.activationMin) gManual = true;
    }

    // tombol pad opsional
    if(pad && S.activateBtn >= 0)
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
    bool slow = gScale < 0.999f;
    gFactor    = (slow && S.cjSpeed > gScale) ? (S.cjSpeed / gScale) : 1.0f;
    gFactorVeh = (slow && S.vehicleBoost && S.vehicleSpeed > gScale) ? (S.vehicleSpeed / gScale) : 1.0f;
}

// ---------------------------------------------------------------- hooks
static void PlayerProcess_hook(void* self)
{
    bool run = S.enabled && !GamePaused() && !InCutscene();
    if(run) Tick();

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

// Kendaraan pemain lebih cepat dari dunia (opsional, eksperimen): langkah waktu khusus kendaraan pemain diperbesar
template<int N> struct VehHook
{
    static PedProcess_t orig;
    static void hook(void* self)
    {
        if(S.enabled && gFactorVeh > 1.001f && pTimeStep && FindPlayerVehicle && !GamePaused() && self && self == FindPlayerVehicle(0, false))
        {
            float saved = *pTimeStep;
            *pTimeStep = saved * gFactorVeh;
            orig(self);
            *pTimeStep = saved;
        }
        else orig(self);
    }
};
template<int N> PedProcess_t VehHook<N>::orig = NULL;

// ---------------------------------------------------------------- tampilan (digambar setelah semua widget sentuh)
static void Rect(float l, float t, float r, float b, uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    CRect rc = { l, b, r, t };                   // CRect(left, bottom, right, top); bottom = y lebih besar
    CRGBA col = { R, G, B, A };
    DrawRectFn(rc, col);
}

static void DrawUI()
{
    int W, H; if(!ScreenSize(W, H) || !DrawRectFn) return;

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

    float m = Clamp(gMeter, 0.0f, 1.0f);
    if(S.showBar)
    {
        float x = S.barX * W, y = S.barY * H, w = S.barW * W, h = S.barH * H;
        float pad = h * 0.18f; if(pad < 1.5f) pad = 1.5f;
        Rect(x - 1.0f, y - 1.0f, x + w + 1.0f, y + h + 1.0f, 0, 0, 0, 200);
        Rect(x, y, x + w, y + h, 25, 25, 30, 170);
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

    if(S.showButton)
    {
        float bx, by, bs; ButtonRect(W, H, bx, by, bs);
        uint8_t a = (uint8_t)Clamp(S.btnAlpha, 0.0f, 255.0f);
        float bd = bs * 0.05f; if(bd < 2.0f) bd = 2.0f;
        // bingkai + latar (biru saat aktif); saat mode geser bingkai jadi kuning
        if(gBtnDrag)
        {
            float g = bd * 1.8f;
            Rect(bx - g, by - g, bx + bs + g, by + bs + g, 255, 200, 40, 255);
        }
        Rect(bx, by, bx + bs, by + bs, 0, 0, 0, a);
        if(gManual) Rect(bx + bd, by + bd, bx + bs - bd, by + bs - bd, 40, 130, 215, a);
        else        Rect(bx + bd, by + bd, bx + bs - bd, by + bs - bd, 35, 38, 48, a);
        // ikon "jeda/slow": dua batang tegak
        float iw = bs * 0.14f, ih = bs * 0.46f, ix = bx + bs * 0.5f, iy = by + bs * 0.30f;
        Rect(ix - iw * 1.5f, iy, ix - iw * 0.5f, iy + ih, 255, 255, 255, a);
        Rect(ix + iw * 0.5f, iy, ix + iw * 1.5f, iy + ih, 255, 255, 255, a);
        // indikator tahan (garis kuning di atas tombol yang memanjang sampai mode geser aktif)
        if(gBtnHoldFrac > 0.02f && !gBtnDrag)
            Rect(bx, by - bd * 2.0f, bx + bs * gBtnHoldFrac, by, 255, 200, 40, 255);
        // meter mini di dasar tombol
        float mx0 = bx + bd * 2.0f, mx1 = bx + bs - bd * 2.0f, my1 = by + bs - bd * 2.0f, my0 = my1 - bs * 0.07f;
        Rect(mx0, my0, mx1, my1, 0, 0, 0, a);
        if(m > 0.01f) Rect(mx0, my0, mx0 + (mx1 - mx0) * m, my1, gManual ? 90 : 235, gManual ? 210 : 235, gManual ? 255 : 235, a);
    }
}

static void DrawAll_hook(bool overlay)
{
    DrawAll_orig(overlay);
    if(overlay || !S.enabled || GamePaused() || InCutscene()) return;   // gambar sekali per frame, setelah widget biasa
    if(!FindPlayerPed || !FindPlayerPed(0)) return;
    DrawUI();
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
    pCutscene         = (uint8_t*)Sym(lib, "_ZN12CCutsceneMgr10ms_runningE");
    GetPad            = (GetPad_t)Sym(lib, "_ZN4CPad6GetPadEi");
    FindPlayerPed     = (FindPlayerPed_t)Sym(lib, "_Z13FindPlayerPedi");
    FindPlayerVehicle = (FindPlayerVehicle_t)Sym(lib, "_Z17FindPlayerVehicleib");
    IsTouchDownFn     = (IsTouchDown_t)Sym(lib, "_ZN15CTouchInterface11IsTouchDownEi");
    GetTouchPosFn     = (GetTouchPosition_t)Sym(lib, "_ZN15CTouchInterface16GetTouchPositionEi");
    pMaxTouches       = (int*)Sym(lib, "_ZN15CTouchInterface13m_nMaxTouchesE");
    DrawRectFn        = (DrawRect_t)Sym(lib, "_ZN9CSprite2d8DrawRectERK5CRectRK5CRGBA");
    pRsGlobal         = (uint8_t*)Sym(lib, "RsGlobal");

    uintptr_t playerProcess = Sym(lib, "_ZN10CPlayerPed14ProcessControlEv");
    uintptr_t animUpdate    = Sym(lib, "_Z32RpAnimBlendClumpUpdateAnimationsP7RpClumpfb");
    uintptr_t drawAll       = Sym(lib, "_ZN15CTouchInterface7DrawAllEb");

    if(S.hookPlayer && playerProcess && pTimeScale)
        aml->Hook((void*)playerProcess, (void*)PlayerProcess_hook, (void**)&PlayerProcess_orig);
    else logger->Error("Hook pemain TIDAK dipasang (bullet time mati)");

    if(S.hookAnim && animUpdate)
        aml->Hook((void*)animUpdate, (void*)AnimUpdate_hook, (void**)&AnimUpdate_orig);

    if(S.hookDraw && drawAll && DrawRectFn)
        aml->Hook((void*)drawAll, (void*)DrawAll_hook, (void**)&DrawAll_orig);
    else logger->Error("Hook gambar TIDAK dipasang (tombol/bar/efek tidak tampil)");

    if(S.vehicleBoost)
    {
        uintptr_t va = Sym(lib, "_ZN11CAutomobile14ProcessControlEv");
        uintptr_t vb = Sym(lib, "_ZN5CBike14ProcessControlEv");
        uintptr_t vc = Sym(lib, "_ZN5CBoat14ProcessControlEv");
        if(va) aml->Hook((void*)va, (void*)VehHook<0>::hook, (void**)&VehHook<0>::orig);
        if(vb) aml->Hook((void*)vb, (void*)VehHook<1>::hook, (void**)&VehHook<1>::orig);
        if(vc) aml->Hook((void*)vc, (void*)VehHook<2>::hook, (void**)&VehHook<2>::orig);
    }

    FLog("=== BulletTimeSA64 v2.2 dimulai (hook: pemain=%d anim=%d gambar=%d, kendaraan boost=%d) ===",
         (int)(PlayerProcess_orig != NULL), (int)(AnimUpdate_orig != NULL), (int)(DrawAll_orig != NULL), (int)S.vehicleBoost);
}

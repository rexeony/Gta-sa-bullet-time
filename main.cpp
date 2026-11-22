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
#include <stdarg.h>

MYMODCFGNAME(net.hafizdms.sabullettime, Bullet Time Dodge, 1.2, hafizdMS/zidanaSQ, BulletTime)
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
typedef void   (*HydraDraw_t)(void*);
typedef bool   (*WidgetIsTouched_t)(void*, void*);      // virtual CWidget::IsTouched(CVector2D*) (vtable +160)
typedef void*  (*GetAnimationBlock_t)(const char*);
typedef void*  (*RegisterAnimBlock_t)(const char*);
struct RwMemory { uint8_t* start; uint32_t length; };
struct CRect { float left, bottom, right, top; };      // bottom = y lebih besar (layar y ke bawah)
struct CRGBA { uint8_t r, g, b, a; };
typedef void   (*DrawRect_t)(const CRect&, const CRGBA&); // CSprite2d::DrawRect
typedef void*  (*RwStreamOpen_t)(int, int, const void*);   // RwStreamOpen(type, access, data)
typedef int    (*RwStreamClose_t)(void*, void*);           // RwStreamClose(stream, data)
typedef void   (*LoadAnimFile_t)(void*, bool, const char (*)[32]); // CAnimManager::LoadAnimFile(RwStream*, bool, ...)
typedef void*  (*GetAnimation_t)(const char*, const void*);
typedef void*  (*BlendAnimation_t)(void*, void*, int, float);
typedef void*  (*TaskNew_t)(size_t);                                              // CTask::operator new
typedef void   (*RunNamedAnimCtor_t)(void*, const char*, const char*, int, float, int, bool, bool, bool, bool); // CTaskSimpleRunNamedAnim::ctor
typedef void   (*SetTask_t)(void*, void*, int, bool);
typedef void   (*SetTaskSecondary_t)(void*, void*, int);
typedef bool   (*MakeAbortable_t)(void*, void*, int, const void*);
typedef bool   (*PadBool_t)(void*);                 // CPad::GetEnterTargeting() / GetExitTargeting()
typedef bool   (*PadTarget_t)(void*, bool);         // CPad::GetTarget(bool)                // CTaskSimpleAnim::MakeAbortable(ped, priority, event)                           // CTaskManager::SetTaskSecondary(task, slot)                              // CTaskManager::SetTask(task, slot, bool)

static GetPad_t           GetPad = NULL;
static FindPlayerPed_t    FindPlayerPed = NULL;
static FindPlayerVehicle_t FindPlayerVehicle = NULL;
static ApplyMoveForce_t   ApplyMoveForce = NULL;
static float*             pTimeScale = NULL;      // CTimer::ms_fTimeScale
static float*             pTimeStep = NULL;       // CTimer::ms_fTimeStep
static PedProcess_t       PlayerProcess_orig = NULL;
static AnimUpdate_t       AnimUpdate_orig = NULL;
static HydraUpdate_t      HydraUpdate_orig = NULL;
static HydraDraw_t        HydraDraw_orig = NULL;
static GetAnimationBlock_t GetAnimationBlock = NULL;
static RegisterAnimBlock_t RegisterAnimBlock = NULL;
static LoadAnimFile_t     LoadAnimFile = NULL;
static TaskNew_t          TaskNewFn = NULL;
static RunNamedAnimCtor_t RunNamedCtor = NULL;
static SetTask_t          SetTaskFn = NULL;
static SetTaskSecondary_t SetTaskSecFn = NULL;
static MakeAbortable_t    MakeAbortableFn = NULL;
static PadBool_t          EnterT_orig = NULL, ExitT_orig = NULL;
static PadTarget_t        Target_orig = NULL;
static char               gBlockName[32] = {0};
static DrawRect_t         DrawRectFn = NULL;
static uint8_t*           pRsGlobal = NULL;        // RsGlobal: +8 lebar layar, +12 tinggi layar
static RwStreamOpen_t     RwStreamOpenFn = NULL;
static RwStreamClose_t    RwStreamCloseFn = NULL;
static GetAnimation_t     GetAnimationFn = NULL;
static BlendAnimation_t   BlendAnimation = NULL;

// ---------------------------------------------------------------- settings
struct Settings
{
    bool  enabled, debugPad, holdToActivate, autoBulletTime, hydraButton, hydraDodge, useIfp;
    bool  hookPlayer, hookAnim, hookHydra, showBar, fallbackJump;
    int   hydraMode, taskFlags;
    bool  useTask, useSecondary, freezeStick, holdLastFrame, forceAim, logAim, hookTarget, hookAimEnter, hookAimExit;
    float dashSeconds, diveDuration, diveAnimSpeed;
    float barX, barY, barW, barH;
    unsigned hydraFlagClear, hydraFlagSet;
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
    S.hydraFlagClear = (unsigned)CF("HydraFlagClear", 4.0f, "Debug");   // bit flag widget (+140) yang dihapus saat jalan kaki
    S.hydraFlagSet   = (unsigned)CF("HydraFlagSet", 0.0f, "Debug");     // bit flag widget yang ditambahkan saat jalan kaki

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

    S.showBar        = CB("ShowBar", true, "Bar");
    S.barX           = CF("BarX", 0.35f, "Bar");      // posisi kiri bar (pecahan lebar layar)
    S.barY           = CF("BarY", 0.04f, "Bar");      // posisi atas bar (pecahan tinggi layar)
    S.barW           = CF("BarWidth", 0.30f, "Bar");
    S.barH           = CF("BarHeight", 0.022f, "Bar");
    S.fallbackJump   = CB("FallbackJump", true, "Dodge");
    S.forceAim       = CB("ForceAimWhileDiving", true, "Aim");     // paksa mode bidik selama dive
    S.logAim         = CB("LogAim", true, "Aim");                  // catat perubahan status aim ke log
    S.hookTarget     = CB("HookGetTarget", true, "Aim");
    S.hookAimEnter   = CB("HookGetEnterTargeting", false, "Aim");  // opsional (uji bertahap)
    S.hookAimExit    = CB("HookGetExitTargeting", false, "Aim");   // opsional (uji bertahap)
    S.useTask        = CB("UseAnimTask", true, "Dodge");          // main animasi lewat task bawaan game (tidak ditimpa)
    S.useSecondary   = CB("AnimInSecondarySlot", true, "Dodge");   // slot sekunder (parsial): kontrol & aim pemain tetap jalan
    S.taskFlags      = (int)CF("TaskFlags", 16.0f, "Dodge");        // 16 = ANIM_FLAG_PARTIAL
    S.dashSeconds    = CF("DashSeconds", 0.6f, "Dodge");
    S.diveDuration   = CF("DiveDuration", 3.0f, "Dodge");          // batas maksimum dive (detik nyata); biasanya berakhir sendiri saat animasi selesai
    S.holdLastFrame  = CB("HoldLastFrame", false, "Dodge");         // 1 = tahan pose terakhir sampai DiveDuration habis
    S.diveAnimSpeed  = CF("DiveAnimSpeed", 0.55f, "Dodge");         // kecepatan animasi saat dive (lebih kecil = dive lebih lama)
    S.freezeStick    = CB("FreezeStickWhileDiving", true, "Dodge");
    S.hydraMode      = (int)CF("HydraulicsMode", 0.0f, "BulletTime"); // 0 = tap: bullet time, tap 2x: dodge | 1 = tap langsung dodge
    S.hydraDodge     = CB("HydraulicsDoubleTapDodge", true, "Dodge");
    S.tapWindow      = CF("DoubleTapWindow", 0.5f, "Dodge");
    S.diveBtn        = CPadBtn("DiveButton", "None", "Dodge");
    S.diveForward    = CF("DiveSpeed", 0.16f, "Dodge");
    S.diveUp         = CF("DiveUpSpeed", 0.0f, "Dodge");
    S.autoBulletTime = CB("AutoBulletTime", true, "Dodge");
    S.diveSlow       = CF("DiveSlowSeconds", 2.5f, "Dodge");
    S.useIfp         = CB("UseIFP", true, "Dodge");
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
static float gScale = 1.0f;          // time scale milik mod yang sedang dipakai
static float gFactor = 1.0f;         // pengali kecepatan CJ relatif terhadap dunia (>=1)
static bool  gInPlayerProcess = false;
static bool  gManual = false;
static float gDiveSlow = 0.0f;
static bool  gHydraPrev = false, gHydraTap = false, gHydraLogged = false;
static double gHydraLast = -10.0;
static float gLastTap = -10.0f;
static bool  gPrevActivate = false, gPrevDive = false;
static int16_t gPrevPad[20];
static bool  gIfpTried = false;
static float gDashT = 0.0f, gDashDX = 0.0f, gDashDY = 0.0f;
static float gDiveAnimT = 0.0f;
static bool  gDiveTaskLive = false, gDiveSecondary = false;
static void* gDiveTask = NULL;
static float gAimKickT = 0.0f;
static int   gAimLogN = 0, gLastTargetReal = -1;
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
    if(!LoadAnimFile || !GetAnimationFn || !GetAnimationBlock || !BlendAnimation || !RwStreamOpenFn || !RwStreamCloseFn) { FLog("IFP: fungsi CAnimManager/RwStream tidak lengkap"); return; }

    char path[500];
    if(S.ifpPath[0]) snprintf(path, sizeof(path), "%s", S.ifpPath);
    else
    {
        snprintf(path, sizeof(path), "%s/BulletTime/MP3SD.ifp", aml->GetAndroidDataPath());
        if(!aml->FileExists(path)) snprintf(path, sizeof(path), "%s/MP3SD.ifp", aml->GetAndroidDataPath());
    }
    if(!aml->FileExists(path)) { FLog("IFP: file tidak ada: %s", path); return; }

    // nama blok animasi ada di header IFP (ANP3): 'ANP3'(4) + ukuran(4) + nama(24)
    char blockName[25] = {0};
    FILE* f = fopen(path, "rb");
    if(!f) { FLog("IFP: gagal membuka %s", path); return; }
    unsigned char hdr[32];
    size_t got = fread(hdr, 1, 32, f);
    fclose(f);
    if(got != 32 || memcmp(hdr, "ANP3", 4) != 0) { FLog("IFP: header bukan ANP3 (format tidak didukung)"); return; }
    memcpy(blockName, hdr + 8, 24);
    memcpy(gBlockName, blockName, sizeof(blockName));

    // loader bawaan mendaftarkan blok dari header sendiri (RegisterAnimBlock tidak perlu dipanggil)
    void* blk = GetAnimationBlock(blockName);
    if(!blk)
    {
        // baca file ke memori dan buka sebagai stream memori RenderWare
        // (LoadAnimFile(path) langsung crash di port Android karena stream file gagal dibuka)
        FILE* g = fopen(path, "rb");
        if(!g) { FLog("IFP: gagal membuka %s", path); return; }
        fseek(g, 0, SEEK_END); long size = ftell(g); fseek(g, 0, SEEK_SET);
        if(size < 64 || size > 64 * 1024 * 1024) { fclose(g); FLog("IFP: ukuran file aneh (%ld)", size); return; }
        uint8_t* buf = (uint8_t*)malloc((size_t)size);
        if(!buf || fread(buf, 1, (size_t)size, g) != (size_t)size) { fclose(g); free(buf); FLog("IFP: gagal membaca file"); return; }
        fclose(g);

        RwMemory mem = { buf, (uint32_t)size };
        FLog("IFP: membuka stream memori (%ld byte)", size);
        void* st = RwStreamOpenFn(3 /*rwSTREAMMEMORY*/, 1 /*rwSTREAMREAD*/, &mem);
        if(!st) { FLog("IFP: RwStreamOpen gagal"); free(buf); return; }
        FLog("IFP: memanggil CAnimManager::LoadAnimFile");
        LoadAnimFile(st, true, NULL);
        FLog("IFP: LoadAnimFile selesai");
        RwStreamCloseFn(st, &mem);
        free(buf);
    }
    blk = GetAnimationBlock(blockName);
    if(!blk) { FLog("IFP: blok '%s' tidak terdaftar setelah LoadAnimFile (%s)", blockName, path); return; }

    int found = 0;
    for(int i = 0; i < 4; ++i) { gDiveHier[i] = GetAnimationFn(kDiveNames[i], blk); if(gDiveHier[i]) ++found; }
    FLog("IFP: blok '%s' dimuat dari %s, %d/4 animasi dive ditemukan", blockName, path, found);
}

// Task yang sekarang ada di slot dive (CTaskManager di CPedIntelligence+8: primer mulai +0, sekunder mulai +40)
static void* DiveSlotTask(void* ped)
{
    if(!ped) return NULL;
    void* intel = *(void**)((uintptr_t)ped + 1336);
    if(!intel) return NULL;
    uint8_t* mgr = (uint8_t*)intel + 8;
    return gDiveSecondary ? *(void**)(mgr + 40 + 4 * 8) : *(void**)(mgr + 3 * 8);
}

// Akhiri task dive dengan benar: MakeAbortable(immediate) melepas animasi (tidak tertinggal beku), lalu slot dikosongkan
static void EndDiveTask(void* ped)
{
    if(!gDiveTaskLive) return;
    gDiveTaskLive = false;
    gDiveAnimT = 0.0f;
    void* task = gDiveTask;
    gDiveTask = NULL;
    if(!ped || !task) return;
    if(DiveSlotTask(ped) != task) return;        // sudah selesai/dihapus game sendiri
    if(MakeAbortableFn) MakeAbortableFn(task, ped, 2 /*immediate*/, NULL);
    void* intel = *(void**)((uintptr_t)ped + 1336);
    if(!intel) return;
    void* mgr = (uint8_t*)intel + 8;
    if(gDiveSecondary && SetTaskSecFn) SetTaskSecFn(mgr, NULL, 4);
    else if(SetTaskFn) SetTaskFn(mgr, NULL, 3, false);
}

static bool DoDodge(void* ped, int16_t* pad)
{
    if(!ApplyMoveForce || !ped) { FLog("Dodge: ApplyMoveForce=%p ped=%p", (void*)ApplyMoveForce, ped); return false; }
    float fx, fy;
    if(!GetPedForward(ped, fx, fy))
    {
        uintptr_t b = (uintptr_t)ped; void* mat = *(void**)(b + 0x18);
        FLog("Dodge: arah hadap ped tidak valid (matrix=%p heading=%.2f)", mat, *(float*)(b + 0x14));
        return false;
    }

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

    // dorongan vertikal opsional (bawaan 0 supaya CJ tidak masuk status "melayang" yang menimpa animasi)
    if(S.diveUp > 0.0f)
    {
        CVector up = { 0.0f, 0.0f, S.diveUp * mass };
        ApplyMoveForce(ped, up);
    }
    // meluncur mendatar selama DashSeconds (diterapkan tiap frame di hook pemain)
    gDashT = S.dashSeconds; gDashDX = dx; gDashDY = dy;

    // animasi lewat task bawaan game (CTaskSimpleRunNamedAnim), seperti script TASK_PLAY_ANIM.
    // Slot sekunder PARTIAL_ANIM (4): task jalan CJ tetap aktif, jadi aim & tembak tetap bisa saat dive.
    bool tasked = false;
    void* clump = *(void**)((uintptr_t)ped + 0x20);
    if(S.useTask && gDiveHier[dir] && gBlockName[0] && TaskNewFn && RunNamedCtor && (SetTaskFn || SetTaskSecFn))
    {
        void* intel = *(void**)((uintptr_t)ped + 1336);     // CPed::m_pIntelligence; CTaskManager di +8
        EndDiveTask(ped);                                    // buang task dive sebelumnya kalau masih ada
        void* t = intel ? TaskNewFn(120) : NULL;
        if(t)
        {
            // endTime=-1: diakhiri oleh mod sesudah DiveDuration; bHoldLastFrame=true menahan pose dive
            RunNamedCtor(t, kDiveNames[dir], gBlockName, S.taskFlags, S.animBlend, -1, false, false, false, S.holdLastFrame);
            void* mgr = (uint8_t*)intel + 8;
            if(S.useSecondary && SetTaskSecFn) { SetTaskSecFn(mgr, t, 4); gDiveSecondary = true; }
            else if(SetTaskFn) { SetTaskFn(mgr, t, 3, false); gDiveSecondary = false; }
            gDiveTask = t;
            gAimKickT = 0.4f;
            gDiveTaskLive = true;
            gDiveAnimT = S.diveDuration;
            tasked = true;
        }
    }

    bool animPlayed = tasked;
    void* assoc = NULL;
    if(!tasked && gDiveHier[dir] && BlendAnimation && clump)
    {
        assoc = BlendAnimation(clump, gDiveHier[dir], S.animFlags, S.animBlend);
        animPlayed = assoc != NULL;
    }
    if(!animPlayed && S.fallbackJump) pad[kBtnSquare] = 255;   // tanpa IFP: lompat bawaan
    { static int n = 0; if(++n <= 8) FLog("Dodge #%d arah=%d task=%d slot=%s assoc=%p fwd=(%.2f,%.2f) aimSebelum=%d", n, dir, (int)tasked, gDiveSecondary ? "sekunder" : "utama", assoc, fx, fy, gLastTargetReal); }

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
    if(!canAct) { gManual = false; gDiveSlow = 0.0f; EndDiveTask(ped); }

    if(!gIfpTried) LoadIfp();

    if(pad && S.debugPad)
    {
        for(int i = 4; i < kPadCount; ++i)
        {
            if(pad[i] != gPrevPad[i]) logger->Info("Pad %s = %d", kPadNames[i], (int)pad[i]);
            gPrevPad[i] = pad[i];
        }
    }

    // tombol hidrolik di mode jalan: mode 0 = tap bullet time on/off, tap 2x cepat = dodge; mode 1 = tap = dodge
    if(gHydraTap)
    {
        gHydraTap = false;
        double now = RealNow();
        double gap = now - gHydraLast;
        bool dodge = (S.hydraMode == 1) || (S.hydraDodge && gap <= S.tapWindow);
        static int tn = 0;
        if(++tn <= 24) FLog("Tap #%d selang=%.2fs aksi=%s bisa=%d meter=%.2f", tn, gap > 100.0 ? -1.0 : gap, dodge ? "dodge" : "bullet-time", (int)canAct, gMeter);
        if(canAct)
        {
            if(dodge) { bool ok = DoDodge(ped, pad); if(!ok) FLog("Dodge gagal dijalankan (lihat baris di atas)"); gHydraLast = -10.0; }
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

    if(gDashT > 0.0f) gDashT -= dt;
    if(gAimKickT > 0.0f) gAimKickT -= dt;
    if(gDiveTaskLive)
    {
        gDiveAnimT -= dt;
        if(DiveSlotTask(ped) != gDiveTask)
        {
            // animasi selesai dan game sudah membuang task-nya: kontrol langsung normal lagi
            gDiveTaskLive = false; gDiveTask = NULL; gDiveAnimT = 0.0f;
            static int en = 0; if(++en <= 8) FLog("Dive selesai (task habis sendiri)");
        }
        else
        {
            if(pad && S.freezeStick) { pad[0] = 0; pad[1] = 0; }   // CJ tidak berjalan sendiri selama animasi dive
            if(gDiveAnimT <= 0.0f) { static int tn2 = 0; if(++tn2 <= 8) FLog("Dive dihentikan paksa (batas waktu)"); EndDiveTask(ped); }
        }
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
// luncuran mendatar dodge: atur kecepatan gerak ped (CPhysical::m_vecMoveSpeed di +104) selama DashSeconds
static void ApplyDash(void* ped)
{
    if(gDashT <= 0.0f || !ped) return;
    float f = gDashT / (S.dashSeconds > 0.01f ? S.dashSeconds : 0.01f);
    if(f > 1.0f) f = 1.0f;
    float* ms = (float*)((uintptr_t)ped + 104);
    ms[0] = gDashDX * S.diveForward * f;
    ms[1] = gDashDY * S.diveForward * f;
}

static void PlayerProcess_hook(void* self)
{
    if(S.enabled) { Tick(self); ApplyDash(self); }

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
    if(S.enabled) ApplyDash(self);
}

// --- status aim (membidik) ---
static void LogAim(const char* what, int v)
{
    if(!S.logAim || gAimLogN >= 80) return;
    static int last[3] = { -1, -1, -1 };
    int k = what[3] == 'T' ? 0 : (what[3] == 'E' ? 1 : 2);   // GetTarget / GetEnter... / GetExit...
    if(last[k] == v) return;
    last[k] = v; ++gAimLogN;
    FLog("Aim: %s=%d (dive=%d)", what, v, (int)gDiveTaskLive);
}

static bool Target_hook(void* pad, bool b)
{
    bool r = Target_orig(pad, b);
    gLastTargetReal = r ? 1 : 0;
    LogAim("GetTarget", r);
    if(S.enabled && S.forceAim && gDiveTaskLive) r = true;
    return r;
}
static bool EnterT_hook(void* pad)
{
    bool r = EnterT_orig(pad);
    LogAim("GetEnterTargeting", r);
    if(S.enabled && S.forceAim && gAimKickT > 0.0f) r = true;
    return r;
}
static bool ExitT_hook(void* pad)
{
    bool r = ExitT_orig(pad);
    LogAim("GetExitTargeting", r);
    if(S.enabled && S.forceAim && gDiveTaskLive) r = false;
    return r;
}

static void AnimUpdate_hook(void* clump, float dt, bool onScreen)
{
    if(S.enabled && (gFactor > 1.001f || gDiveTaskLive) && clump && FindPlayerPed)
    {
        void* ped = FindPlayerPed(0);
        if(ped && clump == *(void**)((uintptr_t)ped + 0x20))
        {
            if(gFactor > 1.001f && !gInPlayerProcess) dt *= gFactor;
            if(gDiveTaskLive) dt *= S.diveAnimSpeed;          // dive lebih lambat = lebih lama
        }
    }
    AnimUpdate_orig(clump, dt, onScreen);
}

// Tombol hidrolik (CWidgetButtonHydraulics): +200 = aktif, +201 = tampil, +140 = flag widget (bit2 = nonaktif).
// Update() bawaan mengosongkan +200/+201 saat tidak di kendaraan; di sini diaktifkan lagi saat berjalan kaki.
static int   gHUpdFoot = 0, gHDrawFoot = 0;
static bool  gHVehLogged = false, gHSumLogged = false;
static double gHFirstFoot = 0.0;
static bool  gOnFootNow = false;

static void HydraUpdate_hook(void* self)
{
    HydraUpdate_orig(self);
    if(!S.enabled || !S.hydraButton || !FindPlayerPed) return;
    uint8_t* w = (uint8_t*)self;
    uint32_t* fl = (uint32_t*)(w + 140);

    void* ped = FindPlayerPed(0);
    if(!ped)
    {
        gOnFootNow = false;
        if(gScale < 0.999f && pTimeScale) { gScale = 1.0f; *pTimeScale = 1.0f; }
        gManual = false; gDiveSlow = 0.0f; gHydraPrev = false;
        return;
    }
    if(FindPlayerVehicle && FindPlayerVehicle(0, false))
    {
        gOnFootNow = false; gHydraPrev = false;
        if(!gHVehLogged) { gHVehLogged = true; FLog("Hydra di kendaraan: aktif=%d tampil=%d flag140=0x%x", (int)w[200], (int)w[201], *fl); }
        return;
    }

    gOnFootNow = true;
    ++gHUpdFoot;
    if(!gHydraLogged)
    {
        gHydraLogged = true;
        gHFirstFoot = RealNow();
        FLog("Hydra Update dipanggil saat jalan kaki. Sebelum: aktif=%d tampil=%d enabled=%d alpha=%d flag140=0x%x", (int)w[200], (int)w[201], (int)w[89], (int)w[88], *fl);
    }
    *fl = (*fl & ~S.hydraFlagClear) | S.hydraFlagSet;
    w[200] = 1;      // aktif
    w[201] = 1;      // tampil
    w[89]  = 1;      // CWidget "enabled" (SetEnabled)
    w[88]  = 255;    // alpha saat ini; 0 = digambar transparan (penyebab tombol tak terlihat)
    if(gHFirstFoot > 0.0 && !gHSumLogged && RealNow() - gHFirstFoot > 8.0)
    {
        gHSumLogged = true;
        FLog("Ringkasan 8 detik jalan kaki: Update=%d Draw=%d flag140=0x%x (clear=0x%x set=0x%x)", gHUpdFoot, gHDrawFoot, *fl, S.hydraFlagClear, S.hydraFlagSet);
    }

    void** vt = *(void***)self;
    bool touched = ((WidgetIsTouched_t)vt[20])(self, NULL);
    if(touched && !gHydraPrev)
    {
        gHydraTap = true;
        static int taps = 0;
        if(++taps <= 8) FLog("Tombol hidrolik ditekan (#%d)", taps);
    }
    gHydraPrev = touched;
}

// Bar adrenalin bullet time (digambar tiap frame saat jalan kaki, dari hook Draw tombol hidrolik)
static void DrawBar()
{
    if(!S.showBar || !DrawRectFn || !pRsGlobal) return;
    int W = *(int*)(pRsGlobal + 8), H = *(int*)(pRsGlobal + 12);
    static bool logged = false;
    if(!logged) { logged = true; FLog("Bar: layar %dx%d", W, H); }
    if(W < 200 || W > 10000 || H < 200 || H > 10000) return;

    float x = S.barX * W, y = S.barY * H, w = S.barW * W, h = S.barH * H;
    float pad = h * 0.18f; if(pad < 1.5f) pad = 1.5f;

    CRGBA border = { 0, 0, 0, 200 };
    CRect rb = { x - 1.0f, y + h + 1.0f, x + w + 1.0f, y - 1.0f };
    DrawRectFn(rb, border);
    CRGBA back = { 25, 25, 30, 170 };
    CRect rk = { x, y + h, x + w, y };
    DrawRectFn(rk, back);

    float m = gMeter < 0.0f ? 0.0f : (gMeter > 1.0f ? 1.0f : gMeter);
    float fw = (w - 2.0f * pad) * m;
    if(fw > 0.5f)
    {
        CRGBA col;
        if(BulletTimeOn()) col = { 90, 210, 255, 235 };        // aktif
        else if(m < 0.25f) col = { 255, 130, 60, 220 };        // hampir habis / mengisi
        else col = { 235, 235, 235, 210 };
        CRect rf = { x + pad, y + h - pad, x + pad + fw, y + pad };
        DrawRectFn(rf, col);
    }
}

static void HydraDraw_hook(void* self)
{
    if(gOnFootNow) ++gHDrawFoot;
    HydraDraw_orig(self);
    if(gOnFootNow) DrawBar();
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
        LoadAnimFile      = (LoadAnimFile_t)Sym(lib, "_ZN12CAnimManager12LoadAnimFileEP8RwStreambPA32_Kc");
        RwStreamOpenFn    = (RwStreamOpen_t)Sym(lib, "_Z12RwStreamOpen12RwStreamType18RwStreamAccessTypePKv");
        RwStreamCloseFn   = (RwStreamClose_t)Sym(lib, "_Z13RwStreamCloseP8RwStreamPv");
        GetAnimationFn    = (GetAnimation_t)Sym(lib, "_ZN12CAnimManager12GetAnimationEPKcPK10CAnimBlock");
        BlendAnimation    = (BlendAnimation_t)Sym(lib, "_ZN12CAnimManager14BlendAnimationEP7RpClumpP19CAnimBlendHierarchyif");
    }

    TaskNewFn    = (TaskNew_t)Sym(lib, "_ZN5CTasknwEm");
    RunNamedCtor = (RunNamedAnimCtor_t)Sym(lib, "_ZN23CTaskSimpleRunNamedAnimC1EPKcS1_ifibbbb");
    SetTaskFn    = (SetTask_t)Sym(lib, "_ZN12CTaskManager7SetTaskEP5CTaskib");
    SetTaskSecFn = (SetTaskSecondary_t)Sym(lib, "_ZN12CTaskManager16SetTaskSecondaryEP5CTaski");
    MakeAbortableFn = (MakeAbortable_t)Sym(lib, "_ZN15CTaskSimpleAnim13MakeAbortableEP4CPediPK6CEvent");
    DrawRectFn = (DrawRect_t)Sym(lib, "_ZN9CSprite2d8DrawRectERK5CRectRK5CRGBA");
    pRsGlobal  = (uint8_t*)Sym(lib, "RsGlobal");
    uintptr_t aimTarget = Sym(lib, "_ZN4CPad9GetTargetEb");
    uintptr_t aimEnter  = Sym(lib, "_ZN4CPad17GetEnterTargetingEv");
    uintptr_t aimExit   = Sym(lib, "_ZN4CPad16GetExitTargetingEv");
    uintptr_t playerProcess = Sym(lib, "_ZN10CPlayerPed14ProcessControlEv");
    uintptr_t animUpdate    = Sym(lib, "_Z32RpAnimBlendClumpUpdateAnimationsP7RpClumpfb");
    uintptr_t hydraUpdate   = Sym(lib, "_ZN23CWidgetButtonHydraulics6UpdateEv");
    uintptr_t hydraDraw     = Sym(lib, "_ZN23CWidgetButtonHydraulics4DrawEv");

    if(S.hookPlayer && playerProcess && pTimeScale && GetPad)
        aml->Hook((void*)playerProcess, (void*)PlayerProcess_hook, (void**)&PlayerProcess_orig);
    else logger->Error("Hook pemain TIDAK dipasang (bullet time/dodge mati)");

    if(S.hookAnim && animUpdate)
        aml->Hook((void*)animUpdate, (void*)AnimUpdate_hook, (void**)&AnimUpdate_orig);

    if(S.hookHydra && S.hydraButton && hydraUpdate)
    {
        aml->Hook((void*)hydraUpdate, (void*)HydraUpdate_hook, (void**)&HydraUpdate_orig);
        if(hydraDraw) aml->Hook((void*)hydraDraw, (void*)HydraDraw_hook, (void**)&HydraDraw_orig);
    }
    else if(S.hydraButton) logger->Error("Tombol hidrolik di mode jalan TIDAK dipasang");

    FLog("=== BulletTimeSA64 v1.10 dimulai ===");
    if(S.hookTarget && aimTarget) aml->Hook((void*)aimTarget, (void*)Target_hook, (void**)&Target_orig);
    if(S.hookAimEnter && aimEnter) aml->Hook((void*)aimEnter, (void*)EnterT_hook, (void**)&EnterT_orig);
    if(S.hookAimExit && aimExit) aml->Hook((void*)aimExit, (void*)ExitT_hook, (void**)&ExitT_orig);

    logger->Info("Siap v1.10 (hook: pemain=%d anim=%d hidrolik=%d, IFP=%d)",
                 (int)(PlayerProcess_orig != NULL), (int)(AnimUpdate_orig != NULL), (int)(HydraUpdate_orig != NULL), (int)S.useIfp);
}

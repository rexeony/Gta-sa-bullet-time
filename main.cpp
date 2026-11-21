// Bullet Time + Dodge for GTA San Andreas Android (AML, arm64-v8a)
// Ide: Max Payne (bullet time + shootdodge). Dibuat ulang dari nol, bukan port dari mod PC.
// Terinspirasi dari "Frnchise Bullet Time" oleh frnchise.
//
// Semua alamat game dicari lewat NAMA SIMBOL di libGTASA.so (tanpa offset hardcode).
// Kalau ada simbol yang tidak ketemu, fitur terkait dimatikan dan alasannya ditulis ke logcat.

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

MYMODCFGNAME(net.hafizdms.sabullettime, Bullet Time Dodge, 1.0, hafizdMS/zidanaSQ, BulletTime)
NEEDGAME(com.rockstargames.gtasa)

// ---------------------------------------------------------------- game symbols
typedef void*  (*GetPad_t)(int);               // CPad::GetPad(int)
typedef void*  (*FindPlayerPed_t)(int);        // FindPlayerPed(int)
struct CVector { float x, y, z; };
typedef void   (*ApplyMoveForce_t)(void*, CVector); // CPhysical::ApplyMoveForce(CVector) - CVector lewat register s0-s2
typedef void   (*HydraUpdate_t)(void*);             // CWidgetButtonHydraulics::Update()
typedef bool   (*WidgetIsTouched_t)(void*, void*);  // virtual CWidget::IsTouched(CVector2D*) (vtable +160)
typedef void*  (*GetAnimationBlock_t)(const char*);       // CAnimManager::GetAnimationBlock
typedef void*  (*RegisterAnimBlock_t)(const char*);       // CAnimManager::RegisterAnimBlock
typedef void   (*LoadAnimFile_t)(const char*);            // CAnimManager::LoadAnimFile(const char*)
typedef void*  (*GetAnimation_t)(const char*, const void*); // CAnimManager::GetAnimation(name, block)
typedef void*  (*BlendAnimation_t)(void*, void*, int, float); // CAnimManager::BlendAnimation(clump, hierarchy, flags, blend)
typedef void*  (*FindPlayerVehicle_t)(int, bool); // FindPlayerVehicle(int, bool)
typedef void   (*PedProcess_t)(void*);            // CPlayerPed::ProcessControl()
typedef void   (*AnimUpdate_t)(void*, float, bool); // RpAnimBlendClumpUpdateAnimations(RpClump*, float, bool)
typedef void   (*Void_t)();

static GetPad_t         GetPad = NULL;
static FindPlayerPed_t  FindPlayerPed = NULL;
static ApplyMoveForce_t ApplyMoveForce = NULL;
static FindPlayerVehicle_t FindPlayerVehicle = NULL;
static HydraUpdate_t    HydraUpdate_orig = NULL;
static GetAnimationBlock_t GetAnimationBlock = NULL;
static RegisterAnimBlock_t RegisterAnimBlock = NULL;
static LoadAnimFile_t   LoadAnimFile = NULL;
static GetAnimation_t   GetAnimationFn = NULL;
static BlendAnimation_t BlendAnimation = NULL;
static float*           pTimeScale = NULL;     // CTimer::ms_fTimeScale
static float*           pTimeStep = NULL;      // CTimer::ms_fTimeStep
static PedProcess_t     PlayerProcess_orig = NULL;
static AnimUpdate_t     AnimUpdate_orig = NULL;
static Void_t           CTimer_Update_orig = NULL;
static Void_t           UpdatePads_orig = NULL;

// ---------------------------------------------------------------- settings
struct Settings
{
    bool  enabled, debugPad, dumpSymbols, holdToActivate, autoBulletTime, hydraButton, hydraDodge, useIfp;
    int   activateBtn, diveBtn;           // index int16 di CControllerState, -1 = None
    float animBlend, cjSpeed, worldSpeed, blendIn, blendOut, duration, refill, startMeter, activationMin;
    float tapWindow, diveForward, diveUp, diveSlow;
    int   animFlags;
    char  ifpPath[400];
} S;

// Urutan field int16 di CControllerState (NewState ada di awal CPad)
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

static float CF(const char* k, float d, const char* s) { float v = cfg->Bind(k, d, s)->GetFloat(); cfg->ClearLast(); return v; }
static bool  CB(const char* k, bool d, const char* s)  { bool v = cfg->Bind(k, d, s)->GetBool();  cfg->ClearLast(); return v; }
static void  CS(const char* k, const char* d, const char* s, char* out, size_t n)
{
    strncpy(out, cfg->Bind(k, d, s)->GetString(), n - 1); out[n - 1] = 0; cfg->ClearLast();
}
static int   CPadBtn(const char* k, const char* d, const char* s)
{
    char buf[64];
    strncpy(buf, cfg->Bind(k, d, s)->GetString(), sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    cfg->ClearLast();
    return PadIndex(buf);
}

static void LoadSettings()
{
    S.enabled        = CB("Enabled", true, "Main");
    S.debugPad       = CB("DebugPad", false, "Main");
    S.dumpSymbols    = CB("DumpSymbols", false, "Main");
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
    S.diveBtn        = CPadBtn("DiveButton", "None", "Dodge");
    S.hydraDodge     = CB("HydraulicsDoubleTapDodge", true, "Dodge");
    S.useIfp         = CB("UseIFP", true, "Dodge");
    CS("IFPPath", "", "Dodge", S.ifpPath, sizeof(S.ifpPath));
    S.animFlags      = (int)CF("AnimFlags", 0.0f, "Dodge");
    S.animBlend      = CF("AnimBlend", 8.0f, "Dodge");
    S.tapWindow      = CF("DoubleTapWindow", 0.35f, "Dodge");
    S.diveForward    = CF("DiveSpeed", 0.30f, "Dodge");
    S.diveUp         = CF("DiveUpSpeed", 0.20f, "Dodge");
    S.autoBulletTime = CB("AutoBulletTime", true, "Dodge");
    S.diveSlow       = CF("DiveSlowSeconds", 2.5f, "Dodge");
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
static float gScale = 1.0f;          // time scale yang sedang dipakai
static float gFactor = 1.0f;         // pengali kecepatan CJ relatif terhadap dunia (>=1)
static bool  gInPlayerProcess = false;
static bool  gHydraDown = false, gHydraPrev = false, gHydraTap = false, gHydraLogged = false;
static float gHydraLast = -10.0f;
static bool  gIfpTried = false;
static void* gDiveHier[4] = { NULL, NULL, NULL, NULL };
static const char* kDiveNames[4] = { "sd_dive_fwd", "sd_dive_bkw", "sd_dive_left", "sd_dive_right" };
static bool  gManual = false;        // bullet time manual (tombol)
static float gDiveSlow = 0.0f;       // sisa detik bullet time otomatis dari dodge
static float gLastTap = -10.0f;      // waktu nyata tap lompat terakhir
static bool  gPrevActivate = false, gPrevDive = false;
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
    if(dt < 0.0f) dt = 0.0f;
    if(dt > 0.1f) dt = 0.1f;
    return dt;
}

static inline bool BulletTimeOn() { return gManual || gDiveSlow > 0.0f; }

// ---------------------------------------------------------------- dodge
// Arah hadap ped. Layout CPlaceable/CEntity di arm64 (vtable 8 byte):
//   +0x08 m_placement { CVector pos; float heading }  -> heading di +0x14
//   +0x18 m_matrix (pointer ke CMatrix, baris "top" = arah depan di float[4..5])
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
    if(h > -20.0f && h < 20.0f)
    {
        fx = -sinf(h); fy = cosf(h);
        return true;
    }
    return false;
}

// Muat MP3SD.ifp lewat fungsi bawaan game (sekali, setelah pemain ada)
static void LoadIfp()
{
    gIfpTried = true;
    if(!S.useIfp) return;
    if(!LoadAnimFile || !GetAnimationFn || !BlendAnimation) { logger->Error("IFP: fungsi CAnimManager tidak lengkap, pakai lompat biasa"); return; }

    char path[500];
    if(S.ifpPath[0]) snprintf(path, sizeof(path), "%s", S.ifpPath);
    else snprintf(path, sizeof(path), "%s/BulletTime/MP3SD.ifp", aml->GetAndroidDataPath());
    if(!aml->FileExists(path)) { logger->Error("IFP: file tidak ada: %s", path); return; }

    void* blk = GetAnimationBlock ? GetAnimationBlock("MP3SD") : NULL;
    if(!blk && RegisterAnimBlock) blk = RegisterAnimBlock("MP3SD");
    LoadAnimFile(path);
    if(!blk && GetAnimationBlock) blk = GetAnimationBlock("MP3SD");
    if(!blk) { logger->Error("IFP: blok MP3SD tidak terdaftar"); return; }

    int found = 0;
    for(int i = 0; i < 4; ++i)
    {
        gDiveHier[i] = GetAnimationFn(kDiveNames[i], blk);
        if(gDiveHier[i]) ++found;
    }
    logger->Info("IFP: %d/4 animasi dive ditemukan (%s)", found, path);
}

static bool DoDodge(int16_t* pad)
{
    if(!ApplyMoveForce || !FindPlayerPed) return false;
    void* ped = FindPlayerPed(0);
    if(!ped) return false;

    float fx, fy;
    if(!GetPedForward(ped, fx, fy))
    {
        logger->Error("Dodge: arah hadap ped tidak valid, dilewati");
        return false;
    }

    // arah dari stik kiri relatif terhadap hadap CJ (stik atas = nilai negatif); diam = maju
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

    // CPhysical::m_fMass di +176 (arm64): gaya = massa x kecepatan yang diinginkan
    float mass = *(float*)((uintptr_t)ped + 176);
    if(!(mass > 1.0f && mass < 2000.0f)) mass = 70.0f;
    CVector force = { dx * S.diveForward * mass, dy * S.diveForward * mass, S.diveUp * mass };
    ApplyMoveForce(ped, force);

    bool animPlayed = false;
    if(gDiveHier[dir] && BlendAnimation)
    {
        void* clump = *(void**)((uintptr_t)ped + 0x20);   // CEntity::m_pRwObject (+32 di arm64)
        if(clump) { BlendAnimation(clump, gDiveHier[dir], S.animFlags, S.animBlend); animPlayed = true; }
    }
    if(!animPlayed) pad[kBtnSquare] = 255;       // tanpa IFP: pakai lompat bawaan supaya CJ terangkat

    if(S.autoBulletTime && gMeter > S.activationMin) gDiveSlow = S.diveSlow;
    return true;
}

// ---------------------------------------------------------------- hooks
static void UpdatePads_hook()
{
    UpdatePads_orig();
    if(!S.enabled || !GetPad) return;

    int16_t* pad = (int16_t*)GetPad(0);   // NewState = awal CPad
    if(!pad) return;

    float dt = RealDelta();
    bool hasPlayer = FindPlayerPed && FindPlayerPed(0);
    bool inVehicle = FindPlayerVehicle && FindPlayerVehicle(0, false);
    if(!hasPlayer || inVehicle) { gManual = false; gDiveSlow = 0.0f; }
    bool canAct = hasPlayer && !inVehicle;     // tombol tidak bisa dipicu saat berkendara

    if(S.debugPad)
    {
        for(int i = 4; i < kPadCount; ++i)
        {
            if(pad[i] != gPrevPad[i]) logger->Info("Pad %s = %d", kPadNames[i], (int)pad[i]);
            gPrevPad[i] = pad[i];
        }
    }

    if(hasPlayer && !gIfpTried) LoadIfp();

    // --- tombol hidrolik di mode berjalan: 1x tap = bullet time on/off, 2x tap cepat = dodge
    if(gHydraTap)
    {
        gHydraTap = false;
        if(canAct)
        {
            float now = (float)RealNow();
            if(S.hydraDodge && now - gHydraLast <= S.tapWindow) { DoDodge(pad); gHydraLast = -10.0f; }
            else
            {
                if(gManual) gManual = false;
                else if(gMeter > S.activationMin) gManual = true;
                gHydraLast = now;
            }
        }
    }

    // --- tombol bullet time
    if(canAct && S.activateBtn >= 0)
    {
        bool down = pad[S.activateBtn] > 0;
        if(S.holdToActivate)
        {
            gManual = down && gMeter > S.activationMin;
        }
        else if(down && !gPrevActivate)
        {
            if(gManual) gManual = false;
            else if(gMeter > S.activationMin) gManual = true;
        }
        gPrevActivate = down;
    }

    // --- dodge: tap ganda tombol lompat
    if(canAct && S.diveBtn >= 0)
    {
        bool down = pad[S.diveBtn] > 0;
        if(down && !gPrevDive)
        {
            float now = (float)RealNow();
            if(now - gLastTap <= S.tapWindow) { DoDodge(pad); gLastTap = -10.0f; }
            else gLastTap = now;
        }
        gPrevDive = down;
    }

    // --- meter adrenalin
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
}

static void CTimer_Update_hook()
{
    if(S.enabled && pTimeScale)
    {
        float dt = RealDelta();
        float target = BulletTimeOn() ? S.worldSpeed : 1.0f;
        float rate = (target < gScale) ? (1.0f - S.worldSpeed) / S.blendIn : (1.0f - S.worldSpeed) / S.blendOut;
        if(gScale > target) { gScale -= rate * dt; if(gScale < target) gScale = target; }
        else if(gScale < target) { gScale += rate * dt; if(gScale > target) gScale = target; }
        *pTimeScale = gScale;
        gFactor = (gScale < 0.999f && S.cjSpeed > gScale) ? (S.cjSpeed / gScale) : 1.0f;
    }
    CTimer_Update_orig();
}

// CJ lebih cepat dari dunia: langkah waktu (dan update animasi) khusus pemain diperbesar
static void PlayerProcess_hook(void* self)
{
    if(S.enabled && pTimeStep && gFactor > 1.001f)
    {
        float saved = *pTimeStep;
        *pTimeStep = saved * gFactor;
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
        // CEntity::m_pRwObject di arm64 = +0x20 (vtable 8, m_placement 16, m_matrix 8)
        if(ped && clump == *(void**)((uintptr_t)ped + 0x20)) dt *= gFactor;
    }
    AnimUpdate_orig(clump, dt, onScreen);
}

// Menulis daftar simbol libGTASA.so yang relevan ke file, supaya nama fungsi bisa diperiksa
static void DumpSymbols()
{
    char path[512] = {0};
    FILE* maps = fopen("/proc/self/maps", "r");
    if(maps)
    {
        char line[768];
        while(fgets(line, sizeof(line), maps))
        {
            size_t n = strlen(line);
            while(n && (line[n-1] == '\n' || line[n-1] == ' ')) line[--n] = 0;
            const char* e = strstr(line, "/libGTASA.so");
            if(e && strlen(e) == 12) { const char* sl = strchr(line, '/'); if(sl) { strncpy(path, sl, sizeof(path)-1); break; } }
        }
        fclose(maps);
    }
    if(!path[0]) { logger->Error("Dump: path libGTASA.so tidak ketemu"); return; }

    FILE* f = fopen(path, "rb");
    if(!f) { logger->Error("Dump: tidak bisa membuka %s", path); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* d = (uint8_t*)malloc(sz);
    if(!d || fread(d, 1, sz, f) != (size_t)sz) { fclose(f); free(d); logger->Error("Dump: gagal baca"); return; }
    fclose(f);

    uint64_t shoff = *(uint64_t*)(d + 0x28);
    uint16_t shentsize = *(uint16_t*)(d + 0x3A), shnum = *(uint16_t*)(d + 0x3C);
    char out[600]; snprintf(out, sizeof(out), "%s/BulletTime_symbols.txt", aml->GetAndroidDataPath());
    FILE* o = fopen(out, "w");
    if(!o) { logger->Error("Dump: tidak bisa menulis %s", out); free(d); return; }

    static const char* keys[] = { "Touch", "Widget", "Hydra", "Anim", "FindPlayer", "CTimer", "CPad", "CPlayerPed", "CPhysical", "CTaskSimpleJump", "CTaskComplexJump" };
    int total = 0, written = 0;
    for(int i = 0; i < shnum; ++i)
    {
        uint8_t* sh = d + shoff + (uint64_t)i * shentsize;
        if(*(uint32_t*)(sh + 4) != 11) continue;               // SHT_DYNSYM
        uint64_t off = *(uint64_t*)(sh + 24), size = *(uint64_t*)(sh + 32);
        uint32_t link = *(uint32_t*)(sh + 40);
        uint8_t* strsh = d + shoff + (uint64_t)link * shentsize;
        const char* str = (const char*)(d + *(uint64_t*)(strsh + 24));
        for(uint64_t k = 0; k + 24 <= size; k += 24)
        {
            uint8_t* sym = d + off + k;
            const char* nm = str + *(uint32_t*)sym;
            uint64_t val = *(uint64_t*)(sym + 8);
            if(!nm[0]) continue;
            ++total;
            for(size_t q = 0; q < sizeof(keys)/sizeof(keys[0]); ++q)
                if(strstr(nm, keys[q])) { fprintf(o, "%s 0x%llx\n", nm, (unsigned long long)val); ++written; break; }
        }
    }
    fprintf(o, "# total simbol dynsym: %d, ditulis: %d\n", total, written);
    fclose(o); free(d);
    logger->Info("Dump simbol selesai: %d dari %d -> %s", written, total, out);
}

// Tombol hidrolik (CWidgetButtonHydraulics) dimunculkan saat berjalan kaki.
// Layout widget (arm64): +200 = aktif, +201 = tampil. Update() bawaan mengosongkan keduanya saat tidak di kendaraan.
static void HydraUpdate_hook(void* self)
{
    HydraUpdate_orig(self);
    gHydraDown = false;
    if(!S.enabled || !S.hydraButton || !FindPlayerPed) return;

    bool onFoot = FindPlayerPed(0) && !(FindPlayerVehicle && FindPlayerVehicle(0, false));
    if(!onFoot) { gHydraPrev = false; return; }

    uint8_t* w = (uint8_t*)self;
    if(!gHydraLogged)
    {
        gHydraLogged = true;
        logger->Info("Hydra widget (jalan kaki): aktif=%d tampil=%d flag140=0x%x", (int)w[200], (int)w[201], *(uint32_t*)(w + 140));
    }
    w[200] = 1;
    w[201] = 1;

    void** vt = *(void***)self;
    bool touched = ((WidgetIsTouched_t)vt[20])(self, NULL);   // vtable +160
    gHydraDown = touched;
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

    pTimeScale     = (float*)Sym(lib, "_ZN6CTimer13ms_fTimeScaleE");
    GetPad         = (GetPad_t)Sym(lib, "_ZN4CPad6GetPadEi");
    FindPlayerPed  = (FindPlayerPed_t)Sym(lib, "_Z13FindPlayerPedi");
    ApplyMoveForce = (ApplyMoveForce_t)Sym(lib, "_ZN9CPhysical14ApplyMoveForceE7CVector");

    pTimeStep      = (float*)Sym(lib, "_ZN6CTimer12ms_fTimeStepE");
    uintptr_t hydraUpdate = Sym(lib, "_ZN23CWidgetButtonHydraulics6UpdateEv");
    GetAnimationBlock = (GetAnimationBlock_t)Sym(lib, "_ZN12CAnimManager17GetAnimationBlockEPKc");
    RegisterAnimBlock = (RegisterAnimBlock_t)Sym(lib, "_ZN12CAnimManager17RegisterAnimBlockEPKc");
    LoadAnimFile      = (LoadAnimFile_t)Sym(lib, "_ZN12CAnimManager12LoadAnimFileEPKc");
    GetAnimationFn    = (GetAnimation_t)Sym(lib, "_ZN12CAnimManager12GetAnimationEPKcPK10CAnimBlock");
    BlendAnimation    = (BlendAnimation_t)Sym(lib, "_ZN12CAnimManager14BlendAnimationEP7RpClumpP19CAnimBlendHierarchyif");
    FindPlayerVehicle = (FindPlayerVehicle_t)Sym(lib, "_Z17FindPlayerVehicleib");
    uintptr_t playerProcess = Sym(lib, "_ZN10CPlayerPed14ProcessControlEv");
    uintptr_t animUpdate    = Sym(lib, "_Z32RpAnimBlendClumpUpdateAnimationsP7RpClumpfb");
    uintptr_t timerUpdate = Sym(lib, "_ZN6CTimer6UpdateEv");
    uintptr_t updatePads  = Sym(lib, "_ZN4CPad10UpdatePadsEv");

    if(timerUpdate && pTimeScale)
        aml->Hook((void*)timerUpdate, (void*)CTimer_Update_hook, (void**)&CTimer_Update_orig);
    else
        logger->Error("Bullet time TIDAK aktif (CTimer tidak ketemu)");

    if(updatePads && GetPad)
        aml->Hook((void*)updatePads, (void*)UpdatePads_hook, (void**)&UpdatePads_orig);
    else
        logger->Error("Tombol bullet time/dodge TIDAK aktif (CPad tidak ketemu)");

    if(playerProcess && pTimeStep)
        aml->Hook((void*)playerProcess, (void*)PlayerProcess_hook, (void**)&PlayerProcess_orig);
    else
        logger->Error("CJ lebih cepat TIDAK aktif (CPlayerPed::ProcessControl/ms_fTimeStep tidak ketemu)");
    if(animUpdate)
        aml->Hook((void*)animUpdate, (void*)AnimUpdate_hook, (void**)&AnimUpdate_orig);
    if(!FindPlayerVehicle) logger->Error("Pemblokiran saat berkendara TIDAK aktif (FindPlayerVehicle tidak ketemu)");
    if(hydraUpdate && S.hydraButton)
        aml->Hook((void*)hydraUpdate, (void*)HydraUpdate_hook, (void**)&HydraUpdate_orig);
    else if(S.hydraButton)
        logger->Error("Tombol hidrolik di mode jalan TIDAK aktif (CWidgetButtonHydraulics::Update tidak ketemu)");
    if(S.dumpSymbols) DumpSymbols();

    if(!ApplyMoveForce || !FindPlayerPed) logger->Error("Dodge TIDAK aktif (CPhysical/FindPlayerPed tidak ketemu)");

    logger->Info("Siap. Tombol hidrolik jalan=%d, pad bullet time=%s, pad dodge=%s",
                 (int)S.hydraButton,
                 S.activateBtn >= 0 ? kPadNames[S.activateBtn] : "None",
                 S.diveBtn >= 0 ? kPadNames[S.diveBtn] : "None");
}

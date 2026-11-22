// BulletTimeSA64 v2.0 - Bullet Time + Dodge (shootdodge) untuk GTA SA Android (arm64, AML)
// Pemicu: tombol hidrolik (widget sentuh) saat jalan kaki.
//   1x tap  = bullet time on/off
//   2x tap  = dodge (animasi dari MP3SD.ifp, varian 1 tangan / 2 tangan / dual)
// Ada bar adrenalin: terkuras saat bullet time, terisi saat menunggu atau saat membunuh.
//
// Dodge bisa dilakukan sambil bidik & tembak; kalau belum bidik, otomatis masuk mode bidik saat melayang.
// Semua offset struct diambil dari disassembly libGTASA.so arm64 (lihat README.md).
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <mod/flogger.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

MYMODCFGNAME(net.hafizdms.sabullettime, Bullet Time Dodge, 2.0, hafizdMS/zidanaSQ, BulletTime)
NEEDGAME(com.rockstargames.gtasa)

static FLogger flog("BulletTime_log.txt");

// ------------------------------------------------------------------ tipe & offset
struct V2 { float x, y; };
struct RectF { float x1, y1, x2, y2; };
struct Rgba { uint8_t r, g, b, a; };
struct RwMemoryT { uint8_t* start; uint32_t length; };

enum { OFF_ENT_MATRIX = 0x18, OFF_ENT_CLUMP = 0x20, OFF_PHYS_MOVESPEED = 0x68,
       OFF_PED_HEALTH = 0x6AC, OFF_PED_WEAPONS = 0x730, OFF_PED_ACTIVESLOT = 0x8DC,
       WEAPON_STRIDE = 0x20,
       OFF_PAD_DISABLED = 0x110,
       OFF_W_HID = 0x08, OFF_W_ENABLED = 0x59, OFF_W_SHOWN = 0xC9, OFF_W_HILITE = 0xC8,
       OFF_ASSOC_BLEND = 0x30, OFF_ASSOC_SPEED = 0x3C, OFF_ASSOC_FLAGS = 0x46 };

// ------------------------------------------------------------------ fungsi game
static void* (*FindPlayerPed)(int);
static void* (*FindPlayerVehicle)(int, bool);
static void* (*GetPad)(int);
static int16_t (*PadWalkLR)(void*);
static int16_t (*PadWalkUD)(void*);
static bool (*PedInControl)(void*);
static int8_t (*PedWeaponSkill)(void*, int);
static void (*LoadAnimFileStream)(void*, bool, void*);
static void* (*RwStreamOpenF)(int, int, const void*);
static int (*RwStreamCloseF)(void*, void*);
static void* (*GetAnimBlock)(const char*);
static int (*GetAnimBlockIndex)(const char*);
static void (*AddAnimBlockRef)(int);
static void* (*GetAnim)(const char*, const void*);
static void* (*FindActiveTask)(void*, int);
static void* (*BlendAnim)(void*, void*, int, float);
static void (*AssocSetFinishCB)(void*, void (*)(void*, void*), void*);
static void (*AssocSetDeleteCB)(void*, void (*)(void*, void*), void*);
static bool (*IsTouchDown)(int);
static V2 (*GetTouchPos)(int);
static bool (*HidImplements)(int);
static bool (*HidIsPressed)(int, float*);
static void (*DrawRectXLU)(const RectF&, const Rgba&, const Rgba&, const Rgba&, const Rgba&);
static int (*ScreenW)();
static int (*ScreenH)();
static float* pTimeScale; static float* pTimeStep;
static uint8_t* pUserPause; static uint8_t* pCodePause;
static int* pMaxTouches;

// ------------------------------------------------------------------ konfigurasi
struct Cfg {
    bool enabled, hydraOnFoot, doubleTapDodge, autoBT, useIFP, useGamepad, lockDive, aimDive, autoFire;
    float worldSpeed, cjSpeed, blendIn, blendOut, duration, refill, startMeter, actMin;
    float killRefill, dodgeCost, diveSpeed, diveUp, diveMove, diveSlow, tapWindow, animBlend, animSpeed;
    float barX, barY, barW, barH;
    bool hookPlayer, hookAnim, hookHydra, hookKill, hookAim;
    char ifpPath[400];
} C;

static float CF(const char* s, const char* k, float d) { ConfigEntry* e = cfg->Bind(k, d, s); float v = e->GetFloat(); delete e; return v; }
static bool  CB(const char* s, const char* k, bool d)  { ConfigEntry* e = cfg->Bind(k, d, s); bool v = e->GetBool(); delete e; return v; }

static void LoadCfg() {
    C.enabled = CB("Main", "Enabled", true);
    C.hookPlayer = CB("Debug", "HookPlayer", true);
    C.hookAnim = CB("Debug", "HookAnim", true);
    C.hookHydra = CB("Debug", "HookHydra", true);
    C.hookKill = CB("Debug", "HookKill", false);
    C.hookAim = CB("Debug", "HookAim", true);
    C.hydraOnFoot = CB("BulletTime", "HydraulicsButtonOnFoot", true);
    C.useGamepad = CB("BulletTime", "UseGamepadMapping", true);
    C.worldSpeed = CF("BulletTime", "WorldSpeed", 0.35f);
    C.cjSpeed = CF("BulletTime", "CJSpeed", 0.6f);
    C.blendIn = CF("BulletTime", "BlendIn", 0.12f);
    C.blendOut = CF("BulletTime", "BlendOut", 0.25f);
    C.duration = CF("BulletTime", "Duration", 14.0f);
    C.refill = CF("BulletTime", "RefillPerSec", 0.04f);
    C.startMeter = CF("BulletTime", "StartMeter", 1.0f);
    C.actMin = CF("BulletTime", "ActivationMinimum", 0.05f);
    C.killRefill = CF("BulletTime", "KillRefill", 0.0f);
    C.doubleTapDodge = CB("Dodge", "HydraulicsDoubleTapDodge", true);
    C.tapWindow = CF("Dodge", "DoubleTapWindow", 0.35f);
    C.dodgeCost = CF("Dodge", "DodgeCost", 0.15f);
    C.diveSpeed = CF("Dodge", "DiveSpeed", 0.30f);
    C.diveUp = CF("Dodge", "DiveUpSpeed", 0.0f);
    C.diveMove = CF("Dodge", "DiveMoveSeconds", 0.9f);
    C.autoBT = CB("Dodge", "AutoBulletTime", true);
    C.diveSlow = CF("Dodge", "DiveSlowSeconds", 2.5f);
    C.useIFP = CB("Dodge", "UseIFP", true);
    C.lockDive = CB("Dodge", "LockControlsDuringDive", false);
    C.aimDive = CB("Dodge", "AimDuringDive", true);
    C.autoFire = CB("Dodge", "AutoFireDuringDive", false);
    C.animBlend = CF("Dodge", "AnimBlend", 8.0f);
    C.animSpeed = CF("Dodge", "AnimSpeed", 1.0f);
    C.barX = CF("Bar", "X", 0.30f); C.barY = CF("Bar", "Y", 0.93f);
    C.barW = CF("Bar", "Width", 0.40f); C.barH = CF("Bar", "Height", 0.018f);
    ConfigEntry* e = cfg->Bind("IFPPath", "", "Dodge");
    e->GetString(C.ifpPath, sizeof(C.ifpPath));
    delete e;
    if (C.worldSpeed < 0.05f) C.worldSpeed = 0.05f;
    if (C.worldSpeed > 0.99f) C.worldSpeed = 0.99f;
    if (C.duration < 1.0f) C.duration = 1.0f;
    if (C.blendIn < 0.01f) C.blendIn = 0.01f;
    if (C.blendOut < 0.01f) C.blendOut = 0.01f;
    cfg->Save();
}

// ------------------------------------------------------------------ state
enum { DIR_FWD, DIR_BKW, DIR_LEFT, DIR_RIGHT };
enum { VAR_1H, VAR_2H, VAR_DUAL };

static struct State {
    double lastT = 0; long frames = 0;
    bool bt = false; float meter = 1.0f; float scale = 1.0f; bool scaleDirty = false;
    // input dari widget
    bool pressPrev = false, evTap = false; double lastTapT = -100; bool pendingOff = false; double pendingOffT = 0;
    // dodge
    bool diving = false; double diveStart = 0; float dirX = 0, dirY = 0; bool upDone = false;
    void* assoc = nullptr; bool assocAlive = false; double diveSlowUntil = 0;
    // ifp
    bool ifpTried = false, ifpOk = false; void* hier[4][3] = {{0}};
    // kill
    void* lastVictim = nullptr; double lastKillT = -10;
    bool inScaled = false; long logCount = 0; bool loggedFoot = false;
} g;

static double Now() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static inline uint8_t* U8(void* p, int off) { return (uint8_t*)p + off; }
static inline float* F32(void* p, int off) { return (float*)((uint8_t*)p + off); }

static void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char* fmt, ...) {
    if (g.logCount++ > 400) return;
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    flog.Write("%s", b);
}

static bool OnFoot(void** pedOut = nullptr) {
    void* ped = FindPlayerPed ? FindPlayerPed(-1) : nullptr;
    if (pedOut) *pedOut = ped;
    if (!ped) return false;
    if (*F32(ped, OFF_PED_HEALTH) <= 0.0f) return false;
    return FindPlayerVehicle(-1, false) == nullptr;
}
static bool PadLocked() { void* p = GetPad(0); return !p || *(uint16_t*)U8(p, OFF_PAD_DISABLED) != 0; }

// ------------------------------------------------------------------ IFP
static const char* kDirName[4] = { "fwd", "bkw", "left", "right" };
static const char* kVarSuffix[3] = { "", "_2h", "_dual" };

static void DiveFinishCB(void*, void*) { g.assocAlive = false; g.assoc = nullptr; }

static bool TryLoadIFPFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 64 || len > 16 * 1024 * 1024) { fclose(f); return false; }
    uint8_t* buf = (uint8_t*)malloc(len);
    if (!buf || fread(buf, 1, len, f) != (size_t)len) { fclose(f); free(buf); return false; }
    fclose(f);
    if (memcmp(buf, "ANP3", 4) != 0 && memcmp(buf, "ANP2", 4) != 0) { Log("IFP bukan ANP2/ANP3: %s", path); free(buf); return false; }
    char name[25]; memcpy(name, buf + 8, 24); name[24] = 0;
    RwMemoryT mem = { buf, (uint32_t)len };
    void* st = RwStreamOpenF(3 /*memory*/, 1 /*read*/, &mem);
    if (!st) { Log("RwStreamOpen gagal"); free(buf); return false; }
    LoadAnimFileStream(st, true, nullptr);
    RwMemoryT dummy = { nullptr, 0 };
    RwStreamCloseF(st, &dummy);
    free(buf);
    void* blk = GetAnimBlock(name);
    if (!blk) { Log("Blok animasi '%s' tidak terdaftar setelah load", name); return false; }
    int idx = GetAnimBlockIndex(name);
    if (idx >= 0) AddAnimBlockRef(idx); // jaga supaya blok tidak dibuang game
    int found = 0;
    for (int d = 0; d < 4; d++) for (int v = 0; v < 3; v++) {
        char an[48]; snprintf(an, sizeof(an), "sd_dive_%s%s", kDirName[d], kVarSuffix[v]);
        g.hier[d][v] = GetAnim(an, blk);
        if (g.hier[d][v]) found++; else Log("Animasi tidak ada: %s", an);
    }
    Log("IFP dimuat dari %s (blok '%s'), %d/12 animasi dodge ketemu", path, name, found);
    return found > 0;
}

static void LoadIFP() {
    g.ifpTried = true;
    if (!C.useIFP) { Log("UseIFP=0, dodge tanpa animasi IFP"); return; }
    char p[600];
    const char* data = aml->GetAndroidDataPath();
    const char* cands[4] = { C.ifpPath, nullptr, nullptr, nullptr };
    char c1[600], c2[600], c3[600];
    snprintf(c1, sizeof(c1), "%s/BulletTime/MP3SD.ifp", data); cands[1] = c1;
    snprintf(c2, sizeof(c2), "%s/MP3SD.ifp", data); cands[2] = c2;
    snprintf(c3, sizeof(c3), "%s/BulletTime/MP3SD.ifp", aml->GetConfigPath()); cands[3] = c3;
    for (int i = 0; i < 4; i++) {
        if (!cands[i] || !cands[i][0]) continue;
        snprintf(p, sizeof(p), "%s", cands[i]);
        if (TryLoadIFPFile(p)) { g.ifpOk = true; return; }
    }
    Log("MP3SD.ifp tidak ditemukan/gagal dimuat. Taruh di: %s/BulletTime/MP3SD.ifp", data);
}

// ------------------------------------------------------------------ bullet time & dodge
static void StartBT() { g.bt = true; g.pendingOff = false; Log("Bullet time ON (meter=%.2f)", g.meter); }
static void StopBT()  { g.bt = false; g.pendingOff = false; Log("Bullet time OFF (meter=%.2f)", g.meter); }

static int WeaponVariant(void* ped) {
    int slot = *(int8_t*)U8(ped, OFF_PED_ACTIVESLOT);
    if (slot < 0 || slot > 12) return VAR_1H;
    int type = *(int*)U8(ped, OFF_PED_WEAPONS + slot * WEAPON_STRIDE);
    switch (type) {
        case 25: case 26: case 27: case 29: case 30: case 31: case 33: case 34:
        case 35: case 36: case 37: case 38: return VAR_2H;
        case 22: case 28: case 32:
            return (PedWeaponSkill(ped, type) >= 2) ? VAR_DUAL : VAR_1H;
        default: return VAR_1H;
    }
}

static bool HasGun(void* ped) {
    int slot = *(int8_t*)U8(ped, OFF_PED_ACTIVESLOT);
    if (slot < 0 || slot > 12) return false;
    int type = *(int*)U8(ped, OFF_PED_WEAPONS + slot * WEAPON_STRIDE);
    return type >= 22 && type <= 34;
}
static bool IsAiming(void* ped) {
    void* mgr = *(void**)U8(ped, 0x538); // CTaskManager, task 0x133 = sedang mengarahkan senjata
    return mgr && FindActiveTask(U8(mgr, 8), 0x133) != nullptr;
}

static void TryDodge(double now) {
    void* ped; if (!OnFoot(&ped) || g.diving || PadLocked()) return;
    if (!PedInControl(ped)) return;
    if (g.meter < C.dodgeCost) { Log("Dodge ditolak: meter %.2f < %.2f", g.meter, C.dodgeCost); return; }
    void* pad = GetPad(0);
    int lr = PadWalkLR(pad), ud = PadWalkUD(pad);
    int dir = DIR_FWD;
    if (abs(lr) > abs(ud) && abs(lr) > 40) dir = lr < 0 ? DIR_LEFT : DIR_RIGHT;
    else if (ud > 40) dir = DIR_BKW;
    int var = WeaponVariant(ped);
    float* m = *(float**)U8(ped, OFF_ENT_MATRIX);
    if (!m) return;
    float rx = m[0], ry = m[1], fx = m[4], fy = m[5];
    float dx = fx, dy = fy;
    if (dir == DIR_BKW) { dx = -fx; dy = -fy; }
    else if (dir == DIR_LEFT) { dx = -rx; dy = -ry; }
    else if (dir == DIR_RIGHT) { dx = rx; dy = ry; }
    float l = sqrtf(dx * dx + dy * dy); if (l < 0.001f) return; g.dirX = dx / l; g.dirY = dy / l;
    g.meter -= C.dodgeCost; if (g.meter < 0) g.meter = 0;
    if (C.autoBT) g.diveSlowUntil = now + C.diveSlow;
    g.diving = true; g.diveStart = now; g.upDone = false; g.assoc = nullptr; g.assocAlive = false;
    void* h = g.hier[dir][var];
    if (!h) h = g.hier[dir][VAR_1H];
    if (h) {
        void* clump = *(void**)U8(ped, OFF_ENT_CLUMP);
        void* a = clump ? BlendAnim(clump, h, 0x0C /* hapus saat selesai + fade out */, C.animBlend) : nullptr;
        if (a) {
            *(uint16_t*)U8(a, OFF_ASSOC_FLAGS) |= 0x0C;
            if (C.animSpeed > 0.01f && fabsf(C.animSpeed - 1.0f) > 0.001f) *F32(a, OFF_ASSOC_SPEED) *= C.animSpeed;
            AssocSetFinishCB(a, DiveFinishCB, nullptr);
            AssocSetDeleteCB(a, DiveFinishCB, nullptr);
            g.assoc = a; g.assocAlive = true;
        }
    }
    Log("DODGE dir=%d varian=%d anim=%s", dir, var, g.assocAlive ? "IFP" : "tanpa-animasi");
    if (C.lockDive) *(uint16_t*)U8(GetPad(0), OFF_PAD_DISABLED) |= 1;
}

static void EndDive() {
    g.diving = false; g.assoc = nullptr; g.assocAlive = false;
    if (C.lockDive) { void* p = GetPad(0); if (p) *(uint16_t*)U8(p, OFF_PAD_DISABLED) &= ~1; }
}

static void HandleTap(double now) {
    bool dbl = (now - g.lastTapT) <= C.tapWindow;
    if (dbl && C.doubleTapDodge) { g.pendingOff = false; g.lastTapT = -100; TryDodge(now); return; }
    g.lastTapT = now;
    if (!g.bt) { if (g.meter >= C.actMin) StartBT(); else Log("Bullet time ditolak: meter kosong"); }
    else if (C.doubleTapDodge) { g.pendingOff = true; g.pendingOffT = now + C.tapWindow; }
    else StopBT();
}

static void Tick() {
    double now = Now(); double dt = now - g.lastT; g.lastT = now; g.frames++;
    if (dt < 0 || dt > 0.25) dt = 0;
    if ((pUserPause && *pUserPause) || (pCodePause && *pCodePause)) dt = 0;
    void* ped = nullptr;
    bool ok = C.enabled && OnFoot(&ped) && !PadLocked();
    if (!ok) {
        if (g.bt || g.diving) { Log("Reset: pemain tidak di kondisi valid"); g.bt = false; if (g.diving) EndDive(); }
        g.evTap = false; g.pendingOff = false; g.diveSlowUntil = 0;
    }
    if (ok && !g.ifpTried && g.frames > 600) LoadIFP();

    if (g.evTap) { g.evTap = false; if (ok) HandleTap(now); }
    if (g.pendingOff && now >= g.pendingOffT) { g.pendingOff = false; if (g.bt) StopBT(); }

    if (g.diving) {
        double el = now - g.diveStart;
        bool noAnimDone = !g.assocAlive && el > 0.9;
        if (noAnimDone || el > 7.0) EndDive();
    }
    if (g.bt) { g.meter -= (float)(dt / C.duration); if (g.meter <= 0) { g.meter = 0; StopBT(); } }
    else if (!g.diving) { g.meter += (float)(C.refill * dt); if (g.meter > 1) g.meter = 1; }

    bool slow = ok && (g.bt || now < g.diveSlowUntil);
    float target = slow ? C.worldSpeed : 1.0f;
    float span = 1.0f - C.worldSpeed;
    float rate = span / (target < g.scale ? C.blendIn : C.blendOut);
    if (g.scale < target) { g.scale += rate * (float)dt; if (g.scale > target) g.scale = target; }
    else if (g.scale > target) { g.scale -= rate * (float)dt; if (g.scale < target) g.scale = target; }
    if (g.scale < 0.9999f) { *pTimeScale = g.scale; g.scaleDirty = true; }
    else if (g.scaleDirty) { g.scale = 1.0f; *pTimeScale = 1.0f; g.scaleDirty = false; }
}

static float CompFactor() {
    if (g.scale >= 0.999f) return 1.0f;
    float c = C.cjSpeed / g.scale;
    return c < 1.0f ? 1.0f : (c > 8.0f ? 8.0f : c);
}

static void ApplyDiveVelocity(void* ped) {
    double el = Now() - g.diveStart;
    float k = 1.0f - (float)(el / C.diveMove);
    float* ms = F32(ped, OFF_PHYS_MOVESPEED);
    if (k > 0) { ms[0] = g.dirX * C.diveSpeed * k; ms[1] = g.dirY * C.diveSpeed * k; }
    if (!g.upDone && C.diveUp > 0) { ms[2] = C.diveUp; g.upDone = true; }
}

// ------------------------------------------------------------------ hook
DECL_HOOKv(CTimer_Update, void) { Tick(); CTimer_Update(); }

DECL_HOOKv(PlayerPed_ProcessControl, void* self) {
    float f = CompFactor();
    if (f > 1.0001f && pTimeStep && !g.inScaled) {
        float old = *pTimeStep; *pTimeStep = old * f; g.inScaled = true;
        PlayerPed_ProcessControl(self);
        *pTimeStep = old; g.inScaled = false;
    } else PlayerPed_ProcessControl(self);
    if (g.diving && self == FindPlayerPed(-1)) ApplyDiveVelocity(self);
}

DECL_HOOKv(Entity_UpdateAnim, void* self) {
    if (g.scale < 0.999f && !g.inScaled && pTimeStep && self == FindPlayerPed(-1)) {
        float f = CompFactor();
        if (f > 1.0001f) {
            float old = *pTimeStep; *pTimeStep = old * f; g.inScaled = true;
            Entity_UpdateAnim(self);
            *pTimeStep = old; g.inScaled = false;
            return;
        }
    }
    Entity_UpdateAnim(self);
}

static bool WidgetHit(void* w) {
    int n = pMaxTouches ? *pMaxTouches : 5; if (n < 1) n = 1; if (n > 10) n = 10;
    typedef bool (*InsideFn)(void*, V2*, int);
    InsideFn inside = (InsideFn)((*(void***)w)[3]);
    for (int i = 0; i < n; i++) {
        if (!IsTouchDown(i)) continue;
        V2 p = GetTouchPos(i);
        if (inside(w, &p, 0)) return true;
    }
    if (C.useGamepad) {
        int map = *(int*)U8(w, OFF_W_HID);
        if (HidImplements(map) && HidIsPressed(map, nullptr)) return true;
    }
    return false;
}

DECL_HOOKv(HydraUpdate, void* self) {
    bool foot = C.enabled && C.hydraOnFoot && OnFoot() && !PadLocked();
    if (foot) *U8(self, OFF_W_ENABLED) = 1; // sebelum ManageAlpha supaya tombol memudar masuk
    HydraUpdate(self);
    if (!foot) { g.pressPrev = false; return; }
    if (!g.loggedFoot) { g.loggedFoot = true; Log("Hydra on-foot aktif (widget=%p)", self); }
    *U8(self, OFF_W_SHOWN) = 1; // Update bawaan menimpa ini jadi 0 saat tanpa kendaraan
    *U8(self, OFF_W_HILITE) = (g.bt || g.diving) ? 1 : 0;
    bool down = WidgetHit(self);
    if (down && !g.pressPrev) g.evTap = true;
    g.pressPrev = down;
}

DECL_HOOKv(HydraDraw, void* self) {
    if (C.enabled && C.hydraOnFoot && FindPlayerPed(-1) && !FindPlayerVehicle(-1, false)) *U8(self, OFF_W_SHOWN) = 1;
    HydraDraw(self);
}

DECL_HOOKv(KillCalc, void* self, void* victim, void* resp, bool b) {
    float hp = victim ? *F32(victim, OFF_PED_HEALTH) : 0.0f;
    void* damager = *(void**)self;
    KillCalc(self, victim, resp, b);
    if (hp > 0.0f && damager && damager == FindPlayerPed(-1) && victim != damager &&
        (*U8(resp, 8) || *U8(resp, 9))) {
        double now = Now();
        if (victim == g.lastVictim && now - g.lastKillT < 1.0) return;
        g.lastVictim = victim; g.lastKillT = now;
        g.meter += C.killRefill; if (g.meter > 1) g.meter = 1;
        Log("Kill! meter=%.2f", g.meter);
    }
}

// Aim & tembak saat dodge: paksa mode bidik masuk (kalau belum) dan jangan keluar selama melayang.
// Menembak tetap dari tombol tembak pemain (atau otomatis kalau AutoFireDuringDive=1).
DECL_HOOK(bool, PadEnterTargeting, void* pad) {
    bool r = PadEnterTargeting(pad);
    if (r || !g.diving || !C.aimDive) return r;
    void* ped = FindPlayerPed(-1);
    if (ped && HasGun(ped) && Now() - g.diveStart < 0.5 && !IsAiming(ped)) return true;
    return r;
}
DECL_HOOK(bool, PadExitTargeting, void* pad) {
    bool r = PadExitTargeting(pad);
    if (r && g.diving && C.aimDive) { void* ped = FindPlayerPed(-1); if (ped && HasGun(ped)) return false; }
    return r;
}
DECL_HOOK(bool, PadGetWeapon, void* pad, void* ped, bool b) {
    bool r = PadGetWeapon(pad, ped, b);
    if (!r && g.diving && C.autoFire && ped && ped == FindPlayerPed(-1) && HasGun(ped) && IsAiming(ped)) return true;
    return r;
}

static void DrawBar() {
    if (!C.enabled || g.frames < 60) return;
    if (!OnFoot()) return;
    if (g.meter >= 0.999f && !g.bt && !g.diving) return;
    float W = (float)ScreenW(), H = (float)ScreenH();
    float x = C.barX * W, y = C.barY * H, w = C.barW * W, h = C.barH * H;
    Rgba bg = { 0, 0, 0, 150 }, fill = g.bt ? Rgba{ 80, 170, 255, 230 } : Rgba{ 255, 190, 40, 230 };
    if (!g.bt && g.meter < C.actMin) fill = Rgba{ 200, 60, 60, 230 };
    float pad = h * 0.18f;
    RectF o = { x, y, x + w, y + h };
    DrawRectXLU(o, bg, bg, bg, bg);
    float fw = (w - 2 * pad) * (g.meter < 0 ? 0 : (g.meter > 1 ? 1 : g.meter));
    if (fw > 0.5f) { RectF i = { x + pad, y + pad, x + pad + fw, y + h - pad }; DrawRectXLU(i, fill, fill, fill, fill); }
}

DECL_HOOKv(CHud_Draw, void) { CHud_Draw(); DrawBar(); }

// ------------------------------------------------------------------ pasang
static bool Sig(uintptr_t a, uint32_t w0) { return a && *(uint32_t*)a == w0; }

template <class T> static bool Bind(void* h, T& fn, const char* sym) {
    uintptr_t a = aml->GetSym(h, sym); fn = (T)a;
    if (!a) Log("Simbol tidak ketemu: %s", sym);
    return a != 0;
}

ON_MOD_LOAD() {
    logger->SetTag("BulletTimeSA64");
    flog.Write("=== BulletTimeSA64 v2.0 dimulai ===");
    void* h = aml->GetLibHandle("libGTASA.so");
    if (!h) { flog.Error("libGTASA.so tidak ditemukan"); return; }
    LoadCfg();
    g.meter = C.startMeter;

    bool ok = true;
    ok &= Bind(h, FindPlayerPed, "_Z13FindPlayerPedi");
    ok &= Bind(h, FindPlayerVehicle, "_Z17FindPlayerVehicleib");
    ok &= Bind(h, GetPad, "_ZN4CPad6GetPadEi");
    ok &= Bind(h, PadWalkLR, "_ZN4CPad19GetPedWalkLeftRightEv");
    ok &= Bind(h, PadWalkUD, "_ZN4CPad16GetPedWalkUpDownEv");
    ok &= Bind(h, PedInControl, "_ZN4CPed14IsPedInControlEv");
    ok &= Bind(h, PedWeaponSkill, "_ZN4CPed14GetWeaponSkillE11eWeaponType");
    ok &= Bind(h, LoadAnimFileStream, "_ZN12CAnimManager12LoadAnimFileEP8RwStreambPA32_Kc");
    ok &= Bind(h, RwStreamOpenF, "_Z12RwStreamOpen12RwStreamType18RwStreamAccessTypePKv");
    ok &= Bind(h, RwStreamCloseF, "_Z13RwStreamCloseP8RwStreamPv");
    ok &= Bind(h, GetAnimBlock, "_ZN12CAnimManager17GetAnimationBlockEPKc");
    ok &= Bind(h, GetAnimBlockIndex, "_ZN12CAnimManager22GetAnimationBlockIndexEPKc");
    ok &= Bind(h, AddAnimBlockRef, "_ZN12CAnimManager15AddAnimBlockRefEi");
    ok &= Bind(h, GetAnim, "_ZN12CAnimManager12GetAnimationEPKcPK10CAnimBlock");
    ok &= Bind(h, FindActiveTask, "_ZNK12CTaskManager20FindActiveTaskByTypeEi");
    ok &= Bind(h, BlendAnim, "_ZN12CAnimManager14BlendAnimationEP7RpClumpP19CAnimBlendHierarchyif");
    ok &= Bind(h, AssocSetFinishCB, "_ZN21CAnimBlendAssociation17SetFinishCallbackEPFvPS_PvES1_");
    ok &= Bind(h, AssocSetDeleteCB, "_ZN21CAnimBlendAssociation17SetDeleteCallbackEPFvPS_PvES1_");
    ok &= Bind(h, IsTouchDown, "_ZN15CTouchInterface11IsTouchDownEi");
    ok &= Bind(h, GetTouchPos, "_ZN15CTouchInterface16GetTouchPositionEi");
    ok &= Bind(h, HidImplements, "_ZN4CHID10ImplementsE10HIDMapping");
    ok &= Bind(h, HidIsPressed, "_ZN4CHID9IsPressedE10HIDMappingPf");
    ok &= Bind(h, DrawRectXLU, "_ZN9CSprite2d11DrawRectXLUERK5CRectRK5CRGBAS5_S5_S5_");
    ok &= Bind(h, ScreenW, "_Z17OS_ScreenGetWidthv");
    ok &= Bind(h, ScreenH, "_Z18OS_ScreenGetHeightv");
    pTimeScale = (float*)aml->GetSym(h, "_ZN6CTimer13ms_fTimeScaleE");
    pTimeStep = (float*)aml->GetSym(h, "_ZN6CTimer12ms_fTimeStepE");
    pUserPause = (uint8_t*)aml->GetSym(h, "_ZN6CTimer11m_UserPauseE");
    pCodePause = (uint8_t*)aml->GetSym(h, "_ZN6CTimer11m_CodePauseE");
    pMaxTouches = (int*)aml->GetSym(h, "_ZN15CTouchInterface13m_nMaxTouchesE");
    ok &= (pTimeScale && pTimeStep);
    if (!ok) { flog.Error("Ada simbol yang hilang -> mod dimatikan (versi libGTASA.so tidak cocok?)"); return; }

    // pemeriksaan versi: offset struct dipakai, jadi prolog fungsi harus cocok dengan libGTASA.so referensi
    uintptr_t aSkill = aml->GetSym(h, "_ZN4CPed14GetWeaponSkillEv");
    uintptr_t aMove = aml->GetSym(h, "_ZN9CPhysical13ApplyMoveSpeedEv");
    uintptr_t aRw = aml->GetSym(h, "_ZN7CEntity13UpdateRwFrameEv");
    bool ver = Sig(aSkill, 0x39a37008) && Sig(aMove, 0x7940c808) && Sig(aRw, 0xf9401008);
    uintptr_t aHU = aml->GetSym(h, "_ZN23CWidgetButtonHydraulics6UpdateEv");
    uintptr_t aHD = aml->GetSym(h, "_ZN23CWidgetButtonHydraulics4DrawEv");
    uintptr_t aPC = aml->GetSym(h, "_ZN10CPlayerPed14ProcessControlEv");
    uintptr_t aUA = aml->GetSym(h, "_ZN7CEntity10UpdateAnimEv");
    uintptr_t aKC = aml->GetSym(h, "_ZN28CPedDamageResponseCalculator18ComputeWillKillPedEP4CPedR18CPedDamageResponseb");
    uintptr_t aHud = aml->GetSym(h, "_ZN4CHud4DrawEv");
    uintptr_t aTU = aml->GetSym(h, "_ZN6CTimer6UpdateEv");
    uintptr_t aEnter = aml->GetSym(h, "_ZN4CPad17GetEnterTargetingEv");
    uintptr_t aExit = aml->GetSym(h, "_ZN4CPad16GetExitTargetingEv");
    uintptr_t aWpn = aml->GetSym(h, "_ZN4CPad9GetWeaponEP4CPedb");
    flog.Write("Cek versi offset: %s", ver ? "COCOK" : "TIDAK COCOK (hook dimatikan)");
    if (!ver) return;

    if (Sig(aTU, 0xf81d0ff5)) aml->Hook((void*)aTU, (void*)HookOf_CTimer_Update, (void**)&CTimer_Update); else flog.Error("CTimer::Update tidak cocok");
    if (Sig(aHud, 0xa9be4ff4)) aml->Hook((void*)aHud, (void*)HookOf_CHud_Draw, (void**)&CHud_Draw); else flog.Error("CHud::Draw tidak cocok");
    if (C.hookHydra) {
        if (Sig(aHU, 0xfc1d0fe8)) aml->Hook((void*)aHU, (void*)HookOf_HydraUpdate, (void**)&HydraUpdate); else flog.Error("Hydra::Update tidak cocok");
        if (Sig(aHD, 0xd100c3ff)) aml->Hook((void*)aHD, (void*)HookOf_HydraDraw, (void**)&HydraDraw); else flog.Error("Hydra::Draw tidak cocok");
    }
    if (C.hookPlayer) { if (Sig(aPC, 0xd10243ff)) aml->Hook((void*)aPC, (void*)HookOf_PlayerPed_ProcessControl, (void**)&PlayerPed_ProcessControl); else flog.Error("PlayerPed::ProcessControl tidak cocok"); }
    if (C.hookAnim) { if (Sig(aUA, 0xd10103ff)) aml->Hook((void*)aUA, (void*)HookOf_Entity_UpdateAnim, (void**)&Entity_UpdateAnim); else flog.Error("Entity::UpdateAnim tidak cocok"); }
    if (C.hookAim && C.aimDive) {
        if (Sig(aEnter, 0xa9be4ff4)) aml->Hook((void*)aEnter, (void*)HookOf_PadEnterTargeting, (void**)&PadEnterTargeting); else flog.Error("CPad::GetEnterTargeting tidak cocok");
        if (Sig(aExit, 0xa9be4ff4)) aml->Hook((void*)aExit, (void*)HookOf_PadExitTargeting, (void**)&PadExitTargeting); else flog.Error("CPad::GetExitTargeting tidak cocok");
        if (C.autoFire) { if (Sig(aWpn, 0xa9be4ff4)) aml->Hook((void*)aWpn, (void*)HookOf_PadGetWeapon, (void**)&PadGetWeapon); else flog.Error("CPad::GetWeapon tidak cocok"); }
    }
    if (C.hookKill && C.killRefill > 0.0f) { if (Sig(aKC, 0xfc1c0fe8)) aml->Hook((void*)aKC, (void*)HookOf_KillCalc, (void**)&KillCalc); else flog.Error("ComputeWillKillPed tidak cocok"); }
    flog.Write("Hook terpasang. Tekan tombol hidrolik saat jalan kaki.");
}

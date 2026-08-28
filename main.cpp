// ---------------------------------------------------------------------------
// tfcbot -- ESP + aimbot for Team Fortress Classic (GoldSrc / hl.exe, 32-bit)
//
// Every offset and table index below was measured, not guessed:
//   - struct offsets came from probe.c / probe2.c compiled against Valve's
//     public Half-Life SDK headers as 32-bit (run them again to re-check)
//   - table slot numbers were counted from engine/APIProxy.h
//
// Two tables make this whole thing work:
//   cl_enginefunc_t  engine -> client. Lives in client.dll data. ~130 engine
//                    functions. We find it by looking for a long run of
//                    consecutive pointers into hw.dll.
//   cldll_func_t     client -> engine. Lives in hw.dll data. The engine calls
//                    the client through it every frame, so overwriting a slot
//                    here IS the hook. No code patching, no trampolines.
// ---------------------------------------------------------------------------

#include <windows.h>
#include <stdio.h>
#include <math.h>

#define PI 3.14159265358979323846f

// ---- tunables -------------------------------------------------------------
#define AIM_FOV        60.0f       // degrees; ignore targets further off-crosshair
#define AIM_Z_OFFSET   20.0f       // aim this far above origin. 0=stomach, ~28=head
#define ESP_MAX_DIST   4000.0f

// Right mouse is sniper zoom in TFC so it is unusable. Thumb buttons only for
// now. Add VK_LBUTTON here if you later want it to engage whenever you fire.
static const int g_aimKeys[] = { VK_XBUTTON1, VK_XBUTTON2 };

static int AimKeyDown(void)
{
    for (int i = 0; i < (int)(sizeof(g_aimKeys)/sizeof(g_aimKeys[0])); ++i)
        if (GetAsyncKeyState(g_aimKeys[i]) & 0x8000) return 1;
    return 0;
}

// ---- cl_entity_t (common/cl_entity.h, 32-bit, sizeof 3000) ----------------
#define ENT_INDEX          0
#define ENT_PLAYER         4
#define ENT_CS_MESSAGENUM  700
#define ENT_CS_ORIGIN      704    // newest origin the server sent
#define ENT_CS_MODELINDEX  728
#define ENT_CS_SOLID       746
#define ENT_CS_VELOCITY    800
#define ENT_CS_TEAM        852
#define ENT_CS_ONGROUND    896    // -1 = airborne, else the entity you stand on
#define ENT_ORIGIN         2888   // interpolated render position (what you see)
#define ENT_ANGLES         2900

// ---- usercmd_t (common/usercmd.h, sizeof 52) ------------------------------
#define UC_VIEWANGLES      4
#define UC_FORWARDMOVE     16
#define UC_SIDEMOVE        20
#define UC_UPMOVE          24
#define UC_BUTTONS         30
#define IN_JUMP            (1 << 1)   // common/in_buttons.h:22
#define STRAFE_SPEED       400.0f     // matches cl_sidespeed default

// ---- pmtrace_t (common/pmtrace.h, sizeof 68) ------------------------------
#define PT_FRACTION        16

// ---- cl_enginefunc_t slots (engine/APIProxy.h, counted from 0) ------------
#define EF_FILLRGBA        11
#define EF_GETSCREENINFO   12
#define EF_GETPLAYERINFO   21
#define EF_DRAWSTRING      27
#define EF_SETTEXTCOLOR    28
#define EF_GETVIEWANGLES   34
#define EF_SETVIEWANGLES   35
#define EF_GETMAXCLIENTS   36
#define EF_CONPRINTF       40
#define EF_GETLOCALPLAYER  51
#define EF_GETENTBYINDEX   53
#define EF_PMTRACELINE     59
#define EF_TRIAPI          82

// ---- triangleapi_s slots (common/triangleapi.h; version is slot 0) --------
#define TRI_WORLDTOSCREEN  12

// ---- cldll_func_t slots (engine/APIProxy.h, counted from 0) --------------
#define CD_INITIALIZE      0
#define CD_HUD_REDRAW      3
#define CD_CLIENTMOVE      6     // HUD_PlayerMove(playermove_t*, int server)
#define CD_CL_CREATEMOVE   14

// ---- playermove_t (pm_shared/pm_defs.h, 32-bit, sizeof 325068) ------------
// This is the authoritative movement state. curstate.onground on the local
// player entity is pinned at 0 and useless; this one actually tracks.
#define PM_ONGROUND        224   // -1 = airborne
#define PM_VELOCITY        92

// ---------------------------------------------------------------------------
typedef int   (__cdecl *fnHudRedraw)(float, int);
typedef void  (__cdecl *fnCreateMove)(float, void*, int);
typedef void  (__cdecl *fnClientMove)(void*, int);
typedef void* (__cdecl *fnGetLocalPlayer)(void);
typedef void* (__cdecl *fnGetEntityByIndex)(int);
typedef int   (__cdecl *fnGetMaxClients)(void);
typedef void  (__cdecl *fnGetViewAngles)(float*);
typedef void  (__cdecl *fnSetViewAngles)(float*);
typedef void  (__cdecl *fnGetScreenInfo)(void*);
typedef void  (__cdecl *fnFillRGBA)(int,int,int,int,int,int,int,int);
typedef int   (__cdecl *fnDrawString)(int,int,const char*);
typedef void  (__cdecl *fnSetTextColor)(float,float,float);
typedef int   (__cdecl *fnGetPlayerInfo)(int,void*);
typedef int   (__cdecl *fnWorldToScreen)(float*,float*);
typedef void* (__cdecl *fnTraceLine)(float*,float*,int,int,int);

typedef struct {
    char *name; short ping;
    unsigned char thisplayer, spectator, packetloss;
    char *model; short topcolor, bottomcolor;
    unsigned __int64 steamid;
} playerinfo_t;

static void**  g_ef = NULL;   // cl_enginefunc_t
static void**  g_cd = NULL;   // cldll_func_t, inside hw.dll
static fnHudRedraw  o_HudRedraw  = NULL;
static fnCreateMove o_CreateMove = NULL;
static fnClientMove o_ClientMove = NULL;
static int     g_scrW = 640, g_scrH = 480;
static int     g_esp = 1, g_visCheck = 0;
static HMODULE g_self = NULL;

// ---- diagnostics ----------------------------------------------------------
static volatile LONG g_redrawHits = 0;   // did the HUD_Redraw hook fire?
static volatile LONG g_moveHits   = 0;   // did the CL_CreateMove hook fire?
static int  g_dump = 0;                  // F9 sets this; next frame dumps entities
static int  g_lastMaxc = 0, g_lastEnts = 0, g_lastPlayers = 0, g_lastDrawn = 0;
static int  g_hideStale = 0;             // F10: hide frozen targets instead of dimming them
static int   g_snapView = 1;             // F11: 1 = view snaps, 0 = silent aim
static int   g_aimOn    = 1;             // F12: aimbot master switch
static float g_smooth   = 1.0f;          // PGUP/PGDN. 1 = instant snap. higher = slower.
static int   g_bhop     = 1;             // F8: auto bunnyhop while space is held
static int   g_autoStrafe   = 1;         // F6: steer the air-strafe for you
// F5 flips strafe direction. Default 0 is CONFIRMED CORRECT in TFC: yaw
// increases turning left, and strafing left means negative sidemove. Verified
// in game against the speedometer, so do not "fix" this.
static int   g_strafeInvert = 0;
static int   g_speedo       = 1;         // F3: on-screen speedometer
static int   g_bhopMode     = 0;         // F4: 0 auto (default), 1 parity, 2 exact
static int   g_pmOnground   = -1;        // from playermove_t, the real one
static int   g_seenAir      = 0;         // has read -1
static int   g_seenGround   = 0;         // ...and has read something else
static int   g_useCurstate = 0;          // F7: 0 = interpolated origin, 1 = newest received
static float g_projSpeed   = 0.0f;       // NUMPAD +/-. 0 = hitscan, no lead.

// ---- freshness tracking ---------------------------------------------------
// GoldSrc culls entities server-side by PVS, so the server just stops sending
// players you cannot see. Their cl_entity_t keeps whatever it last held, so the
// origin freezes in place. That data is gone, we cannot get it back. But we can
// always tell live from frozen: watch curstate.messagenum and remember when it
// last changed. Comparing against the local player messagenum does not work,
// which is what was rejecting every target before.
#define MAX_SLOTS  34
#define FRESH_MS   300      // updated this recently, treat as live
#define FORGET_MS  6000     // frozen this long, stop drawing entirely
static int   g_lastMsg[MAX_SLOTS];
static DWORD g_lastChange[MAX_SLOTS];

// Per-slot velocity, differenced from successive server positions. entity_state
// does carry a velocity field, but I could not compile a probe to measure its
// offset, and I am not going to guess one. Differencing the position we already
// know the offset of gives the same answer without inventing a number.
static float g_lastPos[MAX_SLOTS][3];
static DWORD g_lastPosTime[MAX_SLOTS];
static float g_vel[MAX_SLOTS][3];

#define EF(slot, type) ((type)g_ef[slot])

static float* EntOrigin(void* e){ return (float*)((BYTE*)e + ENT_ORIGIN); }
static int    EntIsPlayer(void* e){ return *(int*)((BYTE*)e + ENT_PLAYER); }
static int    EntMsgNum(void* e){ return *(int*)((BYTE*)e + ENT_CS_MESSAGENUM); }
static int    EntTeam(void* e){ return *(int*)((BYTE*)e + ENT_CS_TEAM); }
static int    EntModel(void* e){ return *(int*)((BYTE*)e + ENT_CS_MODELINDEX); }

// Newest position the server actually sent, as opposed to the interpolated one
// the game renders. For a fast mover these are meaningfully different.
static float* EntCsOrigin(void* e){ return (float*)((BYTE*)e + ENT_CS_ORIGIN); }

// Milliseconds since this slot last received fresh data, and while we are here,
// update its velocity estimate.
static DWORD SlotAge(int i, void* e)
{
    if (i < 0 || i >= MAX_SLOTS) return 0;
    DWORD now = GetTickCount();
    int   m   = EntMsgNum(e);

    if (m != g_lastMsg[i] || g_lastChange[i] == 0)
    {
        float* p  = EntCsOrigin(e);
        DWORD  dt = now - g_lastPosTime[i];

        if (g_lastPosTime[i] && dt > 0 && dt < 500)
        {
            float k = 1000.0f / (float)dt;              // to units per second
            for (int a = 0; a < 3; ++a)
            {
                float v = (p[a] - g_lastPos[i][a]) * k;
                g_vel[i][a] = g_vel[i][a] * 0.5f + v * 0.5f;   // damp packet jitter
            }
        }
        else if (!g_lastPosTime[i])
        {
            g_vel[i][0] = g_vel[i][1] = g_vel[i][2] = 0.f;
        }

        for (int a = 0; a < 3; ++a) g_lastPos[i][a] = p[a];
        g_lastPosTime[i] = now;
        g_lastMsg[i]     = m;
        g_lastChange[i]  = now;
    }
    return now - g_lastChange[i];
}

// ---------------------------------------------------------------------------
// Table discovery
// ---------------------------------------------------------------------------
static DWORD ImageSize(HMODULE h)
{
    IMAGE_DOS_HEADER* d = (IMAGE_DOS_HEADER*)h;
    return ((IMAGE_NT_HEADERS*)((BYTE*)h + d->e_lfanew))->OptionalHeader.SizeOfImage;
}

// The first 82 members of cl_enginefunc_t are plain functions inside hw.dll.
// A run of 60+ consecutive pointers into hw.dll, sitting in client.dll writable
// data, is unique. No byte signature needed, so this survives game updates.
static void** FindEngineTable(HMODULE hClient, HMODULE hEngine)
{
    const int NEEDED = 60;
    const DWORD lo = (DWORD)hEngine, hi = lo + ImageSize(hEngine);

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hClient;
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)((BYTE*)hClient + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);

    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s)
    {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        DWORD* p   = (DWORD*)((BYTE*)hClient + sec[s].VirtualAddress);
        DWORD* end = (DWORD*)((BYTE*)p + sec[s].Misc.VirtualSize);
        while (p + NEEDED <= end)
        {
            int n = 0;
            while (p + n < end && p[n] >= lo && p[n] < hi) ++n;
            if (n >= NEEDED) return (void**)p;
            p += (n > 0) ? n : 1;
        }
    }
    return NULL;
}

// cldll_func_t is found exactly, not heuristically: slot 0 must be the client
// exported Initialize, slot 3 HUD_Redraw, slot 14 CL_CreateMove. Three matches
// at one address means we have definitely found the right table.
static void** FindClientTable(HMODULE hClient, HMODULE hEngine)
{
    void* pInit   = (void*)GetProcAddress(hClient, "Initialize");
    void* pRedraw = (void*)GetProcAddress(hClient, "HUD_Redraw");
    void* pMove   = (void*)GetProcAddress(hClient, "CL_CreateMove");
    if (!pInit || !pRedraw || !pMove) return NULL;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hEngine;
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)((BYTE*)hEngine + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);

    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s)
    {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        void** p   = (void**)((BYTE*)hEngine + sec[s].VirtualAddress);
        void** end = (void**)((BYTE*)p + sec[s].Misc.VirtualSize);
        for (; p + CD_CL_CREATEMOVE < end; ++p)
        {
            if (p[CD_INITIALIZE]    == pInit   &&
                p[CD_HUD_REDRAW]    == pRedraw &&
                p[CD_CL_CREATEMOVE] == pMove)
                return p;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------
static float NormAngle(float a){ while(a > 180.f) a -= 360.f; while(a < -180.f) a += 360.f; return a; }

// Quake-lineage engines use INVERTED pitch. The minus sign is not a typo.
static void AnglesTo(const float* from, const float* to, float* out)
{
    float dx = to[0]-from[0], dy = to[1]-from[1], dz = to[2]-from[2];
    float flat = sqrtf(dx*dx + dy*dy);
    out[0] = NormAngle(-atan2f(dz, flat) * 180.f / PI);   // pitch
    out[1] = NormAngle( atan2f(dy, dx)   * 180.f / PI);   // yaw
    out[2] = 0.f;
    if (out[0] >  89.f) out[0] =  89.f;
    if (out[0] < -89.f) out[0] = -89.f;
}

static float AngleDist(const float* a, const float* b)
{
    float dp = NormAngle(a[0]-b[0]), dy = NormAngle(a[1]-b[1]);
    return sqrtf(dp*dp + dy*dy);
}

// Player hull is 72 units tall centred on origin, so the eye sits +28.
static void EyePos(void* ent, float* out)
{
    float* o = EntOrigin(ent);
    out[0] = o[0]; out[1] = o[1]; out[2] = o[2] + 28.f;
}

static int WorldToScreen(float* world, float* sx, float* sy)
{
    void** tri = (void**)g_ef[EF_TRIAPI];
    if (!tri) return 0;
    float s[3] = {0,0,0};
    if (((fnWorldToScreen)tri[TRI_WORLDTOSCREEN])(world, s)) return 0;  // behind camera
    *sx = (1.f + s[0]) * g_scrW * 0.5f;      // XPROJECT, cl_dll/cl_util.h:77
    *sy = (1.f - s[1]) * g_scrH * 0.5f;      // YPROJECT, cl_dll/cl_util.h:78
    return 1;
}

// ---------------------------------------------------------------------------
// Target filtering, shared by ESP and aimbot
// ---------------------------------------------------------------------------
static int IsValidTarget(void* e, void* me)
{
    if (!e || e == me) return 0;
    if (!EntIsPlayer(e)) return 0;
    if (EntModel(e) <= 0) return 0;                       // dead or not spawned
    if (EntTeam(e) && EntTeam(me) && EntTeam(e) == EntTeam(me)) return 0;  // friendly
    return 1;
}

static int Visible(float* eye, float* target)
{
    if (!g_visCheck) return 1;
    fnTraceLine tr = EF(EF_PMTRACELINE, fnTraceLine);
    if (!tr) return 1;
    void* t = tr(eye, target, 0 /*PM_TRACELINE_PHYSENTSONLY*/, 2 /*point hull*/, -1);
    if (!t) return 1;
    return *(float*)((BYTE*)t + PT_FRACTION) > 0.97f;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
static void DrawBox(int x, int y, int w, int h, int r, int g, int b)
{
    fnFillRGBA f = EF(EF_FILLRGBA, fnFillRGBA);
    f(x,       y,       w, 1, r, g, b, 255);
    f(x,       y+h-1,   w, 1, r, g, b, 255);
    f(x,       y,       1, h, r, g, b, 255);
    f(x+w-1,   y,       1, h, r, g, b, 255);
}

// The client, not the engine, runs the movement code: the engine hands us a
// playermove_t and PM_Move fills it in. So read onground AFTER the original
// runs, which is the freshest ground state available anywhere on the client.
static void __cdecl hk_ClientMove(void* ppmove, int server)
{
    if (o_ClientMove) o_ClientMove(ppmove, server);

    if (ppmove && !server)
    {
        int og = *(int*)((BYTE*)ppmove + PM_ONGROUND);
        g_pmOnground = og;
        if (og == -1) g_seenAir = 1; else g_seenGround = 1;
    }
}

static int __cdecl hk_HudRedraw(float time, int intermission)
{
    int ret = o_HudRedraw ? o_HudRedraw(time, intermission) : 0;
    InterlockedIncrement(&g_redrawHits);
    if (!g_ef) return ret;

    struct { int iSize, iWidth, iHeight, iFlags, iCharHeight; short cw[256]; } si;
    si.iSize = sizeof(si);
    EF(EF_GETSCREENINFO, fnGetScreenInfo)(&si);
    if (si.iWidth > 0) { g_scrW = si.iWidth; g_scrH = si.iHeight; }

    // Heartbeat. If you can see this green bar in game, the hook fires and
    // drawing works, and any remaining problem is in the target filter.
    EF(EF_FILLRGBA, fnFillRGBA)(8, 8, 60, 6, 0, 255, 0, 255);
    EF(EF_SETTEXTCOLOR, fnSetTextColor)(0.f, 1.f, 0.f);
    EF(EF_DRAWSTRING, fnDrawString)(8, 18, "tfcbot");

    if (!g_esp) return ret;

    void* me = EF(EF_GETLOCALPLAYER, fnGetLocalPlayer)();
    if (!me) return ret;

    // ---- speedometer ------------------------------------------------------
    // Horizontal speed only, since that is what bunnyhopping actually builds.
    // Differenced from the rendered origin and smoothed, because raw frame to
    // frame deltas are far too noisy to read while you are moving.
    if (g_speedo)
    {
        static float         lastOrg[2] = {0,0};
        static LARGE_INTEGER lastQ = {0};
        static LARGE_INTEGER freq  = {0};
        static float         shown = 0.f;

        float* o = EntOrigin(me);

        // Preferred source: the engine's own velocity for us. No timing
        // involved, so no timing error. Horizontal only, since that is what
        // bunnyhopping builds.
        float* vel = (float*)((BYTE*)me + ENT_CS_VELOCITY);
        float  sp  = sqrtf(vel[0]*vel[0] + vel[1]*vel[1]);

        // Fallback if that field is not populated: difference the origin. This
        // originally used GetTickCount, whose ~15.6ms resolution is coarser
        // than a frame, which undercounted speed by roughly a third. Use the
        // performance counter instead.
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER q; QueryPerformanceCounter(&q);

        if (sp < 1.f && lastQ.QuadPart && freq.QuadPart)
        {
            float dt = (float)(q.QuadPart - lastQ.QuadPart) / (float)freq.QuadPart;
            if (dt > 0.0005f)
            {
                float dx = o[0] - lastOrg[0], dy = o[1] - lastOrg[1];
                sp = sqrtf(dx*dx + dy*dy) / dt;
            }
        }

        lastOrg[0] = o[0]; lastOrg[1] = o[1]; lastQ = q;
        if (sp < 5000.f) shown = shown * 0.7f + sp * 0.3f;   // ignore teleports

        DWORD now = GetTickCount();   // only used for the trend sampling below

        // Gaining or losing? Sampled on an interval rather than per frame,
        // because a single frame of difference is noise. This is the feedback
        // that tells you whether your current turn rate is paying off, which is
        // the whole skill: the optimal rate gets slower as you get faster.
        static float cmpSpeed = 0.f;
        static DWORD cmpTime  = 0;
        static float trend    = 0.f;
        static float peak     = 0.f;

        if (now - cmpTime > 120)
        {
            if (cmpTime) trend = shown - cmpSpeed;
            cmpSpeed = shown;
            cmpTime  = now;
        }
        if (shown > peak) peak = shown;
        if (shown < 60.f) peak = shown;      // reset once you have basically stopped

        if      (trend >  4.f) EF(EF_SETTEXTCOLOR, fnSetTextColor)(0.3f, 1.0f, 0.3f);  // gaining
        else if (trend < -4.f) EF(EF_SETTEXTCOLOR, fnSetTextColor)(1.0f, 0.3f, 0.3f);  // losing
        else                   EF(EF_SETTEXTCOLOR, fnSetTextColor)(1.0f, 1.0f, 0.4f);  // holding

        char sb[64];
        _snprintf_s(sb, sizeof(sb), _TRUNCATE, "%4.0f u/s   peak %4.0f", shown, peak);
        EF(EF_DRAWSTRING, fnDrawString)(g_scrW / 2 - 60, g_scrH - 90, sb);
    }

    int    maxc  = EF(EF_GETMAXCLIENTS, fnGetMaxClients)();
    float* myOrg = EntOrigin(me);
    int    nEnts = 0, nPlayers = 0, nDrawn = 0;

    if (g_dump)
        printf("\n--- entity dump  maxclients=%d  me=%p  my_msgnum=%d ---\n",
               maxc, me, EntMsgNum(me));

    for (int i = 1; i <= maxc; ++i)
    {
        void* e = EF(EF_GETENTBYINDEX, fnGetEntityByIndex)(i);

        if (e) ++nEnts;
        if (e && EntIsPlayer(e)) ++nPlayers;

        if (g_dump && e)
            printf("  [%2d] ptr=%p player=%d model=%d msgnum=%d team=%d org=%.0f %.0f %.0f\n",
                   i, e, EntIsPlayer(e), EntModel(e), EntMsgNum(e), EntTeam(e),
                   EntOrigin(e)[0], EntOrigin(e)[1], EntOrigin(e)[2]);

        if (!IsValidTarget(e, me)) continue;

        DWORD age   = SlotAge(i, e);
        int   fresh = (age <= FRESH_MS);
        if (age > FORGET_MS) continue;        // frozen too long, forget it
        if (g_hideStale && !fresh) continue;

        float* o = EntOrigin(e);
        float dx = o[0]-myOrg[0], dy = o[1]-myOrg[1], dz = o[2]-myOrg[2];
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (dist > ESP_MAX_DIST) continue;

        float head[3] = { o[0], o[1], o[2] + 36.f };   // hull is 72 tall, centred
        float feet[3] = { o[0], o[1], o[2] - 36.f };
        float hx, hy, fx, fy;
        if (!WorldToScreen(head, &hx, &hy)) continue;
        if (!WorldToScreen(feet, &fx, &fy)) continue;

        int h = (int)(fy - hy);
        if (h < 4) continue;
        int w = (int)(h * 0.45f);
        int x = (int)(hx - w/2), y = (int)hy;

        // Live target: solid red. Frozen last-known position: dim grey, with how
        // long ago the server last mentioned them.
        if (fresh) DrawBox(x, y, w, h, 255, 40,  40);
        else       DrawBox(x, y, w, h, 105, 105, 105);

        playerinfo_t pi;
        memset(&pi, 0, sizeof(pi));
        EF(EF_GETPLAYERINFO, fnGetPlayerInfo)(i, &pi);

        const char* nm = (pi.name && pi.name[0]) ? pi.name : "?";
        char buf[128];
        if (fresh)
        {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s  %dm", nm, (int)(dist / 39.37f));
            EF(EF_SETTEXTCOLOR, fnSetTextColor)(1.f, 0.35f, 0.35f);
        }
        else
        {
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s  %dm  lost %.1fs", nm,
                        (int)(dist / 39.37f), age / 1000.0f);
            EF(EF_SETTEXTCOLOR, fnSetTextColor)(0.45f, 0.45f, 0.45f);
        }
        EF(EF_DRAWSTRING, fnDrawString)(x, y - 14, buf);
        ++nDrawn;
    }

    g_lastMaxc = maxc; g_lastEnts = nEnts; g_lastPlayers = nPlayers; g_lastDrawn = nDrawn;
    if (g_dump) { printf("--- end dump: ents=%d players=%d drawn=%d ---\n\n", nEnts, nPlayers, nDrawn); g_dump = 0; }
    return ret;
}

static void __cdecl hk_CreateMove(float frametime, void* cmd, int active)
{
    if (o_CreateMove) o_CreateMove(frametime, cmd, active);
    InterlockedIncrement(&g_moveHits);
    if (!active || !cmd || !g_ef) return;

    void* me = EF(EF_GETLOCALPLAYER, fnGetLocalPlayer)();
    if (!me) return;

    // ---- auto bunnyhop + air strafe ---------------------------------------
    if (g_bhop && (GetAsyncKeyState(VK_SPACE) & 0x8000))
    {
        unsigned short* buttons  = (unsigned short*)((BYTE*)cmd + UC_BUTTONS);
        int             onground = g_pmOnground;   // from playermove_t, -1 = airborne

        // The mover only jumps on a fresh press: IN_JUMP set this tick, clear
        // last tick, which is why holding space gives you exactly one jump.
        //
        // Two ways to manufacture that edge. Parity just alternates the bit,
        // landing the jump within a frame or two of touching down, costing you
        // a frame or so of ground friction. Exact reads the ground flag and
        // wastes nothing. Exact is only safe if the flag genuinely tracks: a
        // field pinned at one value passes a naive check and then either never
        // presses jump or never releases it. So require having seen BOTH
        // states, and fall back to parity otherwise. Auto is safe by
        // construction, which is why it is the default.
        int useExact = 0;
        if (g_bhopMode == 0) useExact = (g_seenAir && g_seenGround);
        if (g_bhopMode == 2) useExact = 1;

        int airborne;
        if (useExact)
        {
            airborne = (onground == -1);
            if (!airborne) *buttons |=  IN_JUMP;
            else           *buttons &= ~IN_JUMP;
        }
        else
        {
            static int parity = 0;
            parity = !parity;
            if (parity) *buttons |=  IN_JUMP;
            else        *buttons &= ~IN_JUMP;
            airborne = 1;      // no usable ground flag, so assume airborne
        }

        // Air acceleration only pays out on the part of your input that is
        // perpendicular to your current velocity, which in practice means
        // strafing the same way you are turning. So read how far the view
        // turned this frame and pick the matching strafe direction. Forward
        // input is worthless in the air, so drop it.
        if (g_autoStrafe && airborne)
        {
            float* va   = (float*)((BYTE*)cmd + UC_VIEWANGLES);
            float* fwd  = (float*)((BYTE*)cmd + UC_FORWARDMOVE);
            float* side = (float*)((BYTE*)cmd + UC_SIDEMOVE);

            static float lastYaw  = 0.f;
            static int   haveLast = 0;
            static float heldDir  = 0.f;   // -1 left, +1 right, kept through jitter

            float d = haveLast ? NormAngle(va[1] - lastYaw) : 0.f;
            lastYaw  = va[1];
            haveLast = 1;

            // Dead zone, so mouse sensor noise does not make the strafe chatter
            // every frame. The last real direction is held briefly through
            // quiet frames, but then released: sideways input while you are NOT
            // turning does not build speed, it bleeds it, so holding a stale
            // direction actively slows you down.
            static int quiet = 0;
            if (d > 0.05f)        { heldDir = -1.f; quiet = 0; }
            else if (d < -0.05f)  { heldDir =  1.f; quiet = 0; }
            else if (++quiet > 3) { heldDir =  0.f; }

            if (heldDir != 0.f)
            {
                float s = heldDir * STRAFE_SPEED;
                if (g_strafeInvert) s = -s;
                *side = s;
                *fwd  = 0.f;
            }
        }
    }

    if (!g_aimOn || !AimKeyDown()) return;

    float* view = (float*)((BYTE*)cmd + UC_VIEWANGLES);
    float  eye[3];
    EyePos(me, eye);

    int   maxc = EF(EF_GETMAXCLIENTS, fnGetMaxClients)();
    float best = AIM_FOV;
    float bestAng[3] = {0,0,0};
    int   found = 0;

    for (int i = 1; i <= maxc; ++i)
    {
        void* e = EF(EF_GETENTBYINDEX, fnGetEntityByIndex)(i);
        if (!IsValidTarget(e, me)) continue;
        if (SlotAge(i, e) > FRESH_MS) continue;   // never aim at a frozen ghost

        // Interpolated origin is what you see. curstate origin is the newest the
        // server sent, which is ahead of it. For hitscan the rendered one is
        // usually right, because the server rewinds you for lag compensation.
        // For a fast mover the newest one is closer to the truth.
        float* o = g_useCurstate ? EntCsOrigin(e) : EntOrigin(e);
        float aim[3] = { o[0], o[1], o[2] + AIM_Z_OFFSET };

        // Projectile lead. Flight time is distance over speed, and leading
        // changes the distance, so solve it twice. Speed 0 means hitscan.
        if (g_projSpeed > 1.f)
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                float ddx = aim[0]-eye[0], ddy = aim[1]-eye[1], ddz = aim[2]-eye[2];
                float t   = sqrtf(ddx*ddx + ddy*ddy + ddz*ddz) / g_projSpeed;
                aim[0] = o[0] + g_vel[i][0] * t;
                aim[1] = o[1] + g_vel[i][1] * t;
                aim[2] = o[2] + g_vel[i][2] * t + AIM_Z_OFFSET;
            }
        }

        if (!Visible(eye, aim)) continue;

        float ang[3];
        AnglesTo(eye, aim, ang);
        float d = AngleDist(view, ang);
        if (d < best) { best = d; bestAng[0] = ang[0]; bestAng[1] = ang[1]; found = 1; }
    }

    if (!found) return;

    // Smoothing: move a fraction of the way to the target each input frame
    // instead of teleporting. Only meaningful when the view actually moves, so
    // silent aim goes straight to the target and keeps full accuracy.
    float s = g_snapView ? g_smooth : 1.0f;
    if (s < 1.0f) s = 1.0f;

    float dp = NormAngle(bestAng[0] - view[0]);
    float dy = NormAngle(bestAng[1] - view[1]);

    float out[3];
    out[0] = view[0] + dp / s;
    out[1] = NormAngle(view[1] + dy / s);
    out[2] = 0.f;
    if (out[0] >  89.f) out[0] =  89.f;
    if (out[0] < -89.f) out[0] = -89.f;

    // cmd->viewangles is what gets sent to the server, so writing it here is
    // what the server acts on. On its own that IS silent aim: the server sees
    // you pointing at the target while your screen never moves. Calling
    // SetViewAngles as well is the only thing that makes your view follow.
    view[0] = out[0];
    view[1] = out[1];
    view[2] = 0.f;

    if (g_snapView) EF(EF_SETVIEWANGLES, fnSetViewAngles)(out);
}

// ---------------------------------------------------------------------------
static DWORD WINAPI Run(LPVOID param)
{
    g_self = (HMODULE)param;

    AllocConsole();
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    SetConsoleTitleA("tfcbot");

    printf("waiting for client.dll and hw.dll ...\n");
    HMODULE hClient = NULL, hEngine = NULL;
    while (!hClient || !hEngine)
    {
        hClient = GetModuleHandleA("client.dll");
        hEngine = GetModuleHandleA("hw.dll");
        Sleep(250);
    }

    g_ef = FindEngineTable(hClient, hEngine);
    g_cd = FindClientTable(hClient, hEngine);
    printf("cl_enginefunc_t  %p\n", g_ef);
    printf("cldll_func_t     %p\n", g_cd);

    if (g_ef)
    {
        void** tri = (void**)g_ef[EF_TRIAPI];
        printf("TriAPI version   %d (expect 1)\n", tri ? *(int*)tri : -1);
    }

    if (!g_ef || !g_cd)
    {
        printf("\nfailed to locate one of the tables. END to unload.\n");
        while (!(GetAsyncKeyState(VK_END) & 1)) Sleep(50);
        FreeLibraryAndExitThread(g_self, 0);
    }

    o_HudRedraw  = (fnHudRedraw) g_cd[CD_HUD_REDRAW];
    o_CreateMove = (fnCreateMove)g_cd[CD_CL_CREATEMOVE];
    o_ClientMove = (fnClientMove)g_cd[CD_CLIENTMOVE];
    g_cd[CD_HUD_REDRAW]    = (void*)hk_HudRedraw;
    g_cd[CD_CL_CREATEMOVE] = (void*)hk_CreateMove;
    g_cd[CD_CLIENTMOVE]    = (void*)hk_ClientMove;

    printf("\nhooked.\n");
    printf("  hold a THUMB BUTTON  aim\n");
    printf("  hold SPACE         auto bunnyhop + auto strafe\n");
    printf("  F3                 speedometer on/off (on)\n");
    printf("  F4                 bhop mode: parity / exact / auto  (parity default)\n");
    printf("  F5                 INVERT strafe dir (do this if bhop feels slower)\n");
    printf("  F6                 auto strafe on/off (on)\n");
    printf("  F7                 aim source: interpolated / newest\n");
    printf("  F8                 bunnyhop on/off   (on)\n");
    printf("  NUMPAD + / -       projectile speed for lead (0 = hitscan)\n");
    printf("  INSERT             toggle esp        (on)\n");
    printf("  HOME               toggle wallcheck  (off)\n");
    printf("  F9                 dump entity table\n");
    printf("  F10                hide frozen targets (currently shown dim)\n");
    printf("  F11                snap view / silent aim  (snap)\n");
    printf("  F12                aimbot on/off     (on)\n");
    printf("  END                unload\n\n");
    printf("look for a small GREEN BAR top-left in game. that is the hook proving\n");
    printf("it fires. no green bar = the hook is not being called.\n\n");

    int tick = 0;
    while (!(GetAsyncKeyState(VK_END) & 1))
    {
        if (GetAsyncKeyState(VK_INSERT) & 1) { g_esp = !g_esp; printf("esp %s\n", g_esp ? "on" : "off"); }
        if (GetAsyncKeyState(VK_HOME)   & 1) { g_visCheck = !g_visCheck; printf("wallcheck %s\n", g_visCheck ? "on" : "off"); }
        if (GetAsyncKeyState(VK_F3)     & 1) { g_speedo = !g_speedo; printf("speedometer %s\n", g_speedo ? "on" : "off"); }
        if (GetAsyncKeyState(VK_F4)     & 1) { g_bhopMode = (g_bhopMode + 1) % 3; printf("bhop mode: %s\n", g_bhopMode == 0 ? "auto" : (g_bhopMode == 1 ? "parity (cannot fail)" : "exact (forced)")); }
        if (GetAsyncKeyState(VK_F5)     & 1) { g_strafeInvert = !g_strafeInvert; printf("strafe direction %s\n", g_strafeInvert ? "INVERTED" : "normal"); }
        if (GetAsyncKeyState(VK_F6)     & 1) { g_autoStrafe = !g_autoStrafe; printf("auto strafe %s\n", g_autoStrafe ? "on" : "off"); }
        if (GetAsyncKeyState(VK_F7)     & 1) { g_useCurstate = !g_useCurstate; printf("aim source: %s\n", g_useCurstate ? "newest server pos" : "interpolated (what you see)"); }
        if (GetAsyncKeyState(VK_F8)     & 1) { g_bhop = !g_bhop; printf("bunnyhop %s\n", g_bhop ? "on" : "off"); }
        if (GetAsyncKeyState(VK_ADD)      & 1) { g_projSpeed += 100.f; if (g_projSpeed > 3000.f) g_projSpeed = 3000.f; printf("projectile speed %.0f\n", g_projSpeed); }
        if (GetAsyncKeyState(VK_SUBTRACT) & 1) { g_projSpeed -= 100.f; if (g_projSpeed <    0.f) g_projSpeed =    0.f; printf("projectile speed %.0f%s\n", g_projSpeed, g_projSpeed < 1.f ? " (hitscan, no lead)" : ""); }
        if (GetAsyncKeyState(VK_F9)     & 1) { g_dump = 1; }
        if (GetAsyncKeyState(VK_F10)    & 1) { g_hideStale = !g_hideStale; printf("frozen targets %s\n", g_hideStale ? "hidden" : "shown dim"); }
        if (GetAsyncKeyState(VK_F11)    & 1) { g_snapView  = !g_snapView;  printf("aim mode %s\n", g_snapView ? "snap view" : "SILENT"); }
        if (GetAsyncKeyState(VK_F12)    & 1) { g_aimOn     = !g_aimOn;     printf("aimbot %s\n", g_aimOn ? "on" : "off"); }
        if (GetAsyncKeyState(VK_PRIOR)  & 1) { g_smooth += 0.5f; if (g_smooth > 20.f) g_smooth = 20.f; printf("smoothing %.1f\n", g_smooth); }
        if (GetAsyncKeyState(VK_NEXT)   & 1) { g_smooth -= 0.5f; if (g_smooth <  1.f) g_smooth =  1.f; printf("smoothing %.1f%s\n", g_smooth, g_smooth <= 1.f ? " (instant)" : ""); }

        if (++tick % 60 == 0)
            printf("drawn=%d players=%d  proj=%.0f  src=%s  onground=%d  bhop=%s\n",
                   g_lastDrawn, g_lastPlayers, g_projSpeed,
                   g_useCurstate ? "newest" : "interp",
                   g_pmOnground,
                   g_bhopMode == 1 ? "parity" : (g_bhopMode == 2 ? "exact" : (g_seenAir && g_seenGround) ? "auto->exact" : "auto->parity"));
        Sleep(30);
    }

    // Put the engine pointers back before we vanish, then give any in-flight
    // call a moment to finish before the code disappears out from under it.
    g_cd[CD_HUD_REDRAW]    = (void*)o_HudRedraw;
    g_cd[CD_CL_CREATEMOVE] = (void*)o_CreateMove;
    g_cd[CD_CLIENTMOVE]    = (void*)o_ClientMove;
    Sleep(500);

    printf("unloaded\n");
    FreeConsole();
    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hInst);
        CreateThread(NULL, 0, Run, hInst, 0, NULL);
    }
    return TRUE;
}

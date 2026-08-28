#include <stdio.h>
#include <stddef.h>
#include "mathlib.h"
#include "const.h"
#include "com_model.h"
#include "cl_entity.h"
#include "usercmd.h"
#include "pmtrace.h"

// Copied verbatim from engine/cdll_int.h (that header drags in APIProxy.h,
// which will not compile as plain C).
typedef struct SCREENINFO_s {
    int   iSize; int iWidth; int iHeight; int iFlags; int iCharHeight;
    short charWidths[256];
} SCREENINFO;

typedef struct hud_player_info_s {
    char *name; short ping; byte thisplayer;
    byte spectator; byte packetloss;
    char *model; short topcolor; short bottomcolor;
    unsigned __int64 m_nSteamID;
} hud_player_info_t;

int main(void)
{
    printf("ptr=%u\n", (unsigned)sizeof(void*));
    printf("-- usercmd_t size=%u\n", (unsigned)sizeof(usercmd_t));
    printf("   viewangles = %u\n", (unsigned)offsetof(usercmd_t, viewangles));
    printf("   buttons    = %u\n", (unsigned)offsetof(usercmd_t, buttons));
    printf("   msec       = %u\n", (unsigned)offsetof(usercmd_t, msec));
    printf("   forwardmove= %u\n", (unsigned)offsetof(usercmd_t, forwardmove));
    printf("   sidemove   = %u\n", (unsigned)offsetof(usercmd_t, sidemove));
    printf("   upmove     = %u\n", (unsigned)offsetof(usercmd_t, upmove));
    printf("-- pmtrace_t size=%u\n", (unsigned)sizeof(pmtrace_t));
    printf("   fraction   = %u\n", (unsigned)offsetof(pmtrace_t, fraction));
    printf("   endpos     = %u\n", (unsigned)offsetof(pmtrace_t, endpos));
    printf("   ent        = %u\n", (unsigned)offsetof(pmtrace_t, ent));
    printf("-- SCREENINFO size=%u\n", (unsigned)sizeof(SCREENINFO));
    printf("   iSize      = %u\n", (unsigned)offsetof(SCREENINFO, iSize));
    printf("   iWidth     = %u\n", (unsigned)offsetof(SCREENINFO, iWidth));
    printf("   iHeight    = %u\n", (unsigned)offsetof(SCREENINFO, iHeight));
    printf("-- hud_player_info_t size=%u\n", (unsigned)sizeof(hud_player_info_t));
    printf("   name       = %u\n", (unsigned)offsetof(hud_player_info_t, name));
    printf("   thisplayer = %u\n", (unsigned)offsetof(hud_player_info_t, thisplayer));
    printf("   topcolor   = %u\n", (unsigned)offsetof(hud_player_info_t, topcolor));
    printf("-- entity_state_t (inside cl_entity_t curstate at 688)\n");
    printf("   .team abs  = %u\n", (unsigned)offsetof(cl_entity_t, curstate.team));
    printf("   .usehull   = %u\n", (unsigned)offsetof(cl_entity_t, curstate.usehull));
    printf("   .solid     = %u\n", (unsigned)offsetof(cl_entity_t, curstate.solid));
    printf("   .modelindex= %u\n", (unsigned)offsetof(cl_entity_t, curstate.modelindex));
    printf("   .rendermode= %u\n", (unsigned)offsetof(cl_entity_t, curstate.rendermode));
    printf("   .onground  = %u\n", (unsigned)offsetof(cl_entity_t, curstate.onground));
    printf("   .velocity  = %u\n", (unsigned)offsetof(cl_entity_t, curstate.velocity));
    return 0;
}

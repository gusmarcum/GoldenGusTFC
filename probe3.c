#include <stdio.h>
#include <stddef.h>
#include "mathlib.h"
#include "const.h"
#include "com_model.h"
#include "usercmd.h"
#include "pmtrace.h"
#include "pm_defs.h"

int main(void)
{
    printf("ptr=%u sizeof(playermove_t)=%u\n",
           (unsigned)sizeof(void*), (unsigned)sizeof(playermove_t));
    printf("  onground   = %u\n", (unsigned)offsetof(playermove_t, onground));
    printf("  origin     = %u\n", (unsigned)offsetof(playermove_t, origin));
    printf("  velocity   = %u\n", (unsigned)offsetof(playermove_t, velocity));
    printf("  oldbuttons = %u\n", (unsigned)offsetof(playermove_t, oldbuttons));
    printf("  flags      = %u\n", (unsigned)offsetof(playermove_t, flags));
    printf("  maxspeed   = %u\n", (unsigned)offsetof(playermove_t, maxspeed));
    return 0;
}

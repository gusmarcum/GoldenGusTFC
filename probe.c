#include <stdio.h>
#include <stddef.h>
#include "mathlib.h"
#include "const.h"
#include "com_model.h"
#include "cl_entity.h"

int main(void)
{
    printf("sizeof(void*)          = %u\n", (unsigned)sizeof(void*));
    printf("sizeof(cl_entity_t)    = %u\n", (unsigned)sizeof(cl_entity_t));
    printf("offset index           = %u\n", (unsigned)offsetof(cl_entity_t, index));
    printf("offset player          = %u\n", (unsigned)offsetof(cl_entity_t, player));
    printf("offset curstate        = %u\n", (unsigned)offsetof(cl_entity_t, curstate));
    printf("offset curstate.origin = %u\n", (unsigned)offsetof(cl_entity_t, curstate.origin));
    printf("offset curstate.angles = %u\n", (unsigned)offsetof(cl_entity_t, curstate.angles));
    printf("offset curstate.msgnum = %u\n", (unsigned)offsetof(cl_entity_t, curstate.messagenum));
    printf("offset origin          = %u\n", (unsigned)offsetof(cl_entity_t, origin));
    printf("offset angles          = %u\n", (unsigned)offsetof(cl_entity_t, angles));
    printf("offset model           = %u\n", (unsigned)offsetof(cl_entity_t, model));
    return 0;
}

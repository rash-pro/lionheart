#ifndef FF8_GTE_SPECIALIZED_TEST_H
#define FF8_GTE_SPECIALIZED_TEST_H
#include "gte.h"
/* Independent calls exercise every public entry point against the old decoder. */
static void specialized_command(cpu_t *c, uint32_t opcode) {
    unsigned shift = (opcode & (1u << 19)) ? 12 : 0;
    int lm = (opcode & (1u << 10)) != 0;
    switch (opcode & 63) {
    case 0x01: gte_rtps(c, shift, lm); break;
    case 0x06: gte_nclip(c); break;
    case 0x0c: gte_op(c, shift, lm); break;
    case 0x10: gte_dpcs(c, shift, lm); break;
    case 0x11: gte_intpl(c, shift, lm); break;
    case 0x12: gte_mvmva(c, shift, lm, (opcode >> 17) & 3, (opcode >> 15) & 3, (opcode >> 13) & 3); break;
    case 0x13: gte_ncds(c, shift, lm); break;
    case 0x14: gte_cdp(c, shift, lm); break;
    case 0x16: gte_ncdt(c, shift, lm); break;
    case 0x1b: gte_nccs(c, shift, lm); break;
    case 0x1c: gte_cc(c, shift, lm); break;
    case 0x1e: gte_ncs(c, shift, lm); break;
    case 0x20: gte_nct(c, shift, lm); break;
    case 0x28: gte_sqr(c, shift, lm); break;
    case 0x29: gte_dcpl(c, shift, lm); break;
    case 0x2a: gte_dpct(c, shift, lm); break;
    case 0x2d: gte_avsz3(c); break;
    case 0x2e: gte_avsz4(c); break;
    case 0x30: gte_rtpt(c, shift, lm); break;
    case 0x3d: gte_gpf(c, shift, lm); break;
    case 0x3e: gte_gpl(c, shift, lm); break;
    case 0x3f: gte_ncct(c, shift, lm); break;
    default: gte_cmd(c, opcode); break;
    }
}
#endif

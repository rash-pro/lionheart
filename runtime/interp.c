#include "cpu.h"

#if defined(FF8_FRAME_PROVENANCE)
/* Specialize once at entry: OFF executes the ordinary instruction body with
 * no shadow locals or per-operation checks. Keep one semantic implementation. */
#if defined(__GNUC__)
static inline __attribute__((always_inline))
#else
static
#endif
void interp_execute(cpu_t *c, int tagged) {
#else
void interp_step(cpu_t *c) {
#endif
    uint32_t pc = c->pc, word, a, b, value = 0, loaded = 0, address, next;
    uint32_t old_target = c->branch_target, old_return = c->branch_return;
    unsigned op, rs, rt, rd, sh, fn, dst = 0, load = 0;
    unsigned delayed = c->branch_pending, old_kind = c->branch_kind;
    int32_t imm;
    int branch = 0, take = 0, kind = 0;
    uint32_t target = 0, back = 0;
    uint64_t product;
#if defined(FF8_FRAME_PROVENANCE)
    frame_tag_t atag = 0, btag = 0, vtag = 0, ltag = 0, vloc = 0, lloc = 0;
    frame_register_tags_t *tags = tagged ? frame_tags_registers(c) : NULL;
#define TAG(code) do { if (tagged) { code; } } while (0)
#else
#define TAG(code) ((void)0)
#endif
    if (c->halted) return;
    c->in_delay_slot = delayed;
    c->flow = 0;
    if (pc & 3) { cpu_exception(c, 4, pc); return; }
    if ((pc & 0x1fffffffu) >= 0x800000u) {
        cpu_stop(c, "instruction fetch outside RAM at %08x", pc); return;
    }
    word = mem_read32(c, pc);
    if (c->interp_profile) ++c->interp_profile[(pc & (RAM_SIZE - 1)) / 4];
    ++c->interp_instructions;
    c_pc_count(c, pc, 1);
    if (c->overlay_profile) c_profile_count(c, pc, 1, 0);
    c->fault_instructions = 1; c->fault_counted = 1;
    op = word >> 26; rs = (word >> 21) & 31; rt = (word >> 16) & 31;
    rd = (word >> 11) & 31; sh = (word >> 6) & 31; fn = word & 63;
    imm = (int16_t)(word & 65535);
    a = c->r[rs]; b = c->r[rt];
    TAG(atag = tags->r[rs]; btag = tags->r[rt]);
    next = pc + 4;
    address = a + (uint32_t)imm;
    if (op == 0) {
        dst = rd;
        switch (fn) {
        case 0: value = b << sh; TAG(vtag = btag; if (value == b) vloc = tags->location[rt]); break;
        case 2: value = b >> sh; TAG(vtag = btag; if (value == b) vloc = tags->location[rt]); break;
        case 3: value = c_sra(b, sh); TAG(vtag = btag; if (value == b) vloc = tags->location[rt]); break;
        case 4: value = b << (a & 31); TAG(vtag = btag; if (value == b) vloc = tags->location[rt]); break;
        case 6: value = b >> (a & 31); TAG(vtag = btag; if (value == b) vloc = tags->location[rt]); break;
        case 7: value = c_sra(b, a); TAG(vtag = btag; if (value == b) vloc = tags->location[rt]); break;
        case 8: dst = 0; branch = take = 1; target = a; kind = rs == 31 ? 2 : 0; break;
        case 9: value = pc + 8; branch = take = 1; target = a; kind = rd ? 1 : 0; back = pc + 8; break;
        case 12:
            dst = 0;
            TAG(frame_tags_location_retire(c, 0, 0, 0, 0); frame_tags_retire(c, 0, 0, 0, 0));
            c_retire(c, 0, 0, 0, 0);
            bios_syscall(c);
            goto retired;
        case 13: dst = 0; cpu_exception(c, 9, 0); break;
        case 16: value = c->hi; TAG(vtag = tags->hi; vloc = tags->hi_location); break;
        case 17: dst = 0; c->hi = a; TAG(tags->hi = atag; tags->hi_location = tags->location[rs]); break;
        case 18: value = c->lo; TAG(vtag = tags->lo; vloc = tags->lo_location); break;
        case 19: dst = 0; c->lo = a; TAG(tags->lo = atag; tags->lo_location = tags->location[rs]); break;
        case 24:
        case 25:
            dst = 0;
            product = fn == 24 ? (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b) : (uint64_t)a * b;
            c->lo = (uint32_t)product; c->hi = (uint32_t)(product >> 32); TAG(tags->hi = tags->lo = frame_tags_merge(c, atag, btag); tags->hi_location = tags->lo_location = 0); break;
        case 26:
        case 27:
            dst = 0;
            TAG(tags->hi = tags->lo = frame_tags_merge(c, atag, btag); tags->hi_location = tags->lo_location = 0);
            if (b == 0) { c->lo = fn == 26 && (a & 0x80000000u) ? 1 : 0xffffffffu; c->hi = a; }
            else if (fn == 26) {
                if (a == 0x80000000u && b == 0xffffffffu) { c->lo = a; c->hi = 0; }
                else {
                    c->lo = (uint32_t)((int32_t)a / (int32_t)b);
                    c->hi = (uint32_t)((int32_t)a % (int32_t)b);
                }
            } else { c->lo = a / b; c->hi = a % b; }
            break;
        case 32: value = a + b; if ((~(a ^ b) & (a ^ value)) & 0x80000000u) cpu_exception(c, 12, 0); TAG(vtag = frame_tags_merge(c, atag, btag); if (!a && value == b) vloc = tags->location[rt]; else if (!b && value == a) vloc = tags->location[rs]); break;
        case 33: value = a + b; TAG(vtag = frame_tags_merge(c, atag, btag); if (!a && value == b) vloc = tags->location[rt]; else if (!b && value == a) vloc = tags->location[rs]); break;
        case 34: value = a - b; if (((a ^ b) & (a ^ value)) & 0x80000000u) cpu_exception(c, 12, 0); TAG(vtag = rs == rt ? 0 : frame_tags_merge(c, atag, btag)); break;
        case 35: value = a - b; TAG(vtag = rs == rt ? 0 : frame_tags_merge(c, atag, btag)); break;
        case 36: value = a & b; TAG(vtag = frame_tags_merge(c, atag, btag); if (!a && value == b) vloc = tags->location[rt]; else if (!b && value == a) vloc = tags->location[rs]); break;
        case 37: value = a | b; TAG(vtag = frame_tags_merge(c, atag, btag); if (!a && value == b) vloc = tags->location[rt]; else if (!b && value == a) vloc = tags->location[rs]); break;
        case 38: value = a ^ b; TAG(vtag = rs == rt ? 0 : frame_tags_merge(c, atag, btag)); break;
        case 39: value = ~(a | b); TAG(vtag = frame_tags_merge(c, atag, btag); if (!a && value == b) vloc = tags->location[rt]; else if (!b && value == a) vloc = tags->location[rs]); break;
        case 42: value = (int32_t)a < (int32_t)b; break;
        case 43: value = a < b; break;
        default: dst = 0; cpu_exception(c, 10, 0); break;
        }
    } else switch (op) {
    case 1:
        branch = 1;
        take = rt & 1 ? (int32_t)a >= 0 : (int32_t)a < 0;
        target = pc + 4 + (uint32_t)imm * 4;
        if (rt == 16 || rt == 17) { dst = 31; value = pc + 8; kind = 1; back = pc + 8; }
        break;
    case 2:
    case 3:
        branch = take = 1; target = ((pc + 4) & 0xf0000000u) | ((word & 0x03ffffffu) << 2);
        if (op == 3) { dst = 31; value = pc + 8; kind = 1; back = pc + 8; }
        break;
    case 4: branch = 1; take = a == b; break;
    case 5: branch = 1; take = a != b; break;
    case 6: branch = 1; take = (int32_t)a <= 0; break;
    case 7: branch = 1; take = (int32_t)a > 0; break;
    case 8:
        dst = rt; value = a + (uint32_t)imm; TAG(vtag = atag; if (value == a) vloc = tags->location[rs]);
        if ((~(a ^ (uint32_t)imm) & (a ^ value)) & 0x80000000u) cpu_exception(c, 12, 0);
        break;
    case 9: dst = rt; value = address; TAG(vtag = atag; if (value == a) vloc = tags->location[rs]); break;
    case 10: dst = rt; value = (int32_t)a < imm; break;
    case 11: dst = rt; value = a < (uint32_t)imm; break;
    case 12: dst = rt; value = a & (word & 65535); TAG(vtag = atag; if (value == a) vloc = tags->location[rs]); break;
    case 13: dst = rt; value = a | (word & 65535); TAG(vtag = atag; if (value == a) vloc = tags->location[rs]); break;
    case 14: dst = rt; value = a ^ (word & 65535); TAG(vtag = atag; if (value == a) vloc = tags->location[rs]); break;
    case 15: dst = rt; value = (word & 65535) << 16; break;
    case 16:
    case 18:
        if (rs == 0 || rs == 2) {
            load = rt;
            loaded = op == 16 ? c->cop0[rd] : gte_read(c, rd, rs == 2);
            TAG(ltag = op == 18 ? frame_tags_gte_read(c, rd, rs == 2) : 0; if (op == 18 && rs == 2) lloc = frame_tags_control_source(c, rd));
        } else if (rs == 4 || rs == 6) {
            if (op == 16) { if (rd == 12) c_set_sr(c, b); else c->cop0[rd] = b; }
            else { gte_write(c, rd, b, rs == 6); TAG(frame_tags_gte_write(c, rd, rs == 6, btag); if (rs == 6) frame_tags_control_location(c, rd, tags->location[rt])); }
        } else if (rs == 8) {
            branch = 1; take = !(rt & 1); target = pc + 4 + (uint32_t)imm * 4;
        } else if (rs >= 16 && op == 18) { TAG(frame_tags_gte_command(c, word)); gte_cmd(c, word); }
        else if (rs >= 16 && fn == 16) c_set_sr(c, (c->cop0[12] & ~15u) | ((c->cop0[12] >> 2) & 15));
        else cpu_exception(c, 10, 0);
        break;
    case 32: load = rt; loaded = (uint32_t)(int32_t)(int8_t)mem_read8(c, address); TAG(ltag = frame_tags_load(c, address, 1); lloc = frame_tags_location(c, address)); break;
    case 33: load = rt; loaded = (uint32_t)(int32_t)(int16_t)mem_read16(c, address); TAG(ltag = frame_tags_load(c, address, 2); lloc = frame_tags_location(c, address)); break;
    case 34:
    case 38: {
        unsigned n = address & 3, j;
        uint32_t aligned = mem_read32(c, address & ~3u);
        load = rt; loaded = c_merge_value(c, rt);
        TAG(ltag = c->load_reg == rt && rt ? tags->load : btag;
            if ((op == 34 && n == 3) || (op == 38 && n == 0)) ltag = 0;
            ltag = frame_tags_merge(c, ltag, frame_tags_load(c, address & ~3u, 4)));
        if (op == 34) {
            for (j = 0; j <= n; ++j) {
                unsigned shift = (3 - n + j) * 8;
                loaded = (loaded & ~(255u << shift)) | (((aligned >> (j * 8)) & 255) << shift);
            }
        } else for (j = n; j < 4; ++j) {
            unsigned shift = (j - n) * 8;
            loaded = (loaded & ~(255u << shift)) | (((aligned >> (j * 8)) & 255) << shift);
        }
        break;
    }
    case 35: load = rt; loaded = mem_read32(c, address); TAG(ltag = frame_tags_load(c, address, 4); lloc = frame_tags_location(c, address)); break;
    case 36: load = rt; loaded = mem_read8(c, address); TAG(ltag = frame_tags_load(c, address, 1); lloc = frame_tags_location(c, address)); break;
    case 37: load = rt; loaded = mem_read16(c, address); TAG(ltag = frame_tags_load(c, address, 2); lloc = frame_tags_location(c, address)); break;
    case 40: TAG(frame_tags_port(c, btag)); mem_write8(c, address, b); TAG(frame_tags_store(c, address, 1, btag)); break;
    case 41: TAG(frame_tags_port(c, btag)); mem_write16(c, address, b); TAG(frame_tags_store(c, address, 2, btag)); break;
    case 42: TAG(frame_tags_port(c, btag)); c_swl(c, address, b); TAG(frame_tags_store(c, address & ~3u, (address & 3) + 1, btag)); break;
    case 43: TAG(frame_tags_port(c, btag)); mem_write32(c, address, b); TAG(frame_tags_store(c, address, 4, btag); frame_tags_location_store(c, address, 4, tags->location[rt])); break;
    case 46: TAG(frame_tags_port(c, btag)); c_swr(c, address, b); TAG(frame_tags_store(c, address, 4 - (address & 3), btag)); break;
    case 50: gte_write(c, rt, mem_read32(c, address), 0); TAG(frame_tags_gte_write(c, rt, 0, frame_tags_load(c, address, 4))); break;
    case 58: TAG(btag = frame_tags_gte_read(c, rt, 0); frame_tags_port(c, btag)); mem_write32(c, address, gte_read(c, rt, 0)); TAG(frame_tags_store(c, address, 4, btag)); break;
    case 17: case 19: case 49: case 51: case 57: case 59: cpu_exception(c, 11, 0); break;
    default: cpu_exception(c, 10, 0); break;
    }
    if (branch && delayed) cpu_stop(c, "branch in branch delay slot (undefined R3000 behavior)");
    FRAME_ORACLE(frame_oracle_instruction(c, word, a, b));
    TAG(frame_tags_location_retire(c, dst, vloc, load, lloc); frame_tags_retire(c, dst, vtag, load, ltag));
    c_retire(c, dst, value, load, loaded);
retired:
    if (c->halted) return;
    if (op >= 4 && op <= 7) target = pc + 4 + (uint32_t)imm * 4;
    if (branch && take) c_transfer_stats(c, pc, target, kind == 1 ? C_DISPATCH_CALL :
        kind == 2 ? C_DISPATCH_RETURN : op == 0 ? C_DISPATCH_TABLE : C_DISPATCH_COUNT);
    c->branch_pending = (unsigned)branch;
    if (branch) {
        c->branch_target = take ? target : pc + 8;
        c->branch_pc = pc;
        c->branch_kind = take ? (unsigned)kind : 0;
        c->branch_return = back;
    }
    if (delayed) {
        next = old_target;
        c->flow = 1; c->flow_kind = old_kind; c->flow_return = old_return;
    }
    c->pc = next;
    c->in_delay_slot = 0;
    c->fault_instructions = 0;
    c_tick(c, 1);
#undef TAG

}

#if defined(FF8_FRAME_PROVENANCE)
void interp_step(cpu_t *c) {
    if (c->frame_tags) interp_execute(c, 1);
    else interp_execute(c, 0);
}
#endif

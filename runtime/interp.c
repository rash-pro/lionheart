#include "cpu.h"
#include "interp.h"
#include "frame_oracle.h"

void (*interp_gte_command)(cpu_t *c, uint32_t opcode) = gte_cmd;

void interp_step(cpu_t *c) {
    uint32_t pc = c->pc, word, a, b, value = 0, loaded = 0, address, next;
    uint32_t old_target = c->branch_target, old_return = c->branch_return;
    unsigned op, rs, rt, rd, sh, fn, dst = 0, load = 0;
    unsigned delayed = c->branch_pending, old_kind = c->branch_kind;
    int32_t imm;
    int branch = 0, take = 0, kind = 0;
    uint32_t target = 0, back = 0;
    uint64_t product;
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
    next = pc + 4;
    address = a + (uint32_t)imm;
    if (op == 0) {
        dst = rd;
        switch (fn) {
        case 0: value = b << sh; break;
        case 2: value = b >> sh; break;
        case 3: value = c_sra(b, sh); break;
        case 4: value = b << (a & 31); break;
        case 6: value = b >> (a & 31); break;
        case 7: value = c_sra(b, a); break;
        case 8: dst = 0; branch = take = 1; target = a; kind = rs == 31 ? 2 : 0; break;
        case 9: value = pc + 8; branch = take = 1; target = a; kind = rd ? 1 : 0; back = pc + 8; break;
        case 12:
            dst = 0;
            c_retire(c, 0, 0, 0, 0);
            bios_syscall(c);
            goto retired;
        case 13: dst = 0; cpu_exception(c, 9, 0); break;
        case 16: value = c->hi; break;
        case 17: dst = 0; c->hi = a; break;
        case 18: value = c->lo; break;
        case 19: dst = 0; c->lo = a; break;
        case 24:
        case 25:
            dst = 0;
            product = fn == 24 ? (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b) : (uint64_t)a * b;
            c->lo = (uint32_t)product; c->hi = (uint32_t)(product >> 32); break;
        case 26:
        case 27:
            dst = 0;
            if (b == 0) { c->lo = fn == 26 && (a & 0x80000000u) ? 1 : 0xffffffffu; c->hi = a; }
            else if (fn == 26) {
                if (a == 0x80000000u && b == 0xffffffffu) { c->lo = a; c->hi = 0; }
                else {
                    c->lo = (uint32_t)((int32_t)a / (int32_t)b);
                    c->hi = (uint32_t)((int32_t)a % (int32_t)b);
                }
            } else { c->lo = a / b; c->hi = a % b; }
            break;
        case 32: value = a + b; if ((~(a ^ b) & (a ^ value)) & 0x80000000u) cpu_exception(c, 12, 0); break;
        case 33: value = a + b; break;
        case 34: value = a - b; if (((a ^ b) & (a ^ value)) & 0x80000000u) cpu_exception(c, 12, 0); break;
        case 35: value = a - b; break;
        case 36: value = a & b; break;
        case 37: value = a | b; break;
        case 38: value = a ^ b; break;
        case 39: value = ~(a | b); break;
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
        dst = rt; value = a + (uint32_t)imm;
        if ((~(a ^ (uint32_t)imm) & (a ^ value)) & 0x80000000u) cpu_exception(c, 12, 0);
        break;
    case 9: dst = rt; value = address; break;
    case 10: dst = rt; value = (int32_t)a < imm; break;
    case 11: dst = rt; value = a < (uint32_t)imm; break;
    case 12: dst = rt; value = a & (word & 65535); break;
    case 13: dst = rt; value = a | (word & 65535); break;
    case 14: dst = rt; value = a ^ (word & 65535); break;
    case 15: dst = rt; value = (word & 65535) << 16; break;
    case 16:
    case 18:
        if (rs == 0 || rs == 2) {
            load = rt;
            loaded = op == 16 ? c->cop0[rd] : gte_read(c, rd, rs == 2);
        } else if (rs == 4 || rs == 6) {
            if (op == 16) { if (rd == 12) c_set_sr(c, b); else c->cop0[rd] = b; }
            else gte_write(c, rd, b, rs == 6);
        } else if (rs == 8) {
            branch = 1; take = !(rt & 1); target = pc + 4 + (uint32_t)imm * 4;
        } else if (rs >= 16 && op == 18) interp_gte_command(c, word);
        else if (rs >= 16 && fn == 16) c_set_sr(c, (c->cop0[12] & ~15u) | ((c->cop0[12] >> 2) & 15));
        else cpu_exception(c, 10, 0);
        break;
    case 32: load = rt; loaded = (uint32_t)(int32_t)(int8_t)mem_read8(c, address); break;
    case 33: load = rt; loaded = (uint32_t)(int32_t)(int16_t)mem_read16(c, address); break;
    case 34:
    case 38: {
        unsigned n = address & 3, j;
        uint32_t aligned = mem_read32(c, address & ~3u);
        load = rt; loaded = c_merge_value(c, rt);
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
    case 35: load = rt; loaded = mem_read32(c, address); break;
    case 36: load = rt; loaded = mem_read8(c, address); break;
    case 37: load = rt; loaded = mem_read16(c, address); break;
    case 40: mem_write8(c, address, b); break;
    case 41: mem_write16(c, address, b); break;
    case 42: c_swl(c, address, b); break;
    case 43: mem_write32(c, address, b); break;
    case 46: c_swr(c, address, b); break;
    case 50: gte_write(c, rt, mem_read32(c, address), 0); break;
    case 58: mem_write32(c, address, gte_read(c, rt, 0)); break;
    case 17: case 19: case 49: case 51: case 57: case 59: cpu_exception(c, 11, 0); break;
    default: cpu_exception(c, 10, 0); break;
    }
    if (branch && delayed) cpu_stop(c, "branch in branch delay slot (undefined R3000 behavior)");
    FRAME_ORACLE(frame_oracle_step(c, word, a, b, address, dst, value, load, loaded));
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

}

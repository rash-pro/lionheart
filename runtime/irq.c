#include "hw.h"
#include "sio.h"
#include <string.h>

#define IRQ_RETURN 0xfffffff0u

uint32_t bios_guest_call(cpu_t *c, uint32_t address) {
    uint32_t pc = c->pc, ra = c->r[31], value;
    uint32_t load_reg = c->load_reg, load_value = c->load_value;
    unsigned flow = c->flow, kind = c->flow_kind;
    uint32_t back = c->flow_return;
    if (!address) return 0;
    c->load_reg = c->load_value = 0; c->flow = 0;
    c->r[31] = IRQ_RETURN;
    if (c_enter(c, address, IRQ_RETURN)) { dispatch(c, address); c_leave(c); }
    value = c->r[2];
    if (!c->halted) {
        c->pc = pc; c->r[31] = ra; c->load_reg = load_reg; c->load_value = load_value;
        c->flow = flow; c->flow_kind = kind; c->flow_return = back;
    }
    return value;
}

void bios_deliver(cpu_t *c, uint32_t class_id, uint32_t spec) {
    unsigned i;
    struct hw *h = hw_get(c);
    for (i = 0; i < BIOS_EVENTS; ++i) {
        bios_event_t *e = &h->events[i];
        if (e->state != 2 || e->class_id != class_id || e->spec != spec) continue;
        if (e->mode == 0x2000) e->state = 3;
        else if (e->mode == 0x1000 && e->handler) bios_guest_call(c, e->handler);
    }
}

void bios_pad_vblank(cpu_t *c) {
    struct hw *h = c->hw;
    unsigned port;
    if (!h->pad_started) return;
    for (port = 0; port < 2; ++port) if (h->pad_buffer[port] && h->pad_size[port] >= 4) {
        uint16_t buttons = 0xffff;
        int connected = sio_pad_snapshot(c, port, &buttons);
        mem_write8(c, h->pad_buffer[port], connected ? 0 : 0xff);
        mem_write8(c, h->pad_buffer[port] + 1, 0x41);
        mem_write16(c, h->pad_buffer[port] + 2, buttons);
    }
}

void bios_exception(cpu_t *c, uint32_t cause) {
    struct hw *h = hw_get(c);
    struct {
        uint32_t r[32], hi, lo, pc, cop0[32];
        uint32_t load_reg, load_value, branch_target, branch_pc, branch_return, flow_return;
        unsigned branch_pending, branch_kind, in_delay_slot, flow, flow_kind, depth, dispatch_depth;
        unsigned fault_counted, native_pin_count; uint32_t fault_instructions;
    } saved;
    unsigned i;
    volatile uint32_t old_tcb = mem_read32(c, mem_read32(c, 0x108));
    if (h->in_exception) return;
    bios_thread_pipeline_save(c, old_tcb, c->pc);
    memcpy(saved.r, c->r, sizeof(saved.r)); memcpy(saved.cop0, c->cop0, sizeof(saved.cop0));
    saved.hi = c->hi; saved.lo = c->lo; saved.pc = c->pc;
#define SAVE(f) saved.f = c->f
    SAVE(load_reg); SAVE(load_value); SAVE(branch_target); SAVE(branch_pc); SAVE(branch_return);
    SAVE(flow_return); SAVE(branch_pending); SAVE(branch_kind); SAVE(in_delay_slot); SAVE(flow); SAVE(flow_kind);
    SAVE(depth); SAVE(dispatch_depth); SAVE(fault_counted); SAVE(fault_instructions);
    SAVE(native_pin_count);
#undef SAVE
    h->in_exception = 1;
    for (i = 0; i < 11; ++i) if (h->i_stat & h->i_mask & (1u << i)) ++h->irq_delivered[i];
    c->cop0[13] = cause; c->cop0[14] = c->pc;
    c_set_sr(c, (c->cop0[12] & ~63u) | ((c->cop0[12] << 2) & 63));
    bios_thread_save(c, old_tcb, c->pc, c->cop0[12]);
    c->load_reg = c->load_value = c->branch_pending = c->in_delay_slot = c->flow = 0;
    c->fault_counted = c->fault_instructions = 0;
    c->r[29] = 0x8000fff0u;
    if (!setjmp(h->exception_return)) {
        unsigned priority, poll_pad;
        bios_card_deliver(c);
        for (priority = 0; priority < 4; ++priority) {
            uint32_t node = mem_read32(c, mem_read32(c, 0x100) + priority * 8);
            unsigned left = 256;
            while (node && left--) {
                uint32_t next = mem_read32(c, node), first = mem_read32(c, node + 8), second = mem_read32(c, node + 4);
                if (first && bios_guest_call(c, first) && second) bios_guest_call(c, second);
                node = next;
            }
            if (node) { cpu_stop(c, "cyclic BIOS IRQ priority chain %u", priority); break; }
        }
        poll_pad = h->pad_started && (h->i_stat & h->i_mask & 1);
        for (i = 0; i < 4; ++i) {
            unsigned source = i == 3 ? 0 : i + 4;
            if (!(h->i_stat & h->i_mask & (1u << source))) continue;
            bios_deliver(c, 0xf2000000u + i, 2);
            if (h->rcnt_ack[i]) h->i_stat &= ~(1u << source);
        }
        if (poll_pad) {
            bios_pad_vblank(c);
            if (c->pad_clear) h->i_stat &= ~1u;
        }
        if (h->bios_cd && (h->i_stat & h->i_mask & 4)) {
            static const uint32_t specs[] = {0, 0x40, 0x20, 0x10, 0x80, 0x8000};
            if (h->cd.irq_flags <= 5) bios_deliver(c, 0xf0000003, specs[h->cd.irq_flags]);
            h->cd.irq_flags = 0; h->i_stat &= ~4u;
        }
        for (i = 0; i < 11; ++i) if (h->i_stat & h->i_mask & (1u << i)) {
            bios_deliver(c, 0xf0000001u + i, 0x1000);
            if (h->auto_ack & (1u << i)) h->i_stat &= ~(1u << i);
        }
        if (h->exit_hook) {
            static const unsigned regs[] = {31, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23, 28};
            uint32_t target = mem_read32(c, h->exit_hook);
            for (i = 0; i < 12; ++i) c->r[regs[i]] = mem_read32(c, h->exit_hook + 4 * i);
            c->r[2] = 1;
            bios_guest_call(c, target);
        }
    }
    /* ReturnFromException can bypass callback dispatch frames with longjmp. */
    c_release_overlays(c, saved.native_pin_count);
    for (i = saved.dispatch_depth + 1; i <= c->dispatch_depth; ++i) c->trampolines[i].active = 0;
    h->in_exception = 0;
    if (c->halted) return;
    memcpy(c->r, saved.r, sizeof(saved.r)); memcpy(c->cop0, saved.cop0, sizeof(saved.cop0));
    c->hi = saved.hi; c->lo = saved.lo; c->pc = saved.pc;
    c_set_sr(c, saved.cop0[12]);
#define RESTORE(f) c->f = saved.f
    RESTORE(load_reg); RESTORE(load_value); RESTORE(branch_target); RESTORE(branch_pc); RESTORE(branch_return);
    RESTORE(flow_return); RESTORE(branch_pending); RESTORE(branch_kind); RESTORE(in_delay_slot); RESTORE(flow); RESTORE(flow_kind);
    RESTORE(depth); RESTORE(dispatch_depth); RESTORE(fault_counted); RESTORE(fault_instructions);
#undef RESTORE
    {
        uint32_t next_tcb = mem_read32(c, mem_read32(c, 0x108));
        bios_thread_restore(c, next_tcb);
        if (next_tcb != old_tcb || c->pc != saved.pc) {
            cpu_log(c, "THREAD IRQ old=%08x next=%08x resume=%08x load=%u:%08x branch=%u target=%08x",
                    (uint32_t)old_tcb, next_tcb, saved.pc, saved.load_reg, saved.load_value,
                    saved.branch_pending, saved.branch_target);
            bios_thread_pipeline_restore(c, next_tcb);
            hw_reschedule(c);
            c_unwind(c);
        }
    }
}

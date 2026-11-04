#include "hw.h"

static int running(struct hw *h, unsigned i) {
    hw_timer_t *t = &h->timers[i];
    unsigned sync = (t->mode >> 1) & 3, blank = i ? h->vblank : h->hblank;
    if (!(t->mode & 1)) return 1;
    if (i == 2) return sync == 1 || sync == 2;
    return sync == 0 ? !blank : sync == 1 ? 1 : sync == 2 ? blank : t->released;
}

static void rate(struct hw *h, unsigned i, uint64_t *num, uint64_t *den) {
    static const unsigned divs[] = {10, 8, 5, 4};
    hw_timer_t *t = &h->timers[i];
    *num = 1; *den = 1;
    if (i == 0 && (t->mode & 0x100)) {
        uint32_t status = gpu_status(h->gpu);
        *num = 53693175; *den = (uint64_t)HW_CPU_HZ * ((status & (1u << 16)) ? 7 : divs[(status >> 17) & 3]);
    } else if (i == 1 && (t->mode & 0x100)) *num = 0;
    else if (i == 2 && (t->mode & 0x200)) *den = 8;
}

static void interrupt(cpu_t *c, unsigned i) {
    hw_timer_t *t = &c->hw->timers[i];
    if (t->fired && !(t->mode & 0x40)) return;
    t->fired = 1;
    if (t->mode & 0x80) t->mode ^= 0x400;
    else t->mode &= ~0x400;
    if (!(t->mode & 0x400)) hw_irq_raise(c, i + 4);
    if (!(t->mode & 0x80)) t->mode |= 0x400;
}

static void count(cpu_t *c, unsigned i, uint64_t ticks) {
    hw_timer_t *t = &c->hw->timers[i];
    if (ticks && !(t->mode & 0x38)) {
        uint32_t target = (uint16_t)(t->target - t->count), overflow = 0xffffu - t->count;
        if (ticks >= (target ? target : 0x10000u)) t->mode |= 0x800;
        if (ticks >= (overflow ? overflow : 0x10000u)) t->mode |= 0x1000;
        t->count = (uint16_t)(t->count + ticks);
        return;
    }
    while (ticks) {
        uint32_t target = (uint16_t)(t->target - t->count), overflow = 0xffffu - t->count;
        uint32_t step;
        if (!target) target = 0x10000;
        if (!overflow) overflow = 0x10000;
        step = target < overflow ? target : overflow;
        if (ticks < step) { t->count = (uint16_t)(t->count + ticks); break; }
        t->count = (uint16_t)(t->count + step); ticks -= step;
        c->hw->timers_dirty = 1;
        if (step == target) {
            t->mode |= 0x800;
            if (t->mode & 0x10) interrupt(c, i);
        }
        if (step == overflow) {
            t->mode |= 0x1000;
            if (t->mode & 0x20) interrupt(c, i);
        }
        if (step == target && (t->mode & 8)) t->count = 0;
    }
}

void timers_advance(cpu_t *c, uint64_t cycles) {
    unsigned i;
    for (i = 0; i < 3; ++i) {
        uint64_t num, den, total;
        hw_timer_t *t = &c->hw->timers[i];
        cycles = c->hw->now - t->cycle;
        t->cycle = c->hw->now;
        if (!cycles || !running(c->hw, i)) continue;
        if (i == 0 && (t->mode & 0x100)) {
            /* Only the GPU dot clock is rational. Keep its full-width path. */
            rate(c->hw, i, &num, &den);
            total = t->fraction + cycles * num;
            t->fraction = total % den;
            count(c, i, total / den);
        } else {
            /* Timer 1's HBlank ticks arrive through timers_blank(). The other
             * clocks are the CPU clock or timer 2's power-of-two divider.
             * Avoid rate() and a variable division on every materialization. */
            total = t->fraction + (i == 1 && (t->mode & 0x100) ? 0 : cycles);
            if (i == 2 && (t->mode & 0x200)) {
                t->fraction = total & 7u;
                count(c, i, total >> 3);
            } else {
                t->fraction = 0;
                count(c, i, total);
            }
        }
    }
}

void timers_blank(cpu_t *c, unsigned i, unsigned start) {
    hw_timer_t *t = &c->hw->timers[i];
    unsigned mode = (t->mode >> 1) & 3;
    c->hw->timers_dirty = 1;
    if (start && (t->mode & 1)) {
        if (mode == 1 || mode == 2) { t->count = 0; t->fraction = 0; }
        if (mode == 3) t->released = 1;
    }
    if (i == 0 && start && (c->hw->timers[1].mode & 0x100) && running(c->hw, 1)) count(c, 1, 1);
}

uint64_t timers_next(cpu_t *c) {
    uint64_t next = HW_NEVER;
    unsigned i;
    for (i = 0; i < 3; ++i) {
        hw_timer_t *t = &c->hw->timers[i];
        uint64_t num, den, ticks, delay;
        uint32_t target = (uint16_t)(t->target - t->count), overflow = 0xffffu - t->count;
        if (!(t->mode & 0x30) || (t->fired && !(t->mode & 0x40)) || !running(c->hw, i)) continue;
        rate(c->hw, i, &num, &den);
        if (!num) continue;
        if (!target) target = 0x10000;
        if (!overflow) overflow = 0x10000;
        ticks = target < overflow ? target : overflow;
        delay = num == 1 ? (uint32_t)ticks * (uint32_t)den - (uint32_t)t->fraction :
            (ticks * den - t->fraction + num - 1) / num;
        if (t->cycle + delay < next) next = t->cycle + delay;
    }
    return next;
}

int timers_observe_hblank(cpu_t *c) {
    struct hw *h = c->hw;
    return ((h->timers[0].mode & 1) && !h->timers[0].released) ||
        ((h->timers[1].mode & 0x100) && running(h, 1));
}

uint32_t timers_read(cpu_t *c, uint32_t address) {
    timers_advance(c, 0);
    hw_timer_t *t = &c->hw->timers[(address >> 4) & 3];
    if ((address & 12) == 4) { unsigned v = t->mode; t->mode &= ~0x1800; return v; }
    return (address & 12) == 8 ? t->target : t->count;
}

void timers_write(cpu_t *c, uint32_t address, uint32_t value) {
    timers_advance(c, 0);
    hw_timer_t *t = &c->hw->timers[(address >> 4) & 3];
    c->hw->timers_dirty = 1;
    if ((address & 12) == 4) {
        t->mode = (value & 0x3ff) | 0x400; t->count = 0; t->fraction = 0; t->fired = t->released = 0;
    } else if ((address & 12) == 8) t->target = value;
    else { t->count = value; t->fraction = 0; }
}

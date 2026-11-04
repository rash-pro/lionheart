#include "hw.h"
#include "cycle_math.h"
#include <stdlib.h>

#if defined(__GNUC__) && !defined(FF8_STATS)
__attribute__((noinline, cold))
#endif
static void record_access(cpu_t *c, uint32_t address, unsigned width, uint32_t value, int write) {
#ifdef FF8_STATS
    unsigned i;
    mmio_stat_t *s;
    unsigned slot = ((address >> 1) ^ (width << 7)) & 1023;
    i = c->hw->mmio_hint[slot];
    if (i && c->mmio_stats[i - 1].address == address && c->mmio_stats[i - 1].width == width) --i;
    else for (i = 0; i < c->mmio_count; ++i)
        if (c->mmio_stats[i].address == address && c->mmio_stats[i].width == width) break;
    if (i == c->mmio_count) {
        if (i == MMIO_STATS) { cpu_stop(c, "MMIO statistics capacity exceeded"); return; }
        ++c->mmio_count;
        c->mmio_stats[i].address = address;
        c->mmio_stats[i].width = width;
    }
    c->hw->mmio_hint[slot] = (uint16_t)(i + 1);
    s = &c->mmio_stats[i];
    /* Read and write values can legitimately differ for the same port.
     * Alternating identical commands/status reads is not hardware progress. */
    if (write ? (!s->writes || s->last_write != value) : (!s->reads || s->last_read != value))
        ++c->mmio_epoch;
    s->last = value;
    if (write) { ++s->writes; s->last_write = value; }
    else { ++s->reads; s->last_read = value; }
#else
    /* The optional stall detector needs value changes, never access counts.
     * Keep separate read/write observations so command/status alternation is
     * not mistaken for guest progress. Disabled diagnostics cost one branch. */
    if (c->stall_limit) {
        unsigned slot = ((address >> 1) ^ (width << 7)) & 1023, probes = 0;
        uint32_t key = (address & 65535) | (width << 16), bit = write ? 2 : 1;
        while (c->hw->mmio_progress[slot].key && c->hw->mmio_progress[slot].key != key) {
            slot = (slot + 1) & 1023;
            if (++probes == 1024) { ++c->mmio_epoch; return; }
        }
        if (!(c->hw->mmio_progress[slot].seen & bit) ||
            (write ? c->hw->mmio_progress[slot].write : c->hw->mmio_progress[slot].read) != value)
            ++c->mmio_epoch;
        c->hw->mmio_progress[slot].key = key;
        c->hw->mmio_progress[slot].seen |= bit;
        if (write) c->hw->mmio_progress[slot].write = value;
        else c->hw->mmio_progress[slot].read = value;
    }
#endif
    if (c->log) cpu_log(c, "MMIO %c address=%08x width=%u value=%08x pc=%08x", write ? 'W' : 'R', address, width, value, c->pc);
}

/* Normal builds keep only the two diagnostic switches on the MMIO path. */
static inline void record(cpu_t *c, uint32_t address, unsigned width, uint32_t value, int write) {
#ifndef FF8_STATS
    if (!c->stall_limit && !c->log) return;
#endif
    record_access(c, address, width, value, write);
}

static uint64_t video_time(struct hw *h, uint64_t edge) {
    /* HBlank starts at 3/4 line. Keep a rational clock across every edge. */
    uint64_t quarters = (edge / 2) * 4 + (edge & 1 ? 4 : 3);
    if (h->video_pal) return h->video_start + CYCLE_DIV(cycle_mul(quarters, HW_CPU_HZ), (50u * 314u * 4u));
    return h->video_start + CYCLE_DIV(cycle_mul(quarters, (uint64_t)HW_CPU_HZ * 1001), (60000u * 263u * 4u));
}

static uint64_t video_position(cpu_t *c) {
    struct hw *h = c->hw;
    uint64_t elapsed = h->now - h->video_start;
    uint64_t quarters = h->video_pal ?
        CYCLE_DIV(cycle_mul(elapsed, 50 * 314 * 4), HW_CPU_HZ) :
        CYCLE_DIV(cycle_mul(elapsed, 60000 * 263 * 4), (uint64_t)HW_CPU_HZ * 1001);
    uint64_t edge = quarters / 4 * 2 + (quarters % 4 >= 3);
    while (video_time(h, edge) <= h->now) ++edge;
    h->line = (unsigned)(edge / 2 - (h->video_pal ? CYCLE_DIV(edge / 2, 314) * 314 : CYCLE_DIV(edge / 2, 263) * 263));
    h->hblank = edge & 1; h->timers_dirty = 1;
    return edge;
}

static void video_deadline(cpu_t *c, uint64_t edge) {
    struct hw *h = c->hw;
    uint32_t lines = h->video_pal ? 314 : 263;
    if (!timers_observe_hblank(c)) {
        uint64_t frame = h->video_pal ? CYCLE_DIV(edge, 628) : CYCLE_DIV(edge, 526);
        unsigned first = h->video_pal ? 288 : 240;
        edge = (cycle_mul(frame, lines) + (h->line < first ? first : lines)) * 2 - 1;
    }
    h->video_edge = edge;
    h->next_video = video_time(h, edge);
}

static void video_schedule(cpu_t *c) { video_deadline(c, video_position(c)); }

static void spu_irq(void *context) {
    cpu_t *c = context;
    cpu_log(c, "SPU IRQ cycle=%llu pc=%08x address=%05x", (unsigned long long)c->cycles,
            c->pc, spu_read(c->hw->spu, 0x1a4) * 8);
    hw_irq_raise(c, 9);
}

struct hw *hw_get(cpu_t *c) {
    unsigned i;
    if (c->hw) return c->hw;
    c->hw = calloc(1, sizeof(*c->hw));
    if (!c->hw) { cpu_stop(c, "cannot allocate hardware"); abort(); }
    c->hw->gpu = gpu_create();
    if (!c->hw->gpu) { cpu_stop(c, "cannot allocate GPU"); abort(); }
    mdec_init(&c->hw->mdec);
    cdrom_audio_init(&c->hw->cd);
    c->hw->spu = spu_create();
    if (!c->hw->spu) { cpu_stop(c, "cannot allocate SPU"); abort(); }
    spu_set_irq(c->hw->spu, spu_irq, c);
    c->hw->dpcr = 0x07654321;
    video_schedule(c);
    c->hw->cd.command_due = c->hw->cd.second_due = c->hw->cd.read_due = HW_NEVER;
    c->hw->cd.lid_due = HW_NEVER;
    c->hw->cd.status = 0x10;
    c->hw->sio.due = c->hw->sio.ack_due = HW_NEVER;
    mem_write32(c, 0x100, BIOS_EXCB_BASE);
    mem_write32(c, 0x104, 4 * 8);
    for (i = 0; i < 8; ++i) mem_write32(c, BIOS_EXCB_BASE + i * 4, 0);
    mem_write32(c, 0x108, BIOS_PCB_BASE); mem_write32(c, 0x10c, 4);
    mem_write32(c, 0x110, BIOS_TCB_BASE); mem_write32(c, 0x114, BIOS_THREADS * BIOS_TCB_SIZE);
    mem_write32(c, BIOS_PCB_BASE, BIOS_TCB_BASE);
    for (i = 0; i < BIOS_THREADS * BIOS_TCB_SIZE; i += 4) mem_write32(c, BIOS_TCB_BASE + i, 0);
    for (i = 0; i < BIOS_THREADS; ++i) mem_write32(c, BIOS_TCB_BASE + i * BIOS_TCB_SIZE, i ? 0x1000 : 0x4000);
    mem_write32(c, 0x140, BIOS_FCB_BASE); mem_write32(c, 0x144, 16 * 0x2c);
    mem_write32(c, 0x150, BIOS_DCB_BASE); mem_write32(c, 0x154, 10 * 0x50);
    for (i = 0; i < 10 * 0x50; i += 4) mem_write32(c, BIOS_DCB_BASE + i, 0);
    for (i = 0; i < 16 * 0x2c; i += 4) mem_write32(c, BIOS_FCB_BASE + i, 0);
    {
        static const char names[] = "bu\0MEMORY CARD";
        for (i = 0; i < sizeof(names); ++i) mem_write8(c, 0x80003400u + i, (uint8_t)names[i]);
    }
    mem_write32(c, BIOS_DCB_BASE, 0x80003400u);
    mem_write32(c, BIOS_DCB_BASE + 4, 0x14);
    mem_write32(c, BIOS_DCB_BASE + 8, 128);
    mem_write32(c, BIOS_DCB_BASE + 0xc, 0x80003403u);
    mem_write32(c, BIOS_DCB_BASE + 0x34, BIOS_CARD_FIRSTFILE);
    /* An original HLE device entry: dev_card_firstfile(fcb, path, direntry). */
    mem_write32(c, BIOS_CARD_FIRSTFILE, 0x24090069u);
    mem_write32(c, BIOS_CARD_FIRSTFILE + 4, 0x240a00a0u);
    mem_write32(c, BIOS_CARD_FIRSTFILE + 8, 0x01400008u);
    mem_write32(c, BIOS_CARD_FIRSTFILE + 12, 0);
    for (i = 0; i < 2; ++i) { c->hw->cards[i].status = 1; c->hw->cards[i].due = HW_NEVER; }
    for (i = 0; i < 7; ++i) c->hw->dma[i].due = HW_NEVER;
    for (i = 0; i < 3; ++i) c->hw->timers[i].mode = 0x400;
    c->hw->timers_dirty = 1;
    for (i = 0; i < 4; ++i) c->hw->rcnt_ack[i] = 1;
    hw_reschedule(c);
    /* Let the CPU arm its diagnostic instruction budget before a distant first
     * device deadline. This is a single initial safe point, not a device poll. */
    if (c->instruction_limit) c->next_event = c->cycles;
    return c->hw;
}

void hw_destroy(cpu_t *c) {
    if (!c->hw) return;
    if (c->hw->cd.file) fclose(c->hw->cd.file);
    free(c->hw->cd.lba_reads); free(c->hw->cd.lba_dma);
    spu_destroy(c->hw->spu);
    free(c->hw->gpu); free(c->hw); c->hw = NULL;
}

void hw_irq_raise(cpu_t *c, unsigned source) {
    struct hw *h = c->hw;
    if (source >= 11) return;
    ++h->irq_raised[source];
    h->i_stat |= 1u << source;
    ++c->mmio_epoch;
}

/* MDEC DMA moves one word per event. Its only effects the CPU can observe
 * without a synchronizing MMIO access are guest RAM words and the completion
 * IRQ. While a media channel is armed, the CPU need not stop at each word: it
 * stops at the earliest other device deadline or at a lower bound on media
 * completion (one word per two cycles), and every CPU RAM access to the
 * channels' address range first runs the device calendar up to that access.
 * The RAM range is enforced through ram_limit: accesses at or above fence_lo
 * take the timed slow path, which calls hw_ram_fence. Code pages, reverse
 * transfers and wrapping ranges keep the per-word schedule. */
static int media_fence(cpu_t *c, uint32_t lo[2], uint32_t hi[2], uint64_t *bound) {
    struct hw *h = c->hw;
    uint32_t page;
    uint64_t done = HW_NEVER;
    unsigned ch, any = 0;
    lo[0] = lo[1] = hi[0] = hi[1] = 0;
    for (ch = 0; ch < 2; ++ch) {
        uint32_t chcr = h->dma[ch].chcr, address = h->dma[ch].madr & 0x1ffffc, left = h->media_dma[ch].remaining;
        uint32_t start, end;
        uint64_t at;
        lo[ch] = hi[ch] = 0;
        if (!(chcr & 0x1000000) || !(h->dpcr & (8u << (ch * 4)))) {
            if (left || h->dma[ch].due != HW_NEVER) return 0;
            continue;
        }
        if (chcr & 2) return 0;
        if (!left) {
            uint32_t bcr = h->dma[ch].bcr, size = bcr & 0xffff ? bcr & 0xffff : 0x10000;
            uint64_t count = ((chcr >> 9) & 3) == 1 ? (uint64_t)size * ((bcr >> 16) ? bcr >> 16 : 0x10000) : size;
            if (count > RAM_SIZE / 4) return 0;
            left = (uint32_t)count;
        }
        at = (h->dma[ch].due != HW_NEVER ? h->dma[ch].due : h->now + 2) + 2 * (uint64_t)(left - 1);
        if (at < done) done = at;
        /* MDEC output reorders words within a macroblock around MADR. */
        start = ch && address >= 4096 ? address - 4096 : ch ? 0 : address;
        end = address + left * 4 + (ch ? 4096 : 0);
        if (end > RAM_SIZE || end < address) return 0;
        for (page = start >> CODE_PAGE_SHIFT; page <= (end - 1) >> CODE_PAGE_SHIFT; ++page)
            if (c->code_watched[page]) return 0;
        lo[ch] = start; hi[ch] = end;
        any = 1;
    }
    *bound = done;
    return any;
}

void hw_ram_fence(cpu_t *c, uint32_t physical, uint32_t length) {
    struct hw *h = c->hw;
    physical &= RAM_SIZE - 1;
    if (h && ((physical < h->fence_hi[0] && physical + length > h->fence_lo[0]) ||
              (physical < h->fence_hi[1] && physical + length > h->fence_lo[1]))) hw_sync(c);
}

/* A fixed calendar has one slot per device class. Only changed devices and
 * guest register writes rebuild it; syncs before the minimum do no device work. */
void hw_reschedule(cpu_t *c) {
    struct hw *h = c->hw;
    uint64_t *q = h->deadline, next, other, value, dma_other;
    unsigned i;
#if defined(FF8_STATS)
    ++h->reschedules;
#endif
    /* "other" is the minimum over every slot except MDEC and DMA. */
    other = q[HW_VIDEO] = h->next_video;
    if (h->timers_dirty) { h->timer_due = timers_next(c); h->timers_dirty = 0; }
    value = q[HW_TIMER] = h->timer_due;
    if (value < other) other = value;
    value = q[HW_SPU] = h->spu_dma.remaining ? spu_next_event(h->spu) : spu_batch_deadline(h->spu);
    if (value < other) other = value;
    value = h->cd.read_due < h->cd.lid_due ? h->cd.read_due : h->cd.lid_due;
    if (!h->cd.irq_flags) {
        if (h->cd.command_due < value) value = h->cd.command_due;
        if (h->cd.second_due < value) value = h->cd.second_due;
        if (h->cd.lid_irq || h->cd.sector_valid) value = h->now;
    }
    q[HW_CD] = value;
    if (value < other) other = value;
    value = q[HW_SIO] = h->sio.due < h->sio.ack_due ? h->sio.due : h->sio.ack_due;
    if (value < other) other = value;
    value = q[HW_CARD] = h->cards[0].due < h->cards[1].due ? h->cards[0].due : h->cards[1].due;
    if (value < other) other = value;
    dma_other = HW_NEVER;
    for (i = 2; i < 7; ++i) if (h->dma[i].due < dma_other) dma_other = h->dma[i].due;
    value = h->dma[0].due < h->dma[1].due ? h->dma[0].due : h->dma[1].due;
    q[HW_DMA] = value < dma_other ? value : dma_other;
    q[HW_MDEC] = h->mdec.due;
    next = other < q[HW_DMA] ? other : q[HW_DMA];
    if (q[HW_MDEC] < next) next = q[HW_MDEC];
    h->next_device = next;
    h->media_bound = HW_NEVER;
    {
        uint32_t lo[2] = {0, 0}, hi[2] = {0, 0}, top = 0x800000u;
        uint64_t bound;
        if (((h->dma[0].chcr | h->dma[1].chcr) & 0x1000000) && media_fence(c, lo, hi, &bound)) {
            h->media_bound = bound;
            next = bound < other ? bound : other;
            if (dma_other < next) next = dma_other;
            for (i = 0; i < 2; ++i) if (hi[i] && lo[i] < top) top = lo[i];
        } else lo[0] = lo[1] = hi[0] = hi[1] = 0;
        h->fence_lo[0] = lo[0]; h->fence_lo[1] = lo[1]; h->fence_hi[0] = hi[0]; h->fence_hi[1] = hi[1];
        c->ram_top = top;
        if (c->ram_limit) c->ram_limit = top;
    }
    if (!h->in_exception && (c->cop0[12] & 0x401) == 0x401 &&
        (h->i_stat & h->i_mask) && c->cycles + 1 < next) next = c->cycles + 1;
    c->next_event = next;
}

void hw_sr_changed(cpu_t *c) {
    if (c->hw) hw_reschedule(c);
}

int hw_interrupt_window(cpu_t *c, uint32_t window) {
    struct hw *h = c->hw;
    uint64_t until = c->cycles + (uint64_t)window * CYCLES_PER_INSN;
    unsigned i, spu_interrupt, video_interrupts;
    if (!h || (h->i_stat & h->i_mask) || bios_card_pending(c)) return 1;
    /* I_MASK alone does not arm an interrupt source. SPU RAM IRQs require
     * both SPUCNT enables and an unlatched IRQ; timer blank edges require
     * target/overflow IRQ mode and a repeatable or not-yet-fired timer. */
    spu_interrupt = (h->i_mask & 0x200) && spu_irq_deadline(h->spu) <= until;
    video_interrupts = h->i_mask & 0x30;
    for (i = 0; i < 2; ++i)
        if (!(h->timers[i].mode & 0x30) || (h->timers[i].fired && !(h->timers[i].mode & 0x40)))
            video_interrupts &= ~(0x10u << i);
    /* Check the entire calendar: a harmless first event must not hide an IRQ
     * later in the same window. DMA, MDEC, CD, SIO, cards and timers retain
     * conservative instruction-boundary fallback, including chained work. */
    for (i = 0; i < HW_EVENTS; ++i) {
        if (h->deadline[i] > until) continue;
        if (i == HW_SPU) {
            /* Samples touch SPU RAM only. An active SPU DMA can also touch
             * CPU RAM and complete a DMA IRQ, so keep that case precise. */
            if (h->spu_dma.remaining || spu_interrupt) return 1;
        } else if (i == HW_VIDEO) {
            /* Blank edges can clock/reset timers 0/1; timer 2 is independent
             * and covered by HW_TIMER. VBlank also advances SPU and presents
             * a frame, so preserve its boundary even with VBlank masked. */
            if (video_interrupts || spu_interrupt) return 1;
            /* The next VBlank edge changes once per frame; 64-bit division
             * is a libgcc call on some compilers, so keep it out of every check. */
            if (!h->vblank_valid || h->video_edge > h->vblank_edge ||
                h->vblank_start != h->video_start || h->vblank_pal != h->video_pal) {
                uint64_t lines = h->video_pal ? 314 : 263;
                uint64_t first = h->video_pal ? 288 : 240;
                uint64_t edge = (h->video_edge / (lines * 2) * lines + first) * 2 - 1;
                if (edge < h->video_edge) edge += lines * 2;
                h->vblank_edge = edge; h->vblank_time = video_time(h, edge);
                h->vblank_start = h->video_start; h->vblank_pal = h->video_pal; h->vblank_valid = 1;
            }
            if (h->vblank_time <= until) return 1;
        } else if ((i == HW_MDEC || i == HW_DMA) && h->media_bound != HW_NEVER) {
            /* Match hw_reschedule's existing RAM-fenced media horizon. The
             * intermediate word events cannot complete sooner than bound;
             * CPU accesses to their RAM still synchronize through ram_top. */
            unsigned ch;
            if (h->media_bound <= until) return 1;
            if (i == HW_DMA)
                for (ch = 2; ch < 7; ++ch) if (h->dma[ch].due <= until) return 1;
        } else return 1;
    }
    return 0;
}

static void video_event(cpu_t *c) {
    struct hw *h = c->hw;
    timers_advance(c, 0);
    h->hblank = !(h->video_edge & 1);
    timers_blank(c, 0, h->hblank);
    if (!h->hblank) {
        uint64_t line = h->video_edge / 2 + 1;
        h->line = (unsigned)(line - (h->video_pal ? CYCLE_DIV(line, 314) * 314 : CYCLE_DIV(line, 263) * 263));
        if (h->line == (h->video_pal ? 288u : 240u)) {
            h->vblank = 1;
            timers_blank(c, 1, 1);
            ++h->vblanks; gpu_vblank(h->gpu); hw_irq_raise(c, 0);
            spu_advance(h->spu, h->now);
            if (h->present) h->present(c, h->present_context);
        } else if (!h->line) { h->vblank = 0; timers_blank(c, 1, 0); }
    }
    video_schedule(c);
}

void hw_sync(cpu_t *c) {
    struct hw *h = hw_get(c);
    if (h->syncing) return;
#if defined(FF8_STATS)
    ++h->sync_calls;
#endif
    /* A partial DMA4 catch-up can itself read a RAM-fenced media buffer.
     * next_device proves media is already current on this early path; protect
     * it from recursively entering dma_sync before its word cursor advances. */
    if (h->next_device > c->cycles) {
        h->now = c->cycles;
        if (h->spu_dma.remaining) {
            h->syncing = 1;
            dma_sync(c); hw_reschedule(c);
            h->syncing = 0;
        }
        return;
    }
    h->syncing = 1;
    for (;;) {
        /* hw_reschedule writes next_device with the calendar: min(deadline[]). */
        uint64_t until = h->next_device;
        unsigned i, ready = 0;
        if (until > c->cycles) break;
        if (until < h->now) until = h->now;
        h->now = until;
        for (i = 0; i < HW_EVENTS; ++i) if (h->deadline[i] <= until) {
            ready |= 1u << i;
#if defined(FF8_STATS)
            ++h->event_calls[i];
#endif
        }
        if (ready & (1u << HW_SPU)) spu_advance(h->spu, until);
        if (ready & (1u << HW_VIDEO)) video_event(c);
        if (ready & (1u << HW_TIMER)) timers_advance(c, 0);
        if (ready & (1u << HW_MDEC)) { mdec_event(&h->mdec, until); dma_kick(c); }
        if (ready & (1u << HW_CD)) { spu_advance(h->spu, until); cdrom_event(c); dma_kick(c); }
        if (ready & (1u << HW_DMA)) {
#if FF8_MDEC_CALENDAR
            /* Media words touch CPU RAM and MDEC only. The independent SPU
             * calendar bounds every burst and services possible IRQs first;
             * advancing SPU per word defeats its exact sample batching. */
            for (i = 2; i < 7; ++i) if (h->dma[i].due <= until) break;
            if (i < 7) spu_advance(h->spu, until);
#else
            spu_advance(h->spu, until);
#endif
            /* Other calendar slots are fresh unless a device that can move them ran in this step. */
            h->dma_burst = !(ready & ~((1u << HW_DMA) | (1u << HW_SPU) | (1u << HW_MDEC)));
            dma_event(c);
            h->dma_burst = 0;
        }
        if (ready & (1u << HW_SIO)) sio_event(c);
        if (ready & (1u << HW_CARD)) bios_card_event(c);
        hw_reschedule(c);
    }
    h->now = c->cycles;
    if (h->spu_dma.remaining) { dma_sync(c); hw_reschedule(c); }
    h->syncing = 0;
}

void hw_event(cpu_t *c) {
    struct hw *h = hw_get(c);
    hw_sync(c);
    if (h->i_stat & h->i_mask) c->cop0[13] |= 0x400;
    else c->cop0[13] &= ~0x400u;
    if (!h->in_exception && !c->in_delay_slot && (c->cop0[12] & 0x401) == 0x401 &&
        ((h->i_stat & h->i_mask) || bios_card_pending(c))) {
        bios_exception(c, 0x400);
        hw_reschedule(c);
    } else if (c->next_event <= c->cycles || h->media_bound != HW_NEVER) {
        /* hw_sync already published the device/IRQ minimum. Keep it when it
         * is still in the future; only an expired CPU-only safe point needs
         * rearming. Exception callbacks can change any device, so rebuild
         * unconditionally after delivering one. A media fence also needs its
         * code-watch check even if the cached CPU horizon is still future. */
        hw_reschedule(c);
    }
}

uint32_t hw_read(cpu_t *c, uint32_t address, unsigned width) {
    struct hw *h = hw_get(c);
    uint32_t value = 0, offset = address & 65535;
    unsigned i;
    hw_sync(c);
    for (i = 0; i < width && offset + i < 65536; ++i) value |= (uint32_t)c->mmio[offset + i] << (i * 8);
    if ((address & ~3u) == 0x1f801070) value = h->i_stat >> ((address & 3) * 8);
    else if ((address & ~3u) == 0x1f801074) value = h->i_mask >> ((address & 3) * 8);
    else if (address >= 0x1f801080 && address < 0x1f8010f8) value = dma_read(c, address & ~3u) >> ((address & 3) * 8);
    else if (address >= 0x1f801100 && address < 0x1f80112c) value = timers_read(c, address & ~3u) >> ((address & 3) * 8);
    else if (address >= 0x1f801800 && address < 0x1f801804) {
        value = 0;
        for (i = 0; i < width; ++i) value |= (uint32_t)cdrom_read(c, (address + i) & 3) << (8 * i);
    } else if (address == 0x1f801810) value = gpu_read(h->gpu);
    else if (address == 0x1f801814) {
        uint64_t elapsed = h->now - h->video_start;
        unsigned line;
        /* Whole clock periods contain an integer number of frames. Remove
         * them before multiplying so long sessions cannot overflow. */
        if (h->video_pal) {
            elapsed -= cycle_mul(CYCLE_DIV(elapsed, HW_CPU_HZ), HW_CPU_HZ);
            line = (unsigned)CYCLE_DIV(cycle_mul(elapsed, 50u * 314u), HW_CPU_HZ) % 314u;
        } else {
            elapsed -= cycle_mul(CYCLE_DIV(elapsed, (uint64_t)HW_CPU_HZ * 1001u), (uint64_t)HW_CPU_HZ * 1001u);
            line = (unsigned)CYCLE_DIV(cycle_mul(elapsed, 60000u * 263u), (uint64_t)HW_CPU_HZ * 1001u) % 263u;
        }
        value = gpu_status_scanline(h->gpu, line);
    }
    else if (address == 0x1f801820) { value = mdec_read(&h->mdec, h->now); dma_kick(c); hw_reschedule(c); }
    else if (address == 0x1f801824) value = mdec_status(&h->mdec);
    else if (address >= 0x1f801040 && address < 0x1f801050) value = sio_read(c, address);
    else if (address >= 0x1f801c00 && address < 0x1f802000) {
        spu_advance(h->spu, h->now);
        value = 0;
        for (i = 0; i < width; ++i)
            value |= (uint32_t)((spu_read(h->spu, (address + i - 0x1f801c00) & ~1u) >> (((address + i) & 1) * 8)) & 255) << (i * 8);
    }
    if (width == 1) value &= 255;
    if (width == 2) value &= 65535;
    record(c, address, width, value, 0);
    return value;
}

void hw_write(cpu_t *c, uint32_t address, unsigned width, uint32_t value) {
    struct hw *h = hw_get(c);
    unsigned i, shift = (address & 3) * 8;
    uint32_t mask = width == 4 ? ~0u : ((1u << (width * 8)) - 1), offset = address & 65535;
    uint16_t spu_halfword = (uint16_t)value;
    hw_sync(c);
    value &= mask;
    for (i = 0; i < width && offset + i < 65536; ++i) c->mmio[offset + i] = (uint8_t)(value >> (i * 8));
    record(c, address, width, value, 1);
    if ((address & ~3u) == 0x1f801070) h->i_stat &= (value << shift) | ~(mask << shift);
    else if ((address & ~3u) == 0x1f801074) h->i_mask = ((h->i_mask & ~(mask << shift)) | (value << shift)) & 0x7ff;
    else if (address >= 0x1f801080 && address < 0x1f8010f8) dma_write(c, address & ~3u, value << shift, mask << shift);
    else if (address >= 0x1f801100 && address < 0x1f80112c) {
        if (!(address & 3)) {
            uint64_t edge = video_position(c);
            timers_write(c, address, value);
            video_deadline(c, edge);
        }
    } else if (address >= 0x1f801800 && address < 0x1f801804) {
        for (i = 0; i < width; ++i) cdrom_write(c, (address + i) & 3, value >> (8 * i));
    } else if (address == 0x1f801810) {
        gpu_gp0(h->gpu, value);
        if (gpu_status(h->gpu) & (1u << 24)) {
            hw_irq_raise(c, 1);
            hw_reschedule(c);
        } else if (h->media_bound != HW_NEVER) hw_reschedule(c);
        /* GP0 changes rendering state, not the device calendar. */
        return;
    }
    else if (address == 0x1f801814) {
        unsigned op = (value >> 24) & 63;
        if (op == 0 || op == 8) timers_advance(c, 0);
        gpu_gp1(h->gpu, value);
        if (op == 0 || op == 8) h->timers_dirty = 1;
        if (h->video_pal != !!(gpu_status(h->gpu) & (1u << 20))) {
            h->video_pal = !!(gpu_status(h->gpu) & (1u << 20));
            h->video_start = h->now; h->video_edge = 0; h->line = h->hblank = h->vblank = 0;
            timers_blank(c, 0, 0); timers_blank(c, 1, 0);
            video_schedule(c);
        } else if (op != 0 && op != 8 && h->media_bound == HW_NEVER) {
            /* Only reset/display-mode commands affect clock rates or blank
             * edges. FIFO, display-address and drawing commands do not. */
            return;
        }
    }
    else if (address == 0x1f801820) { mdec_write(&h->mdec, value, h->now); dma_kick(c); }
    else if (address == 0x1f801824) { mdec_control(&h->mdec, value); dma_kick(c); }
    else if (address >= 0x1f801040 && address < 0x1f801050) sio_write(c, address, value);
    else if (address >= 0x1f801c00 && address < 0x1f802000 && !(address & 1)) {
        spu_advance(h->spu, h->now);
        spu_write(h->spu, address - 0x1f801c00, spu_halfword);
        if (width == 4) spu_write(h->spu, address + 2 - 0x1f801c00, (uint16_t)(value >> 16));
        if (h->spu_dma.remaining) { h->dma[4].due = HW_NEVER; dma_kick(c); }
    }
    hw_reschedule(c);
}

void hw_summary(cpu_t *c, FILE *out) {
    static const char *names[] = {"VBlank", "GPU", "CD", "DMA", "Timer0", "Timer1", "Timer2", "SIO0", "SIO1", "SPU", "PIO"};
    struct hw *h = hw_get(c);
    hw_cd_t *cd = &h->cd;
    unsigned i, j;
    fprintf(out, "TIMING policy=%s\n",
#ifdef FF8_PLAY
            "play"
#else
            "exact"
#endif
    );
#if defined(FF8_STATS)
    fprintf(out, "Scheduler: syncs=%llu rebuilds=%llu video=%llu timer=%llu spu=%llu mdec=%llu cd=%llu sio=%llu card=%llu dma=%llu\n",
            (unsigned long long)h->sync_calls, (unsigned long long)h->reschedules,
            (unsigned long long)h->event_calls[HW_VIDEO], (unsigned long long)h->event_calls[HW_TIMER],
            (unsigned long long)h->event_calls[HW_SPU], (unsigned long long)h->event_calls[HW_MDEC],
            (unsigned long long)h->event_calls[HW_CD], (unsigned long long)h->event_calls[HW_SIO],
            (unsigned long long)h->event_calls[HW_CARD], (unsigned long long)h->event_calls[HW_DMA]);
#endif
    spu_summary(h->spu, out);
    fprintf(out, "Hardware: cycles=%llu VBlanks=%llu I_STAT=%03x I_MASK=%03x\n", (unsigned long long)c->cycles,
            (unsigned long long)h->vblanks, h->i_stat, h->i_mask);
    fprintf(out, "IRQ source raised delivered (exception entries with source pending):\n");
    for (i = 0; i < 11; ++i) fprintf(out, "  %s %llu %llu\n", names[i], (unsigned long long)h->irq_raised[i], (unsigned long long)h->irq_delivered[i]);
    fprintf(out, "MDEC: blocks=%llu words_in=%llu words_out=%llu status=%08x\n",
            (unsigned long long)h->mdec.blocks_decoded, (unsigned long long)h->mdec.words_in,
            (unsigned long long)h->mdec.words_out, mdec_status(&h->mdec));
    fprintf(out, "MDEC calendar: spans=%llu words_in=%llu words_out=%llu exact_in=%llu exact_out=%llu parser=%llu boundary=%llu layout=%llu folded-mdec=%llu\n",
            (unsigned long long)h->media_calendar.spans,
            (unsigned long long)h->media_calendar.words[0], (unsigned long long)h->media_calendar.words[1],
            (unsigned long long)h->media_calendar.exact_words[0], (unsigned long long)h->media_calendar.exact_words[1],
            (unsigned long long)h->media_calendar.parser, (unsigned long long)h->media_calendar.boundary,
            (unsigned long long)h->media_calendar.layout, (unsigned long long)h->media_calendar.mdec_events);
    fprintf(out, "XA: sectors=%llu frames_44100=%llu filtered=%llu\n",
            (unsigned long long)cd->xa_sectors, (unsigned long long)cd->xa_frames,
            (unsigned long long)cd->xa_filtered);
    fprintf(out, "DMA completions (channel: count):\n");
    for (i = 0; i < 7; ++i) if (h->dma[i].completions)
        fprintf(out, "  %u: %llu\n", i, (unsigned long long)h->dma[i].completions);
    fprintf(out, "CD commands (hex opcode: count):\n");
    for (i = 0; i < 256; ++i) if (cd->commands[i]) fprintf(out, "  %02x: %llu\n", i, (unsigned long long)cd->commands[i]);
    fprintf(out, "CD: sectors=%llu DMA bytes=%llu overruns=%llu INT1..5=%llu,%llu,%llu,%llu,%llu\n",
            (unsigned long long)cd->sectors_read, (unsigned long long)cd->bytes_dma, (unsigned long long)cd->overruns,
            (unsigned long long)cd->interrupts[1], (unsigned long long)cd->interrupts[2], (unsigned long long)cd->interrupts[3],
            (unsigned long long)cd->interrupts[4], (unsigned long long)cd->interrupts[5]);
    if (!cd->lba_reads || !cd->lba_dma) {
        fprintf(out, "CD LBA statistics disabled\n");
        return;
    }
    fprintf(out, "CD LBAs read (inclusive ranges; per-sector events in trace):\n");
    for (i = 0; i < cd->sectors; ++i) if (cd->lba_reads[i]) {
        unsigned start = i;
        while (i + 1 < cd->sectors && cd->lba_reads[i + 1]) ++i;
        fprintf(out, "  %u..%u\n", start, i);
    }
    fprintf(out, "IMG table LBA=%u entries=%u transferred=%s\n", cd->img_lba, cd->img_count,
            cd->img_lba < cd->sectors && cd->lba_dma[cd->img_lba] >= 1064 ? "yes" : "no");
    fprintf(out, "IMG file indices with CD DMA bytes (coverage, not proof of execution):\n");
    for (i = 0; i < cd->img_count; ++i) {
        unsigned sectors = (cd->img[i].size + 2047) / 2048, covered = 0, touched = 0;
        for (j = 0; j < sectors && cd->img[i].lba + j < cd->sectors; ++j) {
            uint32_t needed = j + 1 == sectors ? cd->img[i].size - j * 2048 : 2048;
            uint32_t bytes = cd->lba_dma[cd->img[i].lba + j];
            if (bytes) ++touched;
            if (bytes >= needed) ++covered;
        }
        if (touched) fprintf(out, "  %u LBA=%u bytes=%u sectors=%u/%u %s\n", i, cd->img[i].lba, cd->img[i].size, covered, sectors, covered == sectors ? "complete" : "partial");
    }
}

#include "hw.h"

static void irq(cpu_t *c) {
    struct hw *h = c->hw;
    uint32_t before = h->dicr & 0x80000000u;
    unsigned active = (h->dicr & 0x8000) || ((h->dicr & 0x800000) && ((h->dicr >> 16) & (h->dicr >> 24) & 0x7f));
    h->dicr = (h->dicr & 0x7fffffffu) | (active ? 0x80000000u : 0);
    if (active && !before) hw_irq_raise(c, 3);
}

static uint32_t words(cpu_t *c, unsigned channel) {
    uint32_t bcr = c->hw->dma[channel].bcr, size = bcr & 0xffff;
    unsigned mode = (c->hw->dma[channel].chcr >> 9) & 3;
    uint64_t count;
    if (!size) size = 0x10000;
    count = mode == 1 ? (uint64_t)size * ((bcr >> 16) ? bcr >> 16 : 0x10000) : size;
    if (count > RAM_SIZE / 4) { cpu_stop(c, "DMA%u transfer exceeds RAM: %llu words", channel, (unsigned long long)count); return 0; }
    return (uint32_t)count;
}

/* MDEC DMA words are aligned and inside RAM. This is mem_read32/mem_write32
 * on such an address, without the CPU's slow-path RAM fence: writes are
 * dropped while the cache is isolated, as the slow path does. */
static uint32_t media_ram_read(cpu_t *c, uint32_t address) { return ram_read(c, address, 4); }
static void media_ram_write(cpu_t *c, uint32_t address, uint32_t value) {
    if (!(c->cop0[12] & 0x10000u)) {
        ram_write(c, address, 4, value);
        FRAME_TAGS(frame_tags_reload(c, address, 4));
        FRAME_ORACLE(frame_oracle_reload(c, address, 4));
    }
}

#ifdef FF8_PLAY
/* A media grant has no intervening guest observer. Move its ready FIFO prefix
 * together and run the parser once. Never cross a grant/BCR/completion boundary,
 * a RAM wrap, or the GPU's ownership of the output buffer. The exact calendar
 * below retains its word-at-a-time path. */
static unsigned media_play_words(cpu_t *c, unsigned ch, uint32_t address, uint32_t step) {
    struct hw *h = c->hw;
    mdec_t *m = &h->mdec;
    unsigned n = h->media_dma[ch].grant, i;
    unsigned ready = ch ? m->fifo_count : (64 - m->count) / 2;
    if (n > ready) n = ready;
    if (n > h->media_dma[ch].block_left) n = h->media_dma[ch].block_left;
    if (n > h->media_dma[ch].remaining) n = h->media_dma[ch].remaining;
    if (step != 4) n = 1;
    else if (n > (RAM_SIZE - address) / 4) n = (RAM_SIZE - address) / 4;
    /* Another DMA can observe an intermediate prefix too. The calendar's
     * fences conservatively cover every remaining media address (including
     * output block offsets); absent/wrapping fences keep word steps. Fences
     * may shrink during this burst, so their last calendar snapshot is safe. */
    for (i = 2; i < 7; ++i)
        if (h->dma[i].chcr & 0x1000000) n = 1;
    if (h->dma[ch ^ 1u].chcr & 0x1000000) {
        unsigned other = ch ^ 1u;
        uint32_t lo = ch ? (address >= 4096 ? address - 4096 : 0) : address;
        uint32_t hi = address + n * 4 + (ch ? 4096 : 0);
        if (n == h->media_dma[ch].remaining ||
            !h->fence_hi[other] || hi > RAM_SIZE ||
            (h->fence_lo[other] < hi && lo < h->fence_hi[other])) n = 1;
    }
    for (i = 0; i < n; ++i) {
        if (ch) {
            unsigned at = (m->fifo_head + i) % 32;
            uint32_t dest = (address + (uint32_t)(m->fifo[at].offset * 4)) & 0x1ffffc;
            media_ram_write(c, dest, m->fifo[at].word);
        } else {
            uint32_t value = media_ram_read(c, address);
            m->input[(m->head + m->count++) % 64] = (uint16_t)value;
            m->input[(m->head + m->count++) % 64] = (uint16_t)(value >> 16);
        }
        address += step;
    }
    if (ch) {
        m->fifo_head = (m->fifo_head + n) % 32; m->fifo_count -= n;
        m->words_out += n;
    } else m->words_in += n;
    mdec_event(m, h->now);
    return n;
}
#endif

static int gpu_precedes_mdec_out(const struct hw *h) {
    /* GPU RAM reads are batched at completion. While that DMA owns the
     * bus, MDEC must not overwrite its source buffer with the next column.
     * Lower DPCR values win; equal priorities favour the higher channel. */
    return h->dma[2].due != HW_NEVER && (h->dma[2].chcr & 1) &&
           ((h->dpcr >> 8) & 7) <= ((h->dpcr >> 4) & 7);
}

static uint64_t spu_due(cpu_t *c) {
    struct hw *h = c->hw;
    uint64_t first = h->spu_dma.word_due, sample = spu_next_event(h->spu);
    uint32_t count = h->spu_dma.remaining, irq_words = spu_dma_irq_words(h->spu);
    /* A RAM-to-SPU burst ends before the next sample or the IRQ word. Keep
     * SPU-to-RAM word deadlines because ordinary guest RAM reads observe them. */
    if (!(h->dma[4].chcr & 1) || first >= sample) count = 1;
    else {
        uint32_t before_sample = (uint32_t)((sample - 1 - first) / 32 + 1);
        if (count > before_sample) count = before_sample;
        if (count > irq_words) count = irq_words;
    }
    return first + (uint64_t)(count - 1) * 32;
}

static void kick_channels(cpu_t *c, unsigned last) {
    struct hw *h = c->hw;
    unsigned ch;
    for (ch = 0; ch < last; ++ch) {
        uint32_t control = h->dma[ch].chcr;
        unsigned sync = (control >> 9) & 3;
        if (h->dma[ch].due != HW_NEVER || !(control & 0x1000000) || !(h->dpcr & (8u << (ch * 4)))) continue;
        if (!sync && !(control & 0x10000000)) continue;
        if (ch < 2) {
            if (ch == 1 && gpu_precedes_mdec_out(h)) continue;
            if ((ch == 0 && !(control & 1)) || (ch == 1 && (control & 1))) continue;
            if (!mdec_dma_word_ready(&h->mdec, ch)) continue;
            if (!h->media_dma[ch].grant && !(ch ? mdec_dma_out(&h->mdec) : mdec_dma_in(&h->mdec))) continue;
            if (!h->media_dma[ch].remaining) {
                h->media_dma[ch].remaining = words(c, ch);
                h->media_dma[ch].block_left = (h->dma[ch].bcr & 0xffff) ? h->dma[ch].bcr & 0xffff : 65536;
#ifdef FF8_MDEC_ACCEL
                if (ch == 0 && h->mdec.accel)
                    h->mdec.accel->prepare(&h->mdec, c->ram, h->dma[ch].madr & 0x1ffffc,
                                          h->media_dma[ch].remaining, control & 2 ? -4 : 4);
#endif
            }
            if (!h->media_dma[ch].grant)
                h->media_dma[ch].grant = h->media_dma[ch].block_left < 32 ? h->media_dma[ch].block_left : 32;
            h->dma[ch].due = h->now + 2;
            continue;
        }
        if (ch == 3 && h->cd.data_pos >= h->cd.data_size) continue;
        if (ch == 4) {
            unsigned first = !h->spu_dma.remaining;
            if (first) {
                h->spu_dma.remaining = words(c, ch);
                h->spu_dma.block_left = (h->dma[ch].bcr & 0xffff) ? h->dma[ch].bcr & 0xffff : 65536;
            }
            /* SPU RAM consumes halfwords at 16 CPU clocks each. Moving the
             * entire buffer at completion makes an early IRQ address fire
             * after software has already re-enabled IRQs for playback. */
            if (first) h->spu_dma.word_due = h->now + 48;
            h->dma[ch].due = spu_due(c);
            continue;
        }
        h->dma[ch].due = h->now + (sync == 2 ? 32 : 16 + words(c, ch) * 2);
    }
}
void dma_kick(cpu_t *c) { kick_channels(c, 7); }

static void gpu_words(cpu_t *c, uint32_t address, uint32_t count) {
    while (count) {
        uint32_t n = (RAM_SIZE - address) / 4;
        if (n > count) n = count;
#if defined(FF8_FRAME_PROVENANCE)
        if (c->frame_tags) gpu_gp0_ram(c->hw->gpu, c, address, n);
        else
#endif
        gpu_gp0_words(c->hw->gpu, c->ram + address, n);
        count -= n; address = 0;
    }
}

#if FF8_MDEC_CALENDAR
/* Between observations, a busy decoder cannot consume input or refill output.
 * A full output FIFO also blocks parsing until the last pending pixel word is
 * flushed. Transfer whole inert prefixes, leaving their boundary word to the
 * exact model below (including grants, BCR rollover, completion and IRQs). */
static void media_span(cpu_t *c, uint64_t other, uint64_t fixed) {
    struct hw *h = c->hw;
    mdec_t *m = &h->mdec;
    uint64_t limit = c->cycles, bound = other < fixed ? other : fixed;
    unsigned ch, active = 0, n = 32, pending = (m->output_size - m->output_pos) / 4;
    int busy = m->due != HW_NEVER && m->due > h->now;
    int blocked = m->due == HW_NEVER && m->fifo_count == 32 && pending;
    uint64_t sample = h->spu_dma.remaining ? spu_next_event(h->spu) : spu_batch_deadline(h->spu);
    if (sample < bound) bound = sample;
    if (m->due < bound) bound = m->due;
    if (bound <= h->now) { ++h->media_calendar.boundary; return; }
    if (bound - 1 < limit) limit = bound - 1;
    for (ch = 0; ch < 2; ++ch) {
        uint64_t due = h->dma[ch].due;
        uint32_t control = h->dma[ch].chcr, address = h->dma[ch].madr & 0x1ffffc;
        unsigned count;
        if (due > h->now) {
            if (due != HW_NEVER && due - 1 < limit) limit = due - 1;
            continue;
        }
        if (due != h->now || (control & 2) || ((control >> 9) & 3) != 1 ||
            !h->media_dma[ch].remaining || !h->media_dma[ch].grant ||
            !mdec_dma_word_ready(m, ch) || (ch && gpu_precedes_mdec_out(h))) {
            ++h->media_calendar.layout; return;
        }
        active |= 1u << ch;
        count = h->media_dma[ch].grant;
        if (h->media_dma[ch].block_left < count) count = h->media_dma[ch].block_left;
        if (h->media_dma[ch].remaining < count) count = h->media_dma[ch].remaining;
        if ((RAM_SIZE - address) / 4 < count) count = (RAM_SIZE - address) / 4;
        if (ch) {
            if (m->fifo_count < count) count = m->fifo_count;
            if (blocked && pending < count) count = pending;
        } else if ((64 - m->count) / 2 < count) count = (64 - m->count) / 2;
        if (count < n) n = count;
    }
    /* With no input and no pending pixels, output reads cannot wake a parser.
     * Every other parser state retains the original word deadlines. */
    if (!busy && !blocked && ((active & 1) || m->count || pending)) {
        ++h->media_calendar.parser; return;
    }
    if (limit < h->now || (limit - h->now) / 2 + 1 < n)
        n = limit < h->now ? 0 : (unsigned)((limit - h->now) / 2 + 1);
    if (!active || n < 2) { ++h->media_calendar.boundary; return; }
    if (active == 3) {
        uint32_t input = h->dma[0].madr & 0x1ffffc, output = h->dma[1].madr & 0x1ffffc;
        unsigned i;
        /* Refills stay behind the 32 old FIFO entries, so these are all output
         * destinations in the span. Include the retained final input word. */
        for (i = 0; i < n; ++i) {
            uint32_t dest = (output + i * 4 + (uint32_t)(m->fifo[(m->fifo_head + i) & 31].offset * 4)) & 0x1ffffc;
            if (dest >= input && dest < input + n * 4) { ++h->media_calendar.layout; return; }
        }
    }
    --n;
    for (ch = 0; ch < 2; ++ch) if (active & (1u << ch)) {
        uint32_t address = h->dma[ch].madr & 0x1ffffc;
        unsigned i;
        for (i = 0; i < n; ++i, address += 4) {
            if (!ch) {
                uint32_t word = media_ram_read(c, address);
                m->input[(m->head + m->count++) & 63] = (uint16_t)word;
                m->input[(m->head + m->count++) & 63] = (uint16_t)(word >> 16);
            } else {
                uint32_t dest = (address + (uint32_t)(mdec_dma_offset(m) * 4)) & 0x1ffffc, word;
                word = m->fifo[m->fifo_head].word;
                m->fifo_head = (m->fifo_head + 1) & 31; --m->fifo_count;
                ++m->words_out;
                media_ram_write(c, dest, word);
            }
        }
        /* The retained boundary word proves n < pending. No consumed word
         * reaches newly appended FIFO entries or wakes the parser, so refill
         * the whole prefix once, at its last original event date. */
        if (ch && blocked) mdec_event(m, h->now + (n - 1) * 2);
        if (!ch) m->words_in += n;
        h->dma[ch].madr = address & 0x1ffffc;
        h->dma[ch].due += n * 2;
        h->media_dma[ch].remaining -= n;
        h->media_dma[ch].block_left -= n;
        h->media_dma[ch].grant -= n;
        h->media_calendar.words[ch] += n;
    }
    h->now += n * 2;
    ++h->media_calendar.spans;
}
#endif

/* Media DMA moves one word per event. While only MDEC channels are due before
 * every other device deadline and the sync target, run their next events here:
 * the calendar would select exactly these steps, in the same channel order,
 * with no other device or guest code observing the skipped rebuilds. */
void dma_event(cpu_t *c) {
    struct hw *h = c->hw;
    unsigned ch, last = 7, changed = 1;
    /* Burst constants: other channels' deadlines (ch2-6 change only through
     * their own steps, which end the burst) and other devices' slots. */
    uint64_t other = HW_NEVER, fixed = HW_NEVER;
    for (;;) {
        unsigned stop = 0;
        uint64_t due, bound, spu;
#if FF8_MDEC_CALENDAR
        if (last == 2) media_span(c, other, fixed);
#endif
        for (ch = 0; ch < last; ++ch) if (h->dma[ch].due <= h->now) {
            changed = 1;
            uint32_t address = h->dma[ch].madr & 0x1ffffc, control = h->dma[ch].chcr;
            unsigned sync = (control >> 9) & 3, from_ram = control & 1;
            uint32_t count, i, step = control & 2 ? (uint32_t)-4 : 4;
            h->dma[ch].due = HW_NEVER;
            /* Only media word steps may continue a burst. */
            if (ch >= 2) stop = 1;
            if (ch < 2) {
                if (ch == 1 && gpu_precedes_mdec_out(h)) continue;
                if (!mdec_dma_word_ready(&h->mdec, ch)) continue;
                count = 1;
#ifdef FF8_PLAY
                count = media_play_words(c, ch, address, step);
                h->media_calendar.words[ch] += count;
                ++h->media_calendar.spans;
#else
                ++h->media_calendar.exact_words[ch];
                if (ch == 0) mdec_write(&h->mdec, media_ram_read(c, address), h->now);
                else {
                    uint32_t dest = (address + (uint32_t)(mdec_dma_offset(&h->mdec) * 4)) & 0x1ffffc;
                    media_ram_write(c, dest, mdec_read(&h->mdec, h->now));
                }
#endif
                address = (address + step * count) & 0x1ffffc;
                h->media_dma[ch].grant -= count;
                h->dma[ch].madr = address;
                h->media_dma[ch].block_left -= count;
                if (!h->media_dma[ch].block_left && sync == 1) {
                    h->dma[ch].bcr -= 0x10000;
                    h->media_dma[ch].block_left = (h->dma[ch].bcr & 0xffff) ? h->dma[ch].bcr & 0xffff : 65536;
                }
                /* An MDEC word changes only MDEC channel eligibility: every other
                 * channel was evaluated after its own last state change. The
                 * pass-end kick repeats this one unless a later channel changes. */
                h->media_dma[ch].remaining -= count;
                if (h->media_dma[ch].remaining) { kick_channels(c, 2); changed = 0; continue; }
                stop = 1;
                cpu_log(c, "MDEC DMA%u complete cycle=%llu blocks=%llu", ch,
                        (unsigned long long)h->now, (unsigned long long)h->mdec.blocks_decoded);
            } else if (ch == 4) {
                dma_sync(c);
                continue;
            } else if (ch == 2 && sync == 2 && from_ram) {
                unsigned budget = RAM_SIZE / 4;
                while (budget) {
                    uint32_t header = mem_read32(c, address), next = header & 0xffffff;
                    count = header >> 24;
                    if (count + 1 > budget) { cpu_stop(c, "oversized GPU DMA linked list"); return; }
                    budget -= count + 1;
                    gpu_words(c, (address + 4) & 0x1ffffc, count);
                    address = next & 0x1ffffc;
                    if (next & 0x800000) break;
                }
                if (!budget) { cpu_stop(c, "cyclic or oversized GPU DMA linked list"); return; }
            } else {
                count = words(c, ch);
                if (ch != 2 && ch != 3 && ch != 4 && ch != 6) { cpu_stop(c, "unsupported DMA channel %u", ch); return; }
                if (ch == 3 && from_ram) { cpu_stop(c, "invalid CD DMA direction"); return; }
                if (ch == 6) code_invalidate(c, (address - (count - 1) * 4) & 0x1fffff, (size_t)count * 4);
                if (ch == 3 && !from_ram)
                    code_invalidate(c, step == 4 ? address : (address - (count - 1) * 4) & 0x1fffff, (size_t)count * 4);
                if (ch == 2 && from_ram && step == 4) {
                    gpu_words(c, address, count);
                    address = (address + count * 4) & 0x1ffffc;
                } else for (i = 0; i < count; ++i) {
                    if (ch == 2) {
                        if (from_ram) {
#if defined(FF8_FRAME_PROVENANCE)
                            gpu_gp0_tagged(h->gpu, mem_read32(c, address), frame_tags_load(c, address, 4));
#else
                            gpu_gp0(h->gpu, mem_read32(c, address));
#endif
                        }
                        else {
                            mem_write32(c, address, gpu_read(h->gpu));
                            FRAME_TAGS(frame_tags_reload(c, address, 4));
                            FRAME_ORACLE(frame_oracle_reload(c, address, 4));
                        }
                    } else if (ch == 3) { mem_write32(c, address, cdrom_dma(c)); FRAME_TAGS(frame_tags_reload(c, address, 4)); FRAME_ORACLE(frame_oracle_reload(c, address, 4)); }
                    else if (ch == 6) {
                        uint32_t value = i + 1 == count ? 0xffffff : (address - 4) & 0x1fffff;
                        /* OTC writes aligned RAM only, never executable device data. */
                        FRAME_TAGS(frame_tags_invalidate(c, address, 4); frame_tags_reload(c, address, 4));
                        FRAME_ORACLE(frame_oracle_write(c, address, 4); frame_oracle_reload(c, address, 4));
                        c->ram[address] = (uint8_t)value; c->ram[address + 1] = (uint8_t)(value >> 8);
                        c->ram[address + 2] = (uint8_t)(value >> 16); c->ram[address + 3] = (uint8_t)(value >> 24);
                    }
                    address = (address + (ch == 6 ? (uint32_t)-4 : step)) & 0x1ffffc;
                }
                if (ch == 3) cpu_log(c, "CD DMA lba=%u dest=%06x words=%u", h->cd.data_lba, h->dma[ch].madr, count);
            }
            h->dma[ch].madr = address;
            if (sync == 1) h->dma[ch].bcr &= 0xffff;
            h->dma[ch].chcr &= ~0x11000000u;
            ++h->dma[ch].completions;
            if (h->dicr & (1u << (16 + ch))) h->dicr |= 1u << (24 + ch);
            irq(c);
            if (ch == 2 && (gpu_status(h->gpu) & (1u << 24))) hw_irq_raise(c, 1);
            ++c->mmio_epoch;
        }
        if (changed) dma_kick(c);
        if (!h->dma_burst || stop) return;
        if (last == 7) {
            unsigned i;
            for (ch = 2; ch < 7; ++ch) if (h->dma[ch].due < other) other = h->dma[ch].due;
            for (i = 0; i < HW_EVENTS; ++i)
                if (i != HW_DMA && i != HW_MDEC && i != HW_SPU && h->deadline[i] < fixed) fixed = h->deadline[i];
            last = 2;
        }
        due = h->dma[0].due < h->dma[1].due ? h->dma[0].due : h->dma[1].due;
#if FF8_MDEC_CALENDAR
        /* A decoder completion has no IRQ of its own. Retain its date and its
         * precedence over a simultaneous DMA word, but service it inside this
         * burst while the CPU and every other device are still fenced out. */
        if (h->mdec.due < due) due = h->mdec.due;
#endif
        if (due > c->cycles || other <= due) return;
        spu = h->spu_dma.remaining ? spu_next_event(h->spu) : spu_batch_deadline(h->spu);
#if FF8_MDEC_CALENDAR
        bound = fixed;
#else
        bound = fixed < h->mdec.due ? fixed : h->mdec.due;
#endif
        if (spu < bound) bound = spu;
        if (due >= bound) return;
        h->now = due;
#if FF8_MDEC_CALENDAR
        if (h->mdec.due == due) {
            mdec_event(&h->mdec,due); kick_channels(c,2);
            ++h->media_calendar.mdec_events;
#if defined(FF8_STATS)
            ++h->event_calls[HW_MDEC];
#endif
        }
#endif
#if !FF8_MDEC_CALENDAR
        spu_advance(h->spu, due);
#endif
        changed = 0;
    }
}

void dma_sync(cpu_t *c) {
    struct hw *h = c->hw;
    uint32_t address = h->dma[4].madr & 0x1ffffc;
    uint32_t step = h->dma[4].chcr & 2 ? (uint32_t)-4 : 4;
    if (!h->spu_dma.remaining) return;
    while (h->spu_dma.remaining && h->spu_dma.word_due <= h->now) {
        if (h->dma[4].chcr & 1) spu_dma_write(h->spu, mem_read32(c, address));
        else {
            mem_write32(c, address, spu_dma_read(h->spu));
            FRAME_TAGS(frame_tags_reload(c, address, 4));
            FRAME_ORACLE(frame_oracle_reload(c, address, 4));
        }
        address = (address + step) & 0x1ffffc;
        if (!--h->spu_dma.block_left && ((h->dma[4].chcr >> 9) & 3) == 1) {
            h->dma[4].bcr -= 0x10000;
            h->spu_dma.block_left = (h->dma[4].bcr & 0xffff) ? h->dma[4].bcr & 0xffff : 65536;
        }
        --h->spu_dma.remaining;
        h->spu_dma.word_due += 32;
    }
    h->dma[4].madr = address;
    if (h->spu_dma.remaining) h->dma[4].due = spu_due(c);
    else {
        h->dma[4].due = HW_NEVER;
        if (((h->dma[4].chcr >> 9) & 3) == 1) h->dma[4].bcr &= 0xffff;
        h->dma[4].chcr &= ~0x11000000u;
        ++h->dma[4].completions;
        if (h->dicr & (1u << 20)) h->dicr |= 1u << 28;
        irq(c); ++c->mmio_epoch;
    }
}

uint32_t dma_read(cpu_t *c, uint32_t address) {
    struct hw *h = c->hw;
    unsigned ch = (address - 0x1f801080) >> 4;
    if (address == 0x1f8010f0) return h->dpcr;
    if (address == 0x1f8010f4) return h->dicr;
    if (ch >= 7) return 0;
    switch (address & 15) {
    case 0: return h->dma[ch].madr;
    case 4: return h->dma[ch].bcr;
    case 8: return h->dma[ch].chcr;
    default: return 0;
    }
}
void dma_write(cpu_t *c, uint32_t address, uint32_t value, uint32_t mask) {
    struct hw *h = c->hw;
    unsigned ch = (address - 0x1f801080) >> 4;
    uint32_t *reg = NULL;
    if (address == 0x1f8010f0) reg = &h->dpcr;
    else if (address == 0x1f8010f4) {
        uint32_t control_mask = mask & 0x00ffffff;
        h->dicr = (h->dicr & ~control_mask) | (value & control_mask);
        h->dicr &= ~(value & mask & 0x7f000000);
        irq(c); return;
    } else if (ch < 7) {
        if ((address & 15) == 0) { reg = &h->dma[ch].madr; value &= 0xffffff; }
        else if ((address & 15) == 4) reg = &h->dma[ch].bcr;
        else if ((address & 15) == 8) reg = &h->dma[ch].chcr;
    }
    if (reg) *reg = (*reg & ~mask) | (value & mask);
    if (ch < 2 && (address & 15) == 8) h->media_dma[ch].remaining = h->media_dma[ch].grant = 0;
    if (ch == 4 && (address & 15) == 8) h->spu_dma.remaining = 0;
    if (ch < 7 && (address & 15) == 8 && !(h->dma[ch].chcr & 0x1000000)) h->dma[ch].due = HW_NEVER;
    dma_kick(c);
}

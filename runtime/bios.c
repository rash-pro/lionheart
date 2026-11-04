#include "hw.h"
#include "sio.h"
#include <string.h>
#include <stdio.h>

uint32_t bios_card_wait(cpu_t *c, unsigned slot) {
    bios_card_t *card;
    if (slot >= 2) return 0x11;
    card = &hw_get(c)->cards[slot];
    while (!(card->status & 1) && !c->halted) {
        c->cycles = c->next_event > c->cycles ? c->next_event : c->cycles + 2;
        hw_event(c); c_poll(c);
    }
    return card->status;
}

unsigned bios_card_begin(cpu_t *c, uint32_t port, unsigned operation, uint32_t sector,
                           uint32_t buffer, unsigned software) {
    struct hw *h = hw_get(c);
    bios_card_t *card;
    unsigned i;
    if ((port != 0 && port != 0x10) || sector >= 1024 || !h->card_started) return 0;
    card = &h->cards[port >> 4];
    if (!(card->status & 1)) return 0;
    card->operation = operation; card->sector = sector; card->buffer = buffer;
    card->software = software; card->allow_new = h->card_allow_new;
    h->card_allow_new = 0;
    card->status = (operation == BIOS_CARD_READ || operation == BIOS_CARD_FILE_READ) ? 2 :
                   (operation == BIOS_CARD_WRITE || operation == BIOS_CARD_FILE_WRITE) ? 4 : 8;
    if (operation == BIOS_CARD_WRITE)
        for (i = 0; i < 128; ++i) card->data[i] = mem_read8(c, buffer + i);
    /* The BIOS normally services one sector per two VBlanks, alternating slots. */
    card->due = c->cycles + HW_CPU_HZ / 30 *
        (operation == BIOS_CARD_LOAD && sio_card(c, port >> 4) ? 36 : 1);
    hw_reschedule(c);
    return 1;
}

void bios_card_event(cpu_t *c) {
    struct hw *h = c->hw;
    unsigned slot, i;
    for (slot = 0; slot < 2; ++slot) {
        bios_card_t *card = &h->cards[slot];
        memcard_t *storage;
        unsigned spec = 4;
        if (card->due == HW_NEVER || card->due > h->now) continue;
        card->due = HW_NEVER; storage = sio_card(c, slot);
        if (!storage) spec = 0x100;
        else if (memcard_failed(storage)) spec = 0x8000;
        else if ((memcard_flags(storage) & 8) && !card->allow_new &&
                 card->operation != BIOS_CARD_LOAD && card->operation != BIOS_CARD_ACK) spec = 0x2000;
        else if (card->operation == BIOS_CARD_READ) {
            if (!memcard_read_sector(storage, card->sector, card->data)) spec = 0x8000;
            else for (i = 0; i < 128; ++i) mem_write8(c, card->buffer + i, card->data[i]);
        } else if (card->operation == BIOS_CARD_WRITE) {
            if (!memcard_write_sector(storage, card->sector, card->data)) spec = 0x8000;
        } else if (card->operation == BIOS_CARD_FILE_READ || card->operation == BIOS_CARD_FILE_WRITE) {
            spec = bios_file_complete(c, slot);
        } else if (card->operation == BIOS_CARD_LOAD || card->operation == BIOS_CARD_ACK) {
            memcard_read_sector(storage, 0, card->data);
            if (card->operation == BIOS_CARD_LOAD && (card->data[0] != 'M' || card->data[1] != 'C')) {
                if (h->card_auto) spec = memcard_format(storage) ? 4 : 0x8000;
                else spec = 0x2000;
            }
            else if (!memcard_write_sector(storage, 63, card->data)) spec = 0x8000;
            if (spec == 4 && card->operation == BIOS_CARD_LOAD) {
                for (i = 0; i < 36; ++i) {
                    unsigned j, checksum = 0;
                    memcard_read_sector(storage, i, card->data);
                    for (j = 0; j < 128; ++j) checksum ^= card->data[j];
                    if (checksum) { spec = 0x8000; break; }
                }
            }
        }
        card->status = spec == 4 ? 1 : spec == 0x100 ? 0x11 : 0x21;
        if (card->operation == BIOS_CARD_FILE_READ || card->operation == BIOS_CARD_FILE_WRITE) {
            bios_file_t *file = &h->files[card->file];
            file->busy = 0; file->error = spec == 4 ? 0 : 5;
            if (file->error) h->file_errno = file->error;
            if (spec == 4) card->file_event = card->file;
        }
        card->spec = spec;
        if (card->software) card->software_spec = spec;
        h->card_channel = slot * 16;
        cpu_log(c, "CARD complete port=%02x op=%u status=%02x event=%04x cycle=%llu", slot * 16,
                card->operation, card->status, spec, (unsigned long long)h->now);
        ++c->mmio_epoch;
    }
}

int bios_card_pending(cpu_t *c) {
    return c->hw->cards[0].spec || c->hw->cards[1].spec ||
           c->hw->cards[0].software_spec || c->hw->cards[1].software_spec ||
           c->hw->cards[0].file_event || c->hw->cards[1].file_event;
}

void bios_card_deliver(cpu_t *c) {
    unsigned slot;
    for (slot = 0; slot < 2; ++slot) {
        bios_card_t *card = &c->hw->cards[slot];
        uint32_t spec = card->spec, software = card->software_spec, file = card->file_event;
        card->spec = card->software_spec = card->file_event = 0;
        if (spec || software) c->hw->card_channel = slot * 16;
        if (spec) bios_deliver(c, 0xf0000011u, spec);
        if (software) bios_deliver(c, 0xf4000001u, software);
        if (file) bios_deliver(c, file, 4);
    }
}

static uint32_t string_length(cpu_t *c, uint32_t p) {
    uint32_t n;
    for (n = 0; n < RAM_SIZE && !c->halted; ++n) if (!mem_read8(c, p + n)) return n;
    cpu_stop(c, "unterminated BIOS string at %08x", p);
    return 0;
}

static uint32_t allocate(cpu_t *c, uint32_t size) {
    struct hw *h = hw_get(c);
    unsigned i;
    if (!size || size > RAM_SIZE) return 0;
    size = (size + 7) & ~7u;
    for (i = 0; i < BIOS_HEAP_BLOCKS; ++i) {
        bios_heap_t *block = &h->heap[i];
        unsigned spare;
        if (block->used || block->size < size) continue;
        if (block->size > size) {
            for (spare = 0; spare < BIOS_HEAP_BLOCKS && h->heap[spare].size; ++spare) {}
            if (spare == BIOS_HEAP_BLOCKS) continue;
            h->heap[spare].address = block->address + size;
            h->heap[spare].size = block->size - size;
        }
        block->size = size; block->used = 1;
        return block->address;
    }
    return 0;
}

static void release(cpu_t *c, uint32_t address) {
    struct hw *h = hw_get(c);
    unsigned i, j;
    if (!address) return;
    for (i = 0; i < BIOS_HEAP_BLOCKS; ++i) if (h->heap[i].used && h->heap[i].address == address) break;
    if (i == BIOS_HEAP_BLOCKS) return;
    h->heap[i].used = 0;
    for (j = 0; j < BIOS_HEAP_BLOCKS; ++j) if (j != i && h->heap[j].size && !h->heap[j].used) {
        if (h->heap[j].address + h->heap[j].size == h->heap[i].address) {
            h->heap[i].address = h->heap[j].address; h->heap[i].size += h->heap[j].size; h->heap[j].size = 0;
            j = (unsigned)-1;
        } else if (h->heap[i].address + h->heap[i].size == h->heap[j].address) {
            h->heap[i].size += h->heap[j].size; h->heap[j].size = 0; j = (unsigned)-1;
        }
    }
}

static bios_event_t *event(cpu_t *c, uint32_t handle) {
    if ((handle & 0xffff0000u) != 0xf1000000u || (handle & 0xffff) >= BIOS_EVENTS) return NULL;
    return &hw_get(c)->events[handle & 0xffff];
}

void bios_thread_save(cpu_t *c, uint32_t tcb, uint32_t pc, uint32_t sr) {
    unsigned i;
    for (i = 0; i < 32; ++i) mem_write32(c, tcb + 8 + i * 4, c->r[i]);
    mem_write32(c, tcb + 0x88, pc);
    mem_write32(c, tcb + 0x8c, c->hi); mem_write32(c, tcb + 0x90, c->lo);
    mem_write32(c, tcb + 0x94, sr); mem_write32(c, tcb + 0x98, c->cop0[13]);
}

void bios_thread_restore(cpu_t *c, uint32_t tcb) {
    unsigned i;
    uint32_t sr = mem_read32(c, tcb + 0x94);
    for (i = 1; i < 32; ++i) c->r[i] = mem_read32(c, tcb + 8 + i * 4);
    c->r[0] = 0; c->pc = mem_read32(c, tcb + 0x88);
    c->hi = mem_read32(c, tcb + 0x8c); c->lo = mem_read32(c, tcb + 0x90);
    c_set_sr(c, (sr & ~15u) | ((sr >> 2) & 15));
}

static unsigned thread_index(cpu_t *c, uint32_t tcb) {
    uint32_t offset = tcb - mem_read32(c, 0x110);
    return offset % BIOS_TCB_SIZE ? BIOS_THREADS : offset / BIOS_TCB_SIZE;
}

void bios_thread_pipeline_save(cpu_t *c, uint32_t tcb, uint32_t pc) {
    unsigned index = thread_index(c, tcb);
    if (index >= BIOS_THREADS) return;
    c->hw->thread_pipeline[index].pc = pc;
#define SAVE(f) c->hw->thread_pipeline[index].f = c->f
    SAVE(load_reg); SAVE(load_value); SAVE(branch_target); SAVE(branch_pc); SAVE(branch_return);
    SAVE(branch_pending); SAVE(branch_kind);
#undef SAVE
}

void bios_thread_pipeline_restore(cpu_t *c, uint32_t tcb) {
    unsigned index = thread_index(c, tcb);
    c->load_reg = c->load_value = c->branch_pending = c->in_delay_slot = c->flow = 0;
    c->fault_counted = c->fault_instructions = 0;
    if (index >= BIOS_THREADS || c->hw->thread_pipeline[index].pc != c->pc) return;
#define RESTORE(f) c->f = c->hw->thread_pipeline[index].f
    RESTORE(load_reg); RESTORE(load_value); RESTORE(branch_target); RESTORE(branch_pc); RESTORE(branch_return);
    RESTORE(branch_pending); RESTORE(branch_kind);
#undef RESTORE
}

static uint32_t thread_address(cpu_t *c, uint32_t handle) {
    unsigned index = handle & 0xffff, count = mem_read32(c, 0x114) / BIOS_TCB_SIZE;
    if ((handle & 0xffff0000u) != 0xff000000u || index >= count || index >= BIOS_THREADS) return 0;
    return mem_read32(c, 0x110) + index * BIOS_TCB_SIZE;
}

static int change_thread(cpu_t *c, uint32_t next, uint32_t resume) {
    struct hw *h = hw_get(c);
    uint32_t pcb = mem_read32(c, 0x108), old = mem_read32(c, pcb);
    if (!next || mem_read32(c, next) != 0x4000) return 0;
    if (h->in_exception) { mem_write32(c, pcb, next); return 1; }
    cpu_log(c, "THREAD switch old=%08x next=%08x resume=%08x load=%u:%08x branch=%u target=%08x",
            old, next, resume, c->load_reg, c->load_value, c->branch_pending, c->branch_target);
    bios_thread_save(c, old, resume, (c->cop0[12] & ~63u) | ((c->cop0[12] << 2) & 63));
    bios_thread_pipeline_save(c, old, resume);
    mem_write32(c, old + 8 + 2 * 4, 1);
    mem_write32(c, pcb, next);
    bios_thread_restore(c, next);
    bios_thread_pipeline_restore(c, next);
    c_unwind(c);
    return 1;
}

static void tty_string(cpu_t *c, uint32_t p) {
    char text[384];
    uint32_t i, n = string_length(c, p);
    for (i = 0; i < n && i < sizeof(text) - 1; ++i) text[i] = (char)mem_read8(c, p + i);
    text[i] = 0;
    cpu_log(c, "TTY %s", text);
}

static uint32_t print_arg(cpu_t *c, unsigned n) {
    return n < 3 ? c->r[5 + n] : mem_read32(c, c->r[29] + 16 + (n - 3) * 4);
}

static void tty_printf(cpu_t *c, uint32_t p) {
    char out[384], part[128];
    unsigned len = 0, arg = 0, used = 0;
    while (used++ < 4096 && len < sizeof(out) - 1 && !c->halted) {
        unsigned ch = mem_read8(c, p++), j = 0;
        uint32_t value;
        if (!ch) break;
        if (ch != '%') { out[len++] = (char)ch; continue; }
        ch = mem_read8(c, p++);
        if (ch == '%') { out[len++] = '%'; continue; }
        while (ch == '-' || ch == '+' || ch == '#' || ch == '.' || (ch >= '0' && ch <= '9') || ch == 'l') {
            if (used++ >= 4096) break;
            ch = mem_read8(c, p++);
        }
        value = print_arg(c, arg++);
        if (ch == 's') {
            for (; j < sizeof(part) - 1; ++j) { part[j] = (char)mem_read8(c, value + j); if (!part[j]) break; }
            part[j] = 0;
        } else if (ch == 'c') { part[0] = (char)value; part[1] = 0; }
        else if (ch == 'd' || ch == 'i') snprintf(part, sizeof(part), "%d", (int32_t)value);
        else if (ch == 'u') snprintf(part, sizeof(part), "%u", value);
        else if (ch == 'x' || ch == 'X' || ch == 'p') snprintf(part, sizeof(part), "%x", value);
        else { cpu_stop(c, "unsupported BIOS printf conversion %c", ch); break; }
        for (j = 0; part[j] && len < sizeof(out) - 1; ++j) out[len++] = part[j];
    }
    out[len] = 0;
    cpu_log(c, "TTY %s", out);
    c->r[2] = len;
}

void bios_syscall(cpu_t *c) {
    uint32_t n = c->r[4];
#ifdef FF8_STATS
    ++c->bios_counts[3][n & 255];
#endif
    if (c->log) cpu_log(c, "BIOS SYS:%02x args=%08x,%08x,%08x,%08x pc=%08x", n,
            c->r[4], c->r[5], c->r[6], c->r[7], c->pc);
    switch (n) {
    case 0: break;
    /* HLE returns directly to the guest, so use IEc (bit 0), not the
     * exception handler's saved IEp (bit 2). Exit preserves the GPRs. */
    case 1: c->r[2] = (c->cop0[12] & 0x401) == 0x401; c_set_sr(c, c->cop0[12] & ~0x401u); break;
    case 2: c_set_sr(c, c->cop0[12] | 0x401); break;
    case 3: c->r[2] = change_thread(c, c->r[5], c->pc + 4); break;
    default: cpu_stop(c, "unimplemented BIOS syscall %08x", n); break;
    }
}

void bios_call(cpu_t *c, unsigned table) {
    uint32_t fn, a, b, n, d, s, i, value, count;
    struct hw *h = hw_get(c);
    bios_event_t *e;
    static const unsigned saved[] = {31, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23, 28};
    c_retire(c, 0, 0, 0, 0);
    fn = c->r[9]; a = c->r[4]; b = c->r[5]; n = c->r[6];
#ifdef FF8_STATS
    ++c->bios_counts[table][fn & 255];
#endif
    if (c->log) cpu_log(c, "BIOS %c0:%02x args=%08x,%08x,%08x,%08x ra=%08x", 'A' + table, fn, a, b, n, c->r[7], c->r[31]);
    if (fn > 255) goto unsupported;
    if (bios_file_call(c, table, fn)) return;
    if (table == 0) switch (fn) {
    case 0x0e: case 0x0f: c->r[2] = a >> 31 ? 0u - a : a; return;
    case 0x13:
        for (i = 0; i < sizeof(saved) / sizeof(saved[0]); ++i) mem_write32(c, a + 4 * i, c->r[saved[i]]);
        c->r[2] = 0; return;
    case 0x14:
        for (i = 0; i < sizeof(saved) / sizeof(saved[0]); ++i) c->r[saved[i]] = mem_read32(c, a + 4 * i);
        c->r[2] = b; return;
    case 0x15: case 0x16: case 0x19: case 0x1a:
        d = a; count = 0;
        if (fn == 0x15 || fn == 0x16) d += string_length(c, d);
        if (fn == 0x15 || fn == 0x19) n = RAM_SIZE;
        if (n > RAM_SIZE) { cpu_stop(c, "oversized BIOS string copy"); return; }
        for (i = 0; i < n && !c->halted; ++i) {
            value = count ? 0 : mem_read8(c, b + i);
            mem_write8(c, d + i, value);
            if (!value) { count = 1; if (fn != 0x1a) break; }
        }
        if (fn == 0x16) mem_write8(c, d + i, 0);
        c->r[2] = a; return;
    case 0x17: case 0x18:
        if (fn == 0x17) n = RAM_SIZE;
        for (i = 0; i < n && i < RAM_SIZE; ++i) {
            d = mem_read8(c, a + i); s = mem_read8(c, b + i);
            if (d != s || !d) { c->r[2] = d - s; return; }
        }
        c->r[2] = 0; return;
    case 0x1b: c->r[2] = string_length(c, a); return;
    case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        n = string_length(c, a); c->r[2] = 0;
        for (i = 0; i <= n; ++i) if (mem_read8(c, a + i) == (b & 255)) {
            c->r[2] = a + i; if (!(fn & 1)) break;
        }
        return;
    case 0x25: c->r[2] = a >= 'a' && a <= 'z' ? a - 32 : a; return;
    case 0x26: c->r[2] = a >= 'A' && a <= 'Z' ? a + 32 : a; return;
    case 0x27: case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e:
        if (fn == 0x28) n = b;
        if (n > RAM_SIZE) { cpu_stop(c, "oversized BIOS memory operation: %u", n); return; }
        d = fn == 0x27 ? b : a; s = fn == 0x27 ? a : b;
        c->r[2] = a;
        if (fn == 0x29 || fn == 0x2d || fn == 0x2e) c->r[2] = 0;
        for (i = 0; i < n && !c->halted; ++i) {
            uint32_t index = fn == 0x2c && d > s ? n - 1 - i : i;
            if (fn == 0x29 || fn == 0x2d) {
                value = mem_read8(c, a + i) - mem_read8(c, b + i);
                if (value) { c->r[2] = value; break; }
            } else if (fn == 0x2e) {
                if (mem_read8(c, a + i) == (b & 255)) { c->r[2] = a + i; break; }
            } else mem_write8(c, d + index, fn == 0x28 ? 0 : fn == 0x2b ? b : mem_read8(c, s + index));
        }
        return;
    case 0x2f: c->seed = c->seed * 0x41c64e6du + 12345; c->r[2] = (c->seed >> 16) & 32767; return;
    case 0x30: c->seed = a; return;
    case 0x33: c->r[2] = allocate(c, a); return;
    case 0x34: release(c, a); c->r[2] = 0; return;
    case 0x38:
        if (!a) { c->r[2] = allocate(c, b); return; }
        if (!b) { release(c, a); c->r[2] = 0; return; }
        for (i = 0; i < BIOS_HEAP_BLOCKS; ++i) if (h->heap[i].used && h->heap[i].address == a) break;
        if (i == BIOS_HEAP_BLOCKS) { c->r[2] = 0; return; }
        n = h->heap[i].size < b ? h->heap[i].size : b;
        d = allocate(c, b);
        if (d) { for (i = 0; i < n; ++i) mem_write8(c, d + i, mem_read8(c, a + i)); release(c, a); }
        c->r[2] = d; return;
    case 0x37:
        if (b && a > RAM_SIZE / b) { c->r[2] = 0; return; }
        n = a * b; d = allocate(c, n); c->r[2] = d;
        if (d) for (i = 0; i < n; ++i) mem_write8(c, d + i, 0);
        return;
    case 0x39:
        if (b > RAM_SIZE || (a & 0x1fffffffu) >= RAM_SIZE || b > RAM_SIZE - (a & 0x1fffffffu)) {
            cpu_stop(c, "invalid BIOS heap %08x+%08x", a, b); return;
        }
        c->heap_next = (a + 7) & ~7u; c->heap_end = a + b;
        memset(h->heap, 0, sizeof(h->heap));
        if (c->heap_next < c->heap_end) { h->heap[0].address = c->heap_next; h->heap[0].size = (c->heap_end - c->heap_next) & ~7u; }
        c->r[2] = a; return;
    case 0x3c: cpu_log(c, "TTY %c", (char)a); c->r[2] = a; return;
    case 0x3e: tty_string(c, a); c->r[2] = 0; return;
    case 0x3f: tty_printf(c, a); return;
    case 0x44: c->r[2] = 0; return;
    case 0x48: hw_write(c, 0x1f801814, 4, a); return;
    case 0x49: hw_write(c, 0x1f801810, 4, a); c->r[2] = 0; return;
    case 0x4d: c->r[2] = hw_read(c, 0x1f801814, 4); return;
    case 0x55: case 0x70:
        h->card_auto = 0;
        for (i = 0; i < 2; ++i) {
            bios_card_wait(c, i);
            if (bios_card_begin(c, i * 16, BIOS_CARD_LOAD, 0, 0, 0)) bios_card_wait(c, i);
        }
        c->r[2] = 0; return;
    case 0xab: c->r[2] = bios_card_begin(c, a, BIOS_CARD_INFO, 0, 0, 1); return;
    case 0xac: c->r[2] = bios_card_begin(c, a, BIOS_CARD_LOAD, 0, 0, 1); return;
    case 0xad: c->r[2] = h->card_auto; h->card_auto = a; return;
    case 0xaf: c->r[2] = bios_card_begin(c, a, BIOS_CARD_ACK, 63, 0, 0); return;
    case 0xa7: case 0xa8: case 0xa9: case 0xaa: case 0xae:
        bios_deliver(c, 0xf4000001u, fn == 0xa7 ? 4 : fn == 0xa9 ? 0x100 : fn == 0xaa ? 0x2000 : 0x8000);
        c->r[2] = 0; return;
    case 0x54: case 0x71: h->bios_cd = 1; c->r[2] = 1; return;
    case 0x56: case 0x72: h->bios_cd = 0; c->r[2] = 0; return;
    case 0x9f: mem_write32(c, 0x60, a); c->r[2] = a; return;
    case 0xb4:
        if (a == 0) c->r[2] = 0x19951204;
        else if (a == 5) c->r[2] = 2048;
        else if (a == 7) c->r[2] = 1024;
        else if (a == 9) c->r[2] = 512;
        else if (a == 1) c->r[2] = 3;
        else goto unsupported;
        return;
    default: break;
    }
    if (table == 1) switch (fn) {
    case 0x07: bios_deliver(c, a, b); return;
    case 0x08:
        for (i = 0; i < BIOS_EVENTS && h->events[i].state; ++i) {}
        if (i == BIOS_EVENTS) { c->r[2] = ~0u; return; }
        e = &h->events[i]; e->class_id = a; e->spec = b; e->mode = n; e->handler = c->r[7]; e->state = 1;
        c->r[2] = 0xf1000000u | i; return;
    case 0x09: case 0x0c: case 0x0d:
        e = event(c, a);
        if (e && e->state) e->state = fn == 9 ? 0 : fn == 0xc ? 2 : 1;
        c->r[2] = 1; return;
    case 0x0a: case 0x0b:
        e = event(c, a);
        while (fn == 0xa && e && e->state == 2 && !c->halted) {
            c->cycles = c->next_event > c->cycles ? c->next_event : c->cycles + 2;
            ++c->interp_instructions;
            if (c->overlay_profile) c_profile_count(c, UINT32_MAX, 1, 0);
            hw_event(c); c_poll(c);
        }
        c->r[2] = e && e->state == 3;
        if (c->r[2]) e->state = 2;
        return;
    case 0x20:
        for (i = 0; i < BIOS_EVENTS; ++i) {
            e = &h->events[i];
            if (e->state == 3 && e->mode == 0x2000 && e->class_id == a && e->spec == b) e->state = 2;
        }
        return;
    case 0x0e:
        count = mem_read32(c, 0x114) / BIOS_TCB_SIZE;
        d = mem_read32(c, 0x110);
        for (i = 0; i < count && i < BIOS_THREADS && mem_read32(c, d + i * BIOS_TCB_SIZE) != 0x1000; ++i) {}
        if (i == count || i == BIOS_THREADS) { c->r[2] = ~0u; return; }
        d += i * BIOS_TCB_SIZE;
        memset(&h->thread_pipeline[i], 0, sizeof(h->thread_pipeline[i]));
        mem_write32(c, d, 0x4000); mem_write32(c, d + 4, 0x1000);
        mem_write32(c, d + 0x88, a);
        mem_write32(c, d + 8 + 29 * 4, b); mem_write32(c, d + 8 + 30 * 4, b);
        mem_write32(c, d + 8 + 28 * 4, n);
        c->r[2] = 0xff000000u | i; return;
    case 0x0f:
        d = thread_address(c, a); c->r[2] = !!d;
        if (d) mem_write32(c, d, 0x1000);
        return;
    case 0x10: c->r[2] = change_thread(c, thread_address(c, a), c->r[31]); return;
    case 0x12:
        h->pad_buffer[0] = a; h->pad_size[0] = b; h->pad_buffer[1] = n; h->pad_size[1] = c->r[7];
        c->r[2] = 1; return;
    case 0x13: h->pad_started = 1; h->i_mask |= 1; c_set_sr(c, c->cop0[12] | 0x401); c->r[2] = 1; return;
    case 0x14: h->pad_started = 0; c->r[2] = 1; return;
    case 0x17:
        if (h->in_exception) longjmp(h->exception_return, 1);
        cpu_stop(c, "ReturnFromException outside an exception"); return;
    case 0x18: h->exit_hook = 0; c->r[2] = 0; return;
    case 0x19: h->exit_hook = a; c->r[2] = 0; return;
    case 0x02:
        i = a & 3; if (i == 3) { c->r[2] = 0; return; }
        value = 0;
        if (n & 0x10) value |= 0x50;
        if (!(n & 0x1000)) value |= 8;
        if (!(n & 0x100)) value |= i == 2 ? 0x200 : 0x100;
        hw_write(c, 0x1f801104 + i * 16, 2, value); hw_write(c, 0x1f801108 + i * 16, 2, b);
        c->r[2] = 1; return;
    case 0x03: c->r[2] = (a & 3) == 3 ? (uint32_t)h->vblanks : hw_read(c, 0x1f801100 + (a & 3) * 16, 2); return;
    case 0x04: case 0x05:
        i = (a & 3) == 3 ? 0 : (a & 3) + 4;
        if (fn == 4) h->i_mask |= 1u << i; else h->i_mask &= ~(1u << i);
        hw_reschedule(c); c->r[2] = 1; return;
    case 0x06:
        if ((a & 3) < 3) { hw_write(c, 0x1f801104 + (a & 3) * 16, 2, 0); hw_write(c, 0x1f801108 + (a & 3) * 16, 2, 0); }
        c->r[2] = 1; return;
    case 0x3d: cpu_log(c, "TTY %c", (char)a); c->r[2] = a; return;
    case 0x3f: tty_string(c, a); c->r[2] = 0; return;
    case 0x4a:
        h->card_started = 0; h->card_allow_new = 0;
        memset(h->cards, 0, sizeof(h->cards));
        memset(h->files, 0, sizeof(h->files));
        for (i = 0; i < 2; ++i) { h->cards[i].status = 1; h->cards[i].due = HW_NEVER; }
        hw_reschedule(c); c->r[2] = 0; return;
    case 0x4b: h->card_started = 1; c->r[2] = 0; return;
    case 0x4c:
        h->card_started = 0;
        for (i = 0; i < 2; ++i) { h->cards[i].due = HW_NEVER; h->cards[i].status = 1; }
        for (i = 2; i < 16; ++i) if (h->files[i].busy) { h->files[i].busy = 0; h->files[i].error = 5; }
        hw_reschedule(c); c->r[2] = 0; return;
    case 0x4d: c->r[2] = bios_card_begin(c, a, BIOS_CARD_INFO, 0, 0, 0); return;
    case 0x4e: case 0x4f:
        c->r[2] = bios_card_begin(c, a, fn == 0x4e ? BIOS_CARD_WRITE : BIOS_CARD_READ, b, n, 0); return;
    case 0x50: h->card_allow_new = 1; c->r[2] = 0; return;
    case 0x58: c->r[2] = h->card_channel; return;
    case 0x5c: hw_sync(c); c->r[2] = a < 2 ? h->cards[a].status : 0x11; return;
    case 0x5d: c->r[2] = bios_card_wait(c, a); return;
    case 0x56: c->r[2] = 0x80001000u; return;
    case 0x57: c->r[2] = 0x80001400u; return;
    case 0x5b: c->r[2] = c->pad_clear; c->pad_clear = a; return;
    default: break;
    }
    if (table == 2) switch (fn) {
    case 0x00: for (i = 0; i < 4; ++i) h->rcnt_ack[i] = 1; c->r[2] = 1; return;
    case 0x01: case 0x07: case 0x0c: c->r[2] = 1; return;
    case 0x02:
        if (a >= 4 || !b) { c->r[2] = ~0u; return; }
        d = mem_read32(c, 0x100) + a * 8;
        mem_write32(c, b, mem_read32(c, d)); mem_write32(c, d, b); c->r[2] = 0; return;
    case 0x03:
        if (a >= 4) { c->r[2] = ~0u; return; }
        n = mem_read32(c, 0x100) + a * 8;
        d = mem_read32(c, n); s = 0;
        for (i = 0; d && i < 256; ++i) {
            if (d == b) {
                if (s) mem_write32(c, s, mem_read32(c, d)); else mem_write32(c, n, mem_read32(c, d));
                c->r[2] = 0; return;
            }
            s = d; d = mem_read32(c, d);
        }
        c->r[2] = ~0u; return;
    case 0x0a: i = a & 3; c->r[2] = h->rcnt_ack[i]; h->rcnt_ack[i] = b; return;
    case 0x0d:
        if (a >= 11) { c->r[2] = 0; return; }
        c->r[2] = !!(h->auto_ack & (1u << a));
        if (b) h->auto_ack |= 1u << a; else h->auto_ack &= ~(1u << a);
        return;
    default: break;
    }
unsupported:
    cpu_stop(c, "unimplemented BIOS %c0:%02x args=%08x,%08x,%08x,%08x", 'A' + table, fn, a, b, n, c->r[7]);
}

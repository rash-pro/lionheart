#ifndef FF8_HW_H
#define FF8_HW_H

#include "cpu.h"
#include <stdio.h>
#include <setjmp.h>

#define HW_CPU_HZ 33868800u
#define HW_NEVER UINT64_MAX
#ifndef FF8_MDEC_CALENDAR
#define FF8_MDEC_CALENDAR 1
#endif
#define BIOS_EVENTS 256
#define BIOS_HEAP_BLOCKS 1024
#define BIOS_EXCB_BASE 0x8000e000u
#define BIOS_PCB_BASE 0x8000e020u
#define BIOS_TCB_BASE 0x8000e100u
#define BIOS_TCB_SIZE 0xc0u
#define BIOS_THREADS 16u
#define BIOS_DCB_BASE 0x80003000u
#define BIOS_FCB_BASE 0x80003800u
#define BIOS_CARD_FIRSTFILE 0x80003500u

#include "gpu.h"
#include "mdec.h"
#include "spu.h"

typedef struct {
    uint16_t count, mode, target;
    unsigned fired, released;
    uint64_t fraction, cycle;
} hw_timer_t;

typedef struct { uint32_t class_id, spec, mode, handler; unsigned state; } bios_event_t;
typedef struct { uint32_t address, size; unsigned used; } bios_heap_t;
enum {
    BIOS_CARD_READ = 2, BIOS_CARD_WRITE = 4, BIOS_CARD_INFO = 8,
    BIOS_CARD_LOAD = 16, BIOS_CARD_ACK = 32, BIOS_CARD_FILE_READ = 64, BIOS_CARD_FILE_WRITE = 128
};
typedef struct {
    uint64_t due;
    uint32_t status, sector, buffer, spec, software_spec;
    uint32_t file_event, length, file;
    unsigned operation, allow_new, software;
    uint8_t data[128];
} bios_card_t;
typedef struct { unsigned used, slot, block, mode, busy, error; uint32_t offset, size; } bios_file_t;

typedef struct {
    FILE *file;
    uint32_t sectors, offset, img_lba, img_count;
    uint32_t *lba_reads, *lba_dma;
    struct { uint32_t lba, size; } img[256];
    uint8_t index, params[16], response[16], command_params[16];
    unsigned param_count, response_pos, response_count, command_count;
    uint8_t command, second, irq_flags, irq_enable, mode, status, filter_file, filter_channel, muted;
    uint8_t sector[2352], data[2352], header[8];
    unsigned data_pos, data_size, sector_size, sector_valid, request, reading, playing, location_pending;
    uint32_t location, read_lba, sector_lba, data_lba;
    uint64_t command_due, second_due, read_due, commands[256], interrupts[6], sectors_read, bytes_dma, overruns;
    uint8_t volume[4], pending_volume[4], xa_muted, xa_file, xa_channel, xa_active;
    int16_t xa_history[2][2], xa_ring[2][32];
    unsigned xa_ring_pos, xa_sixstep;
    uint64_t xa_sectors, xa_frames, xa_filtered;
    uint8_t sector_buffer[2352];
    uint32_t buffer_lba;
    unsigned shell_open, lid_irq;
    uint64_t lid_due;
} hw_cd_t;

enum { HW_VIDEO, HW_TIMER, HW_SPU, HW_MDEC, HW_CD, HW_SIO, HW_CARD, HW_DMA, HW_EVENTS };

struct hw {
    uint64_t deadline[HW_EVENTS], next_device, media_bound;
#if defined(FF8_STATS)
    uint64_t event_calls[HW_EVENTS], sync_calls, reschedules;
#endif
#ifdef FF8_STATS
    uint16_t mmio_hint[1024];
#else
    struct { uint32_t key, read, write, seen; } mmio_progress[1024];
#endif
    gpu_t *gpu;
    struct spu *spu;
    mdec_t mdec;
    struct { uint32_t remaining, block_left, grant; } media_dma[2];
    struct {
        uint64_t spans, words[2], exact_words[2], parser, boundary, layout, mdec_events;
    } media_calendar;
    struct { uint32_t remaining, block_left; uint64_t word_due; } spu_dma;
    uint32_t i_stat, i_mask, dpcr, dicr;
    hw_timer_t timers[3];
    struct { uint32_t madr, bcr, chcr; uint64_t due, completions; } dma[7];
    hw_cd_t cd;
    struct { uint16_t mode, control, baud; uint8_t rx, step; unsigned ready, ack, irq; uint64_t due, ack_due; } sio;
    uint64_t now, next_video, video_edge, video_start, vblanks, irq_raised[11], irq_delivered[11];
    unsigned line, hblank, vblank, video_pal, syncing, in_exception, dma_burst;
    /* hw_interrupt_window: the first VBlank edge at or after video_edge and its
     * time, valid while video_edge <= vblank_edge for this start and standard. */
    uint64_t vblank_edge, vblank_time, vblank_start;
    unsigned vblank_pal, vblank_valid;
    /* timers_next() depends only on timer, blank and GPU dot-clock state; writers set timers_dirty. */
    uint64_t timer_due;
    unsigned timers_dirty;
    /* Guest RAM range MDEC DMA may still touch while the CPU runs ahead of it. */
    uint32_t fence_lo[2], fence_hi[2];
    void (*present)(cpu_t *, void *);
    void *present_context;
    bios_event_t events[BIOS_EVENTS];
    bios_heap_t heap[BIOS_HEAP_BLOCKS];
    struct {
        uint32_t pc, load_reg, load_value, branch_target, branch_pc, branch_return;
        unsigned branch_pending, branch_kind;
    } thread_pipeline[BIOS_THREADS];
    uint32_t exit_hook, auto_ack, rcnt_ack[4], pad_buffer[2], pad_size[2];
    unsigned pad_started, card_started, card_auto, bios_cd;
    bios_card_t cards[2];
    unsigned card_channel, card_allow_new;
    bios_file_t files[16];
    unsigned file_errno, find_slot, find_index;
    char find_pattern[21];
    jmp_buf exception_return;
};

struct hw *hw_get(cpu_t *);
void hw_destroy(cpu_t *);
void hw_event(cpu_t *);
void hw_sync(cpu_t *);
void hw_reschedule(cpu_t *);
void hw_irq_raise(cpu_t *, unsigned);
void hw_summary(cpu_t *, FILE *);
void timers_advance(cpu_t *, uint64_t);
void timers_blank(cpu_t *, unsigned, unsigned);
uint64_t timers_next(cpu_t *);
int timers_observe_hblank(cpu_t *);
uint32_t timers_read(cpu_t *, uint32_t);
void timers_write(cpu_t *, uint32_t, uint32_t);
uint32_t dma_read(cpu_t *, uint32_t);
void dma_write(cpu_t *, uint32_t, uint32_t, uint32_t);
void dma_event(cpu_t *);
void dma_sync(cpu_t *);
void dma_kick(cpu_t *);
int cdrom_open(cpu_t *, const char *, char *, size_t);
int cdrom_swap(cpu_t *, const char *, char *, size_t);
uint8_t cdrom_read(cpu_t *, unsigned);
void cdrom_write(cpu_t *, unsigned, uint8_t);
void cdrom_event(cpu_t *);
uint32_t cdrom_dma(cpu_t *);
void cdrom_audio_init(hw_cd_t *);
unsigned cdrom_decode_xa(hw_cd_t *, const uint8_t *sector, int16_t *stereo);
uint32_t sio_read(cpu_t *, uint32_t);
void sio_write(cpu_t *, uint32_t, uint32_t);
void sio_event(cpu_t *);
void bios_exception(cpu_t *, uint32_t);
void bios_deliver(cpu_t *, uint32_t, uint32_t);
uint32_t bios_guest_call(cpu_t *, uint32_t);
void bios_thread_save(cpu_t *, uint32_t tcb, uint32_t pc, uint32_t sr);
void bios_thread_restore(cpu_t *, uint32_t tcb);
void bios_thread_pipeline_save(cpu_t *, uint32_t tcb, uint32_t pc);
void bios_thread_pipeline_restore(cpu_t *, uint32_t tcb);
void bios_pad_vblank(cpu_t *);
void bios_card_event(cpu_t *);
int bios_card_pending(cpu_t *);
void bios_card_deliver(cpu_t *);
unsigned bios_card_begin(cpu_t *, uint32_t port, unsigned operation, uint32_t sector, uint32_t buffer, unsigned software);
uint32_t bios_card_wait(cpu_t *, unsigned slot);
int bios_file_call(cpu_t *, unsigned table, unsigned function);
unsigned bios_file_complete(cpu_t *, unsigned slot);

#endif

#include "savestate.h"
#include "savestate_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

const unsigned generated_host_precise = 1;
static savestate_identity_t identity;
static const char *state_path;
static unsigned callbacks, receipts;
static void callback(cpu_t *c) {
    ++callbacks; savestate_request(c);
    assert(!savestate_ready(c));
    c->r[2] = 77; c->pc = c->r[31];
}
static void caller(cpu_t *c) {
    assert(bios_guest_call(c,0x80011000) == 77);
    assert(!receipts);
    c->hw->events[0] = (bios_event_t){0xf0000001,0x1000,0x1000,0x80011000,2};
    c->hw->i_stat = c->hw->i_mask = c->hw->auto_ack = 1;
    c->hw->rcnt_ack[3] = 0;
    bios_exception(c,0x400);
    assert(callbacks == 2 && !receipts && !c->hw->in_exception);
    c->pc = 0x80012000;
}
static int receipt(cpu_t *c, void *unused) {
    char error[256];
    (void)unused;
    assert(callbacks && c->dispatch_depth == 1 && !c->hw->in_exception && !c->hw->syncing && !c->native_pin_count);
    ++receipts;
    assert(savestate_write(c,state_path,&identity,error,sizeof(error)));
    return 1;
}
static cpu_t *new_cpu(void) {
    cpu_t *c = malloc(sizeof(*c)); assert(c); cpu_init(c); hw_get(c); sio_init(c);
    c->instruction_limit = 0; c->stall_limit = 0; return c;
}
int main(int argc, char **argv) {
    cpu_t *a = new_cpu(), *b;
    char error[256];
    unsigned i;
    assert(argc >= 3); state_path = argv[2];
    if (!strcmp(argv[1],"read")) {
        a->r[5] = 0x12345678;
        b = savestate_read(a,state_path,&identity,NULL,NULL,error,sizeof(error));
        if (!b) { printf("rejected: %s\n",error); assert(a->r[5] == 0x12345678); }
        savestate_dispose(b); savestate_dispose(a); return b ? 0 : 2;
    }
    /* Device 3 is the legitimate latched "no device" transaction. */
    hw_write(a,0x1f801048,2,0xd); hw_write(a,0x1f80104e,2,136);
    hw_write(a,0x1f80104a,2,0x1003); hw_write(a,0x1f801040,1,2);
    a->cycles += 2000; hw_event(a); assert(hw_read(a,0x1f801040,1) == 0xff);
    a->pc = 0x80010000;
    /* lw t0,0(a0); beq zero,zero,-2; addiu t1,t1,1 (delay slot). */
    mem_write32(a,a->pc,0x8c880000); mem_write32(a,a->pc+4,!strcmp(argv[1],"untaken") ? 0x1400fffe : 0x1000fffe); mem_write32(a,a->pc+8,0x25290001);
    a->r[4] = 0x80014000; mem_write32(a,a->r[4],0xdeadbeef);
    gpu_gp1(a->hw->gpu,0x03000000); gpu_gp0(a->hw->gpu,0xa0000000); gpu_gp0(a->hw->gpu,0x00010001); gpu_gp0(a->hw->gpu,0x00020002);
    gpu_gp0(a->hw->gpu,0x12345678);
    /* Outstanding decoder/DMA, timer and asynchronous BIOS card state. */
    mdec_write(&a->hw->mdec,0x40000001,0); a->hw->spu_dma.remaining = 12; a->hw->spu_dma.word_due = 123;
    a->hw->timers[2].fraction = 7; a->hw->cards[0].operation = BIOS_CARD_WRITE; a->hw->cards[0].due = 99;
    if (argc > 3) {
        unsigned ack;
        assert(sio_open_card(a,0,argv[3],error,sizeof(error)));
        for (i = 0; i < 140; ++i) (void)memcard_exchange(sio_card(a,0),i == 0 ? 0x81 : i == 1 ? 'R' : 0,&ack);
    }
    interp_step(a); assert(a->load_reg == 8);
    assert(savestate_write(a,state_path,&identity,error,sizeof(error)));
    b = savestate_read(a,state_path,&identity,NULL,NULL,error,sizeof(error));
    assert(b && b->load_reg == 8 && b->load_value == 0xdeadbeef);
    interp_step(b); interp_step(a);
    assert(!memcmp(a->r,b->r,sizeof(a->r))); savestate_dispose(b);
    assert(a->branch_pending && !a->fault_instructions);
    assert(savestate_write(a,state_path,&identity,error,sizeof(error)));
    b = savestate_read(a,state_path,&identity,NULL,NULL,error,sizeof(error));
    if (!b) fprintf(stderr,"%s\n",error);
    assert(b && b->branch_pending && b->hw->cards[0].operation == BIOS_CARD_WRITE && b->hw->spu_dma.remaining == 12);
    /* Isolate the instruction pipeline from deliberately synthetic device deadlines. */
    a->next_event = b->next_event = UINT64_MAX;
    for (i = 0; i < 1000; ++i) { interp_step(a); interp_step(b); }
    assert(!memcmp(a->r,b->r,sizeof(a->r)) && a->pc == b->pc && a->cycles == b->cycles);
    assert(!memcmp(a->ram,b->ram,RAM_SIZE));
    if (argc > 3) {
        uint8_t sector[128]; memset(sector,0xaa,sizeof(sector));
        unsigned ack;
        memcard_reset(sio_card(a,0));
        for (i = 0; i < 70; ++i) (void)memcard_exchange(sio_card(a,0),
            i == 0 ? 0x81 : i == 1 ? 'W' : i == 5 ? 100 : i >= 6 ? 0xaa : 0,&ack);
        assert(savestate_write(a,state_path,&identity,error,sizeof(error)));
        savestate_dispose(b); b = savestate_read(a,state_path,&identity,NULL,NULL,error,sizeof(error));
        assert(b);
        for (; i <= 137; ++i) {
            uint8_t result = memcard_exchange(sio_card(b,0),i < 134 ? 0xaa : i == 134 ? 100 : 0,&ack);
            if (i == 137) assert(result == 0x47);
        }
        assert(memcard_read_sector(sio_card(b,0),100,sector) && sector[0] == 0xaa && sector[127] == 0xaa);
        assert(memcard_read_sector(sio_card(a,0),100,sector)); assert(sector[0] != 0xaa);
    }
    savestate_dispose(a); savestate_dispose(b);
    a = new_cpu(); a->pc = 0x80010000;
    {
        static const entry_t entries[] = {{0x80010000,caller},{0x80011000,callback}};
        a->entries = entries; a->entry_count = 2;
        savestate_handler(receipt,NULL); c_enter(a,a->pc,0xffffffff); dispatch(a,a->pc);
        assert(receipts == 1 && callbacks == 2 && !a->halted);
    }
    savestate_dispose(a);
    puts("pipeline, devices, private cards, callback deferral and root suspension: OK"); return 0;
}

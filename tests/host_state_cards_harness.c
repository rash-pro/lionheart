/* Host restoration policy through guest SIO and BIOS file writes. Synthetic cards only. */
#include "states.h"
#include "savestate_internal.h"
#include "state_build.h"
#include "display.h"
#include "osd.h"
#include "audio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned last_osd;
void __real_host_osd_event(unsigned,unsigned,const char *);
void __wrap_host_osd_event(unsigned event,unsigned number,const char *detail) {
    last_osd=event; __real_host_osd_event(event,number,detail);
}
static cpu_t *c;
static uint32_t call(unsigned table,unsigned fn,uint32_t a,uint32_t b,uint32_t n) {
    c->r[9]=fn; c->r[4]=a; c->r[5]=b; c->r[6]=n; bios_call(c,table); return c->r[2];
}
static uint32_t text(const char *s) { memcpy(c->ram+0x10000,s,strlen(s)+1); return 0x10000; }
static uint32_t event(unsigned spec) {
    c->r[7]=0; uint32_t id=call(1,8,0xf0000011,spec,0x2000); assert(id!=~0u);
    assert(call(1,0xc,id,0,0)); return id;
}
static void select_card(void) {
    hw_write(c,0x1f80104a,2,0); hw_write(c,0x1f801048,2,0xd);
    hw_write(c,0x1f80104e,2,136); hw_write(c,0x1f80104a,2,0x1003);
}
static unsigned receive(void) { c->cycles+=2000; hw_event(c); return hw_read(c,0x1f801040,1)&255; }
static unsigned send(unsigned byte) { hw_write(c,0x1f801040,1,byte); return receive(); }
static unsigned write_byte(unsigned i,unsigned value) {
    return i==0 ? 0x81 : i==1 ? 0x57 : i==5 ? 100 : i>=6 && i<134 ? value : i==134 ? 100 : 0;
}
static unsigned disk(const char *path,unsigned sector) {
    FILE *f=fopen(path,"rb"); assert(f && !fseek(f,(long)sector*128,SEEK_SET));
    int value=fgetc(f); assert(value!=EOF); assert(!fclose(f)); return (unsigned)value;
}
static void sector(unsigned slot,unsigned value) {
    uint8_t data[128]; memset(data,value,sizeof(data)); assert(memcard_write_sector(sio_card(c,slot),100,data));
}
static void bios_save(unsigned slot,unsigned value) {
    char name[32]; snprintf(name,sizeof(name),"bu%u0:FF8TEST",slot);
    call(1,0x4a,1,0,0); call(1,0x4b,0,0,0);
    unsigned fd=call(0,0,text(name),0x10203,0); assert(fd!=~0u);
    memset(c->ram+0x12000,value,128); assert(call(0,3,fd,0x12000,128)==128);
    assert(call(0,4,fd,0,0)==fd);
}
int main(int argc,char **argv) {
    assert(argc==4);
    const char *kind=argv[1],*dir=argv[3]; int snapshot=!strcmp(argv[2],"snapshot");
    char card[2][4096],state[4096],error[256];
    uint8_t data[128]; unsigned i,done=0,changed=0;
    c=malloc(sizeof(*c)); assert(c); cpu_init(c); hw_get(c); sio_init(c); c->pc=0x80010000;
    assert(host_display_init(1,NULL,NULL) && host_audio_init(1,1,NULL));
    for (i=0;i<2;++i) {
        snprintf(card[i],sizeof(card[i]),"%s/card%u.mcr",dir,i+1);
        assert(sio_open_card(c,i,card[i],error,sizeof(error))); sector(i,0x11+i);
    }
    snprintf(state,sizeof(state),"%s/test.ff8s",dir);
    int wire=!strcmp(kind,"wire"),bios=!strncmp(kind,"bios-",5);
    if (wire) {
        select_card(); for (i=0;i<70;++i) send(write_byte(i,0xaa));
        hw_write(c,0x1f801040,1,0xaa); /* One byte is in flight at serialization. */
    }
    if (bios) {
        call(1,0x4a,1,0,0); call(1,0x4b,0,0,0);
        unsigned fd=0;
        if (!strcmp(kind,"bios-file")) {
            fd=call(0,0,text("bu00:ASYNC"),0x18203,0); assert(fd==2);
        }
        done=event(4); changed=event(0x2000);
        memset(c->ram+0x12000,0xaa,128); c->hw->card_allow_new=1;
        if (fd) {
            assert(call(0,3,fd,0x12000,128)==0 && c->hw->files[fd].busy);
        } else {
            unsigned op=!strcmp(kind,"bios-load") ? BIOS_CARD_LOAD : !strcmp(kind,"bios-ack") ? BIOS_CARD_ACK : BIOS_CARD_WRITE;
            assert(bios_card_begin(c,0,op,100,0x12000,1));
        }
    }
    assert(host_states_init(c,dir,card[0],NULL,NULL,0,NULL,0,0,0,0,snapshot));
    savestate_request(c); assert(savestate_ready(c)); savestate_cancel(c);
    /* Even the safe root boundary can contain an unfinished card operation. */
    savestate_identity_t id={0}; memcpy(id.build,FF8_STATE_BUILD,64);
    assert(savestate_write(c,state,&id,error,sizeof(error))); host_states_shutdown();
    int same=!strcmp(kind,"equal");
    if (!same) { sector(0,0xcc); sector(1,0xdd); }
    if (!strcmp(kind,"absent")) { sio_destroy(c); sio_init(c); }
    if (!strcmp(kind,"failed")) {
        FILE *f=fopen(state,"r+b"); assert(f && !fseek(f,20,SEEK_SET));
        int value=fgetc(f); assert(!fseek(f,20,SEEK_SET)); fputc(value^255,f); assert(!fclose(f));
    }
    memcard_t *attached[2]={sio_card(c,0),sio_card(c,1)};
    unsigned file_before=disk(card[0],64);
    cpu_t *old=c;
    assert(host_states_init(c,dir,card[0],state,NULL,0,NULL,0,0,0,0,snapshot));
    assert(host_states_apply(&c,NULL));
    assert(last_osd==(!strcmp(kind,"failed") ? HOST_OSD_LOAD_FAILED : !snapshot && !same ? HOST_OSD_CARD : HOST_OSD_LOADED));
    if (!strcmp(kind,"failed")) {
        assert(c==old && host_states_failed()); assert(memcard_read_sector(sio_card(c,0),100,data) && data[0]==0xcc);
    } else {
        assert(c!=old && !host_states_failed());
        for (i=0;i<2;++i) {
            if (!snapshot) assert(sio_card(c,i)==attached[i]);
            if (!snapshot && !strcmp(kind,"absent")) { assert(!sio_card(c,i)); continue; }
            assert(memcard_read_sector(sio_card(c,i),100,data));
            assert(data[0]==(snapshot || same ? 0x11+i : 0xcc+i*0x11));
        }
    }
    if (wire) {
        assert(receive()==(snapshot ? 0xaa : 0xff));
        unsigned reply=0;
        for (i=71;i<138;++i) reply=send(write_byte(i,0xaa));
        assert(reply==(snapshot ? 0x47u : 0xffu));
        assert(disk(card[0],100)==0xcc);
        assert(memcard_read_sector(sio_card(c,0),100,data) && data[0]==(snapshot ? 0xaa : 0xcc));
        select_card();
        for (i=0;i<138;++i) reply=send(write_byte(i,0xee));
        assert(reply==0x47 && disk(card[0],100)==(snapshot ? 0xccu : 0xeeu));
    } else if (bios) {
        if (snapshot) {
            assert(c->hw->cards[0].due!=HW_NEVER);
            if (!strcmp(kind,"bios-file")) {
                assert(bios_card_wait(c,0)==1);
                assert(memcard_read_sector(sio_card(c,0),64,data) && data[0]==0xaa);
                assert(disk(card[0],64)==file_before && file_before!=0xaa);
            }
        } else {
            assert(c->hw->cards[0].due==HW_NEVER && bios_card_wait(c,0)==0x21);
            if (!strcmp(kind,"bios-file")) {
                assert(!c->hw->files[2].busy && c->hw->files[2].error==5);
                assert(disk(card[0],64)==file_before && file_before!=0xaa);
            }
            c_set_sr(c,c->cop0[12]|0x401); hw_event(c);
            assert(call(1,0xb,changed,0,0)==1 && call(1,0xb,done,0,0)==0);
            assert(disk(card[0],100)==0xcc);
            c->cycles+=2ull*HW_CPU_HZ; hw_event(c); assert(disk(card[0],100)==0xcc);
            if (!strcmp(kind,"bios-file")) assert(disk(card[0],64)==file_before);
        }
    } else if (strcmp(kind,"absent")) {
        for (i=0;i<2;++i) {
            unsigned before=disk(card[i],64); bios_save(i,0x5a+i);
            assert(disk(card[i],64)==(snapshot && strcmp(kind,"failed") ? before : 0x5au+i));
        }
    }
    assert(host_audio_shutdown(c->hw->spu) && host_display_shutdown(c->hw->gpu));
    host_states_shutdown(); savestate_dispose(c);
    puts("Cards: restored storage, guest writes, disk persistence, and I/O recovery: PASS"); return 0;
}

#ifndef FF8_SIO_H
#define FF8_SIO_H

#include "cpu.h"

enum {
    PAD_SELECT = 1u << 0, PAD_L3 = 1u << 1, PAD_R3 = 1u << 2, PAD_START = 1u << 3,
    PAD_UP = 1u << 4, PAD_RIGHT = 1u << 5, PAD_DOWN = 1u << 6, PAD_LEFT = 1u << 7,
    PAD_L2 = 1u << 8, PAD_R2 = 1u << 9, PAD_L1 = 1u << 10, PAD_R1 = 1u << 11,
    PAD_TRIANGLE = 1u << 12, PAD_CIRCLE = 1u << 13, PAD_CROSS = 1u << 14, PAD_SQUARE = 1u << 15
};

typedef struct memcard memcard_t;
memcard_t *memcard_open(const char *path, char *error, size_t error_size);
void memcard_close(memcard_t *);
void memcard_reset(memcard_t *);
/* ack_delay is zero on the final byte or an absent device, otherwise cycles after RX. */
uint8_t memcard_exchange(memcard_t *, uint8_t tx, unsigned *ack_delay);
int memcard_failed(const memcard_t *);
unsigned memcard_flags(const memcard_t *);
int memcard_read_sector(memcard_t *, unsigned sector, uint8_t data[128]);
int memcard_write_sector(memcard_t *, unsigned sector, const uint8_t data[128]);
int memcard_format(memcard_t *);

typedef struct sio_state sio_state_t;
/* Host owns this attachment: destroy before hw_destroy/cpu_init when reusing a CPU. */
void sio_init(cpu_t *);
void sio_destroy(cpu_t *);
void sio_set_buttons(cpu_t *, unsigned port, uint16_t pressed);
void sio_set_pad(cpu_t *, unsigned port, int connected, int dualshock);
/* Wire order: right X/Y, left X/Y; centre is 0x80. */
void sio_set_axes(cpu_t *, unsigned port, const uint8_t axes[4]);
typedef struct { uint8_t analog, small_motor, large_motor; } sio_pad_output_t;
sio_pad_output_t sio_pad_output(cpu_t *, unsigned port);
int sio_open_card(cpu_t *, unsigned port, const char *path, char *error, size_t error_size);
int sio_cards_ok(cpu_t *);
memcard_t *sio_card(cpu_t *, unsigned port);
int sio_pad_snapshot(cpu_t *, unsigned port, uint16_t *buttons);
int sio_load_script(cpu_t *, const char *path, char *error, size_t error_size);
void sio_input_vblank(cpu_t *);
unsigned sio_input_disc(cpu_t *);

/* SDL entry points live in display.c; this keeps input-only additions out of display.h. */
uint16_t host_input_buttons(unsigned port);

#endif

/* Private real-disc probe. Link with --wrap=host_present,--wrap=host_input_sync,
 * --wrap=sio_write,--wrap=sio_read.
 * Use SDL dummy drivers, --pad dualshock, no --input-script, and a copied card.
 * FF8_PAD_PROBE=idle|axis|digital: load with the profile-field schedule, then
 * move right for VBlanks 1700..1899. FF8_PAD_PROBE_SEQUENCE optionally replaces
 * that schedule: VBLANK HEX_BUTTONS LEFTX LEFTY RIGHTX RIGHTY (raw SDL axes).
 * Output and captures stay in build/.
 */
#include "display.h"
#include "input.h"
#include "hw.h"
#include <SDL.h>
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void __real_host_present(gpu_t *);
void __real_host_input_sync(cpu_t *, host_input_t *);
void __real_sio_write(cpu_t *, uint32_t, uint32_t);
uint32_t __real_sio_read(cpu_t *, uint32_t);
static SDL_Joystick *joystick;
static unsigned frame, previous_mode = 99;
static uint8_t tx[16], rx[16];
static unsigned count, received, port;
static uint32_t peek(cpu_t *c, uint32_t address, unsigned width) {
    /* Observations must not synchronize deferred DMA through guest accessors. */
    return ram_read(c, address & (RAM_SIZE - 1), width);
}
void __wrap_sio_write(cpu_t *c, uint32_t address, uint32_t value) {
    if ((address & 15) == 10 && (value & 0x42) != 2) {
        if (count >= 2 && tx[0] == 1 && tx[1] != 0x42) {
            fprintf(stderr, "PAD_COMMAND vblank=%llu pc=%08x tx=", (unsigned long long)c->hw->vblanks, c->pc);
            for (unsigned i = 0; i < count; ++i) fprintf(stderr, "%02x", tx[i]);
            fputc('\n', stderr);
        }
        if (count >= 2 && tx[0] == 1 && tx[1] == 0x42 && c->hw->vblanks >= 1600) {
            fprintf(stderr, "PAD_POLL vblank=%llu port=%u tx=", (unsigned long long)c->hw->vblanks, port);
            for (unsigned i = 0; i < count; ++i) fprintf(stderr, "%02x", tx[i]);
            fprintf(stderr, " rx=");
            for (unsigned i = 0; i < received; ++i) fprintf(stderr, "%02x", rx[i]);
            fputc('\n', stderr);
        }
        count = received = 0;
    }
    if ((address & 15) == 10) port = (value >> 13) & 1;
    if (!(address & 15) && count < sizeof(tx)) tx[count++] = (uint8_t)value;
    __real_sio_write(c, address, value);
}
uint32_t __wrap_sio_read(cpu_t *c, uint32_t address) {
    uint32_t value = __real_sio_read(c, address);
    /* Ignore the driver's drain of the previous transaction before its first TX. */
    if (!(address & 15) && received < count) rx[received++] = (uint8_t)value;
    return value;
}
static void sequence(uint16_t *buttons, int axes[4]) {
    static FILE *file;
    static unsigned next, previous, held, next_buttons;
    static int active, pending, held_axes[4], next_axes[4];
    if (frame == 1) {
        const char *path = getenv("FF8_PAD_PROBE_SEQUENCE");
        if (path) { file = fopen(path, "r"); assert(file); active = 1; }
    }
    if (!active) return;
    for (;;) {
        if (!pending && file) {
            int n = fscanf(file, "%u %x %d %d %d %d", &next, &next_buttons,
                           next_axes, next_axes + 1, next_axes + 2, next_axes + 3);
            if (n == EOF) { fclose(file); file = NULL; }
            else {
                assert(n == 6 && next > previous && next_buttons <= 65535);
                for (unsigned i = 0; i < 4; ++i) assert(next_axes[i] >= -32768 && next_axes[i] <= 32767);
                pending = 1;
            }
        }
        if (!pending || frame < next) break;
        previous = next; held = next_buttons; memcpy(held_axes, next_axes, sizeof(held_axes)); pending = 0;
    }
    *buttons = (uint16_t)held; memcpy(axes, held_axes, sizeof(held_axes));
}
void __wrap_host_present(gpu_t *gpu) {
    static const unsigned cross[] = {300, 600, 900, 1140, 1300, 1460};
    const char *mode = getenv("FF8_PAD_PROBE");
    if (!joystick) {
        int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 16, 0);
        assert(device >= 0); joystick = SDL_JoystickOpen(device); assert(joystick);
        for (unsigned axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; ++axis)
            assert(SDL_JoystickSetVirtualAxis(joystick, (int)axis,
                   axis >= SDL_CONTROLLER_AXIS_TRIGGERLEFT ? -32768 : 0) == 0);
    }
    ++frame;
    int down = 0, moving = frame >= 1700 && frame < 1900;
    for (unsigned i = 0; i < sizeof(cross) / sizeof(*cross); ++i)
        if (frame >= cross[i] && frame < cross[i] + (i < 3 ? 2 : 8)) down = 1;
    uint16_t buttons = down ? PAD_CROSS : 0;
    int axes[4] = {moving && mode && !strcmp(mode, "axis") ? 32767 : 0, 0, 0, 0};
    if (moving && mode && !strcmp(mode, "digital")) buttons |= PAD_RIGHT;
    sequence(&buttons, axes);
    static const int sdl_buttons[] = {
        SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT, -1, -1,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_X
    };
    for (unsigned i = 0; i < 16; ++i)
        if (sdl_buttons[i] >= 0) assert(SDL_JoystickSetVirtualButton(joystick, sdl_buttons[i], !!(buttons & (1u << i))) == 0);
    for (unsigned i = 0; i < 4; ++i) assert(SDL_JoystickSetVirtualAxis(joystick, (int)i, (Sint16)axes[i]) == 0);
    __real_host_present(gpu);
}
void __wrap_host_input_sync(cpu_t *c, host_input_t *input) {
    /* Detect a probe built with the wrong statistics-dependent hw_t layout. */
    assert(c->hw->vblanks == frame);
    __real_host_input_sync(c, input);
    /* Trigger rest is raw -32768, not the midpoint of SDL's full-range map. */
    assert(!(host_input_buttons(0) & (PAD_L2 | PAD_R2)));
    sio_pad_output_t out = sio_pad_output(c, 0);
    /* USA field module metadata, read only. A changed pose alone is not walking. */
    uint32_t map = peek(c, 0x8005f0f8, 4), actors = peek(c, 0x80085224, 4);
    unsigned actor = peek(c, 0x8005f148, 2);
    int field_name = map >= 0x80000000 && map <= 0x801ffff8;
    for (unsigned i = 0; field_name && i < 8; ++i) {
        unsigned ch = peek(c, map + i, 1);
        if (!ch) { if (!i) field_name = 0; break; }
        if (!isalnum((int)ch) && ch != '_') field_name = 0;
    }
    if (frame >= 1600 && field_name && actors >= 0x80000000 && actors < 0x80200000 && actor < 32) {
        uint32_t player = actors + actor * 0x264;
        fprintf(stderr, "PAD_FIELD vblank=%llu map=%.8s x=%d y=%d z=%d facing=%u\n",
                (unsigned long long)c->hw->vblanks, (char *)c->ram + (map & (RAM_SIZE - 1)),
                (int32_t)peek(c, player + 0x190, 4), (int32_t)peek(c, player + 0x194, 4),
                (int32_t)peek(c, player + 0x198, 4), peek(c, player + 0x241, 1));
    }
    if (out.analog != previous_mode || frame % 100 == 0) {
        uint8_t axes[4]; host_input_axes(0, axes);
        fprintf(stderr, "PAD_PROBE vblank=%llu analog=%u motors=%u,%u axes=%u,%u,%u,%u buttons=%04x pc=%08x bios_pad=%u buffer=%08x\n",
                (unsigned long long)c->hw->vblanks, out.analog, out.small_motor, out.large_motor,
                axes[0], axes[1], axes[2], axes[3], host_input_buttons(0), c->pc,
                c->hw->pad_started, c->hw->pad_buffer[0]);
        previous_mode = out.analog;
    }
}

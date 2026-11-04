#include "hw.h"
#include "sio.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct pad {
    uint16_t buttons;
    unsigned connected, dualshock, config, analog, command, step, length;
    uint8_t reply[9], rumble[6];
    uint8_t axes[4], small_motor, large_motor;
};
struct input_event { uint64_t frame; uint16_t buttons; unsigned disc; };
struct sio_state {
    cpu_t *cpu;
    struct sio_state *next;
    struct pad pads[2];
    memcard_t *cards[2];
    uint8_t fifo[8], last_rx, tx, pending_tx;
    unsigned count, pending, latched_enable, device;
    struct input_event *script;
    size_t script_count, script_pos;
    uint16_t script_buttons;
    int scripted;
    unsigned disc_request;
};

/* The runtime ownership contract leaves hw's layout with the hardware streams.
 * Hosts attach/detach here; a process-exit fallback also covers old harnesses. */
static sio_state_t *states;
static int cleanup_registered;

static void release(sio_state_t *s) {
    memcard_close(s->cards[0]); memcard_close(s->cards[1]); free(s->script); free(s);
}
static void cleanup(void) {
    while (states) { sio_state_t *s = states; states = s->next; release(s); }
}
static sio_state_t *state(cpu_t *c, int create) {
    sio_state_t *s;
    unsigned port;
    for (s = states; s; s = s->next) if (s->cpu == c) return s;
    if (!create) return NULL;
    s = calloc(1, sizeof(*s));
    if (!s) { cpu_stop(c, "cannot allocate SIO state"); abort(); }
    if (!cleanup_registered) { atexit(cleanup); cleanup_registered = 1; }
    for (port = 0; port < 2; ++port) {
        s->pads[port].connected = 1;
        memset(s->pads[port].axes, 0x80, 4);
        memset(s->pads[port].rumble, 0xff, sizeof(s->pads[port].rumble));
    }
    s->cpu = c; s->next = states; states = s;
    return s;
}
void sio_init(cpu_t *c) { hw_get(c); (void)state(c, 1); }
void sio_destroy(cpu_t *c) {
    sio_state_t **at = &states;
    while (*at && (*at)->cpu != c) at = &(*at)->next;
    if (*at) { sio_state_t *s = *at; *at = s->next; release(s); }
}
void sio_set_buttons(cpu_t *c, unsigned port, uint16_t pressed) {
    if (port < 2) {
        sio_state_t *s = state(c, 1);
        s->pads[port].buttons = !port && s->scripted ? s->script_buttons : pressed;
    }
}
void sio_set_pad(cpu_t *c, unsigned port, int connected, int dualshock) {
    if (port < 2) {
        struct pad *p = &state(c, 1)->pads[port];
        memset(p, 0, sizeof(*p)); p->connected = !!connected; p->dualshock = !!dualshock;
        memset(p->rumble, 0xff, sizeof(p->rumble));
        memset(p->axes, 0x80, 4);
    }
}
void sio_set_axes(cpu_t *c, unsigned port, const uint8_t axes[4]) {
    if (port < 2) {
        sio_state_t *s = state(c, 1);
        if (port || !s->scripted) memcpy(s->pads[port].axes, axes, 4);
    }
}
sio_pad_output_t sio_pad_output(cpu_t *c, unsigned port) {
    sio_state_t *s = state(c, 0);
    sio_pad_output_t out = {0, 0, 0};
    if (s && port < 2 && s->pads[port].connected && s->pads[port].dualshock) {
        out.analog = (uint8_t)s->pads[port].analog;
        out.small_motor = s->pads[port].small_motor;
        out.large_motor = s->pads[port].large_motor;
    }
    return out;
}
int sio_open_card(cpu_t *c, unsigned port, const char *path, char *error, size_t error_size) {
    sio_state_t *s;
    memcard_t *card;
    if (port >= 2) { snprintf(error, error_size, "invalid card slot"); return 0; }
    card = memcard_open(path, error, error_size);
    if (!card) return 0;
    s = state(c, 1); memcard_close(s->cards[port]); s->cards[port] = card;
    return 1;
}
int sio_cards_ok(cpu_t *c) {
    sio_state_t *s = state(c, 0);
    return !s || (!memcard_failed(s->cards[0]) && !memcard_failed(s->cards[1]));
}
memcard_t *sio_card(cpu_t *c, unsigned port) {
    sio_state_t *s = state(c, 0);
    return s && port < 2 ? s->cards[port] : NULL;
}
int sio_pad_snapshot(cpu_t *c, unsigned port, uint16_t *buttons) {
    sio_state_t *s = state(c, 1);
    if (port >= 2 || !s->pads[port].connected) return 0;
    *buttons = (uint16_t)~s->pads[port].buttons;
    return 1;
}

static void deselect(cpu_t *c, sio_state_t *s) {
    unsigned port;
    s->device = 0;
    for (port = 0; port < 2; ++port) {
        s->pads[port].step = 0; memcard_reset(s->cards[port]);
    }
    c->hw->sio.ack = 0; c->hw->sio.ack_due = HW_NEVER;
}

static uint8_t pad_exchange(struct pad *p, uint8_t tx, unsigned *ack) {
    unsigned step = p->step;
    uint8_t rx;
    *ack = 0;
    if (!p->connected || step >= 9) return 0xff;
    if (!step) {
        if (tx != 1) { p->step = 9; return 0xff; }
        p->step = 1; *ack = 338; return 0xff;
    }
    if (step == 1) {
        uint16_t buttons = (uint16_t)~p->buttons;
        if ((!p->config && tx != 0x42 && !(p->dualshock && tx == 0x43)) ||
            (p->config && tx != 0x42 && tx != 0x43 && tx != 0x44 && tx != 0x45 &&
             tx != 0x46 && tx != 0x47 && tx != 0x4c && tx != 0x4d)) {
            p->step = 9; return 0xff;
        }
        p->command = tx; p->length = p->config || p->analog ? 9 : 5;
        memset(p->reply, 0, sizeof(p->reply));
        p->reply[1] = p->config ? 0xf3 : p->analog ? 0x73 : 0x41; p->reply[2] = 0x5a;
        if (tx == 0x42 || (tx == 0x43 && !p->config)) {
            p->reply[3] = (uint8_t)buttons; p->reply[4] = (uint8_t)(buttons >> 8);
            memcpy(p->reply + 5, p->axes, 4);
        } else if (tx == 0x45) {
            p->reply[3] = 1; p->reply[4] = 2; p->reply[5] = p->analog;
            p->reply[6] = 2; p->reply[7] = 1;
        } else if (tx == 0x47) { p->reply[5] = 2; p->reply[7] = 1; }
        else if (tx == 0x4d) memcpy(p->reply + 3, p->rumble, 6);
    }
    rx = p->reply[step];
    if (step == 3) {
        if (p->command == 0x43 && tx <= 1) p->config = tx;
        if (p->command == 0x44 && tx <= 1) p->analog = tx;
        if (p->command == 0x46 && tx <= 1) {
            p->reply[5] = 1; p->reply[6] = tx ? 1 : 2;
            p->reply[7] = tx; p->reply[8] = tx ? 20 : 10;
        }
        if (p->command == 0x4c) p->reply[6] = tx == 0 ? 4 : tx == 1 ? 7 : 0;
    }
    if (p->command == 0x4d && step >= 3) p->rumble[step - 3] = tx;
    if (p->command == 0x42 && step >= 3 && p->dualshock) {
        if (p->rumble[step - 3] == 0) p->small_motor = tx == 1;
        if (p->rumble[step - 3] == 1) p->large_motor = tx;
    }
    if (++p->step < p->length) *ack = 338;
    else p->step = 9;
    return rx;
}

static unsigned bit_cycles(cpu_t *c) {
    static const unsigned factors[] = {1, 1, 16, 64};
    unsigned ticks = (c->hw->sio.baud * factors[c->hw->sio.mode & 3]) & ~1u;
    return ticks ? ticks : 1;
}
static void interrupt(cpu_t *c) {
    if (!c->hw->sio.irq) { c->hw->sio.irq = 1; hw_irq_raise(c, 7); }
}
static void start(cpu_t *c, sio_state_t *s) {
    struct hw *h = c->hw;
    unsigned cycles;
    if (!s->pending || h->sio.due != HW_NEVER || (!(h->sio.control & 1) && !s->latched_enable)) return;
    s->tx = s->pending_tx; s->pending = s->latched_enable = 0;
    cycles = bit_cycles(c) * (5 + ((h->sio.mode >> 2) & 3));
    h->sio.due = h->now + cycles;
    if (h->sio.control & 0x400) interrupt(c);
}

uint32_t sio_read(cpu_t *c, uint32_t address) {
    struct hw *h = c->hw;
    sio_state_t *s = state(c, 1);
    uint32_t value;
    unsigned i;
    switch (address & 15) {
    case 0:
        value = s->count ? s->fifo[0] : s->last_rx;
        for (i = 1; i < 4; ++i) value |= (uint32_t)(i < s->count ? s->fifo[i] : s->last_rx) << (8 * i);
        if (s->count) { --s->count; memmove(s->fifo, s->fifo + 1, s->count); }
        return value;
    case 4:
        return (!s->pending ? 1 : 0) | (s->count ? 2 : 0) |
            (h->sio.due == HW_NEVER && !s->pending ? 4 : 0) |
            (h->sio.ack == 2 ? 0x80 : 0) | (h->sio.irq ? 0x200 : 0);
    case 8: return h->sio.mode;
    case 9: return h->sio.mode >> 8;
    case 10: return h->sio.control;
    case 11: return h->sio.control >> 8;
    case 14: return h->sio.baud;
    case 15: return h->sio.baud >> 8;
    default: return 0;
    }
}
void sio_write(cpu_t *c, uint32_t address, uint32_t value) {
    struct hw *h = c->hw;
    sio_state_t *s = state(c, 1);
    switch (address & 15) {
    case 0:
        s->pending_tx = (uint8_t)value; s->pending = 1; s->latched_enable = h->sio.control & 1;
        start(c, s); break;
    case 9: value = (h->sio.mode & 255) | (value << 8); /* fall through */
    case 8: h->sio.mode = value & 0x13f; break;
    case 11: value = (h->sio.control & 255) | (value << 8); /* fall through */
    case 10:
        if (value & 0x40) {
            deselect(c, s); s->count = s->pending = s->latched_enable = 0;
            h->sio.mode = h->sio.control = h->sio.baud = h->sio.irq = 0;
            h->sio.due = HW_NEVER; break;
        }
        if (((h->sio.control & 2) && !(value & 2)) || ((value ^ h->sio.control) & 0x2000)) {
            deselect(c, s); h->sio.due = HW_NEVER; s->pending = s->latched_enable = 0;
        }
        if (value & 0x10) h->sio.irq = 0;
        h->sio.control = value & 0x3f07;
        start(c, s); break;
    case 15: value = (h->sio.baud & 255) | (value << 8); /* fall through */
    case 14: h->sio.baud = (uint16_t)value; break;
    }
}
void sio_event(cpu_t *c) {
    struct hw *h = c->hw;
    sio_state_t *s = state(c, 0);
    if (!s) return;
    if (h->sio.ack_due <= h->now) {
        if (h->sio.ack == 1) {
            h->sio.ack = 2; h->sio.ack_due = h->now + 96;
            if (h->sio.control & 0x1000) interrupt(c);
        } else { h->sio.ack = 0; h->sio.ack_due = HW_NEVER; }
    }
    if (h->sio.due > h->now) return;
    h->sio.due = HW_NEVER;
    {
        uint8_t rx = 0xff;
        unsigned delay = 0, port = (h->sio.control >> 13) & 1;
        if (h->sio.control & 2) {
            if (!s->device) s->device = s->tx == 1 ? 1 : s->tx == 0x81 ? 2 : 3;
            if (s->device == 1) rx = pad_exchange(&s->pads[port], s->tx, &delay);
            if (s->device == 2) rx = memcard_exchange(s->cards[port], s->tx, &delay);
        }
        if (h->sio.control & 6) {
            s->last_rx = rx;
            if (s->count < 8) s->fifo[s->count++] = rx;
            else s->fifo[7] = rx;
            h->sio.control &= (uint16_t)~4u;
            if ((h->sio.control & 0x800) && s->count >= (1u << ((h->sio.control >> 8) & 3))) interrupt(c);
        }
        if (delay) { h->sio.ack = 1; h->sio.ack_due = h->now + delay; }
        if (h->sio.control & 0x400) interrupt(c);
        start(c, s);
    }
}

static int buttons(char *text, uint16_t *mask) {
    static const char *names[] = {"SELECT", "L3", "R3", "START", "UP", "RIGHT", "DOWN", "LEFT",
                                  "L2", "R2", "L1", "R1", "TRIANGLE", "CIRCLE", "CROSS", "SQUARE"};
    char *part = text;
    *mask = 0;
    if (!strcmp(text, "-")) return 1;
    while (*part) {
        char *end = strchr(part, '+');
        unsigned i;
        if (end) *end = 0;
        for (i = 0; i < 16 && strcmp(part, names[i]); ++i) {}
        if (i == 16) return 0;
        *mask |= (uint16_t)(1u << i);
        if (!end) return 1;
        part = end + 1;
    }
    return 0;
}
int sio_load_script(cpu_t *c, const char *path, char *error, size_t error_size) {
    FILE *file = fopen(path, "r");
    struct input_event *events = NULL;
    size_t count = 0;
    unsigned line = 0;
    char buffer[512];
    if (!file) { snprintf(error, error_size, "%s: %s", path, strerror(errno)); return 0; }
    while (fgets(buffer, sizeof(buffer), file)) {
        char *p = buffer, *end, *comment = strchr(buffer, '#');
        unsigned long long frame;
        uint16_t mask;
        unsigned disc = 0;
        struct input_event *grown;
        ++line;
        if (!strchr(buffer, '\n') && !feof(file)) goto invalid;
        if (comment) *comment = 0;
        while (isspace((unsigned char)*p)) ++p;
        if (!*p) continue;
        if (!isdigit((unsigned char)*p)) goto invalid;
        errno = 0; frame = strtoull(p, &end, 10);
        if (errno || !isspace((unsigned char)*end)) goto invalid;
        p = end;
        while (isspace((unsigned char)*p)) ++p;
        end = p + strlen(p);
        while (end > p && isspace((unsigned char)end[-1])) *--end = 0;
        if (strlen(p) == 5 && !memcmp(p, "DISC", 4) && p[4] >= '1' && p[4] <= '4') {
            disc = (unsigned)(p[4] - '0'); mask = 0;
        } else if (!buttons(p, &mask)) goto invalid;
        if (count && frame <= events[count - 1].frame) goto invalid;
        grown = realloc(events, (count + 1) * sizeof(*events));
        if (!grown) { snprintf(error, error_size, "cannot allocate input script"); goto failure; }
        events = grown; events[count].frame = frame; events[count].buttons = mask; events[count++].disc = disc;
    }
    if (ferror(file)) { snprintf(error, error_size, "%s: cannot read script", path); goto failure; }
    fclose(file);
    {
        sio_state_t *s = state(c, 1);
        free(s->script); s->script = events; s->script_count = count; s->script_pos = 0;
        s->scripted = 1; s->script_buttons = 0; s->disc_request = 0;
        sio_input_vblank(c);
    }
    return 1;
invalid:
    snprintf(error, error_size, "%s:%u: expected increasing VBLANK BUTTONS (uppercase names joined by +, -, or DISC1..DISC4)", path, line);
failure:
    fclose(file); free(events); return 0;
}
void sio_input_vblank(cpu_t *c) {
    sio_state_t *s = state(c, 0);
    if (!s || !s->scripted) return;
    while (s->script_pos < s->script_count && s->script[s->script_pos].frame <= c->hw->vblanks) {
        struct input_event *e = &s->script[s->script_pos++];
        if (e->disc) {
            s->disc_request = e->disc;
            cpu_log(c, "INPUT VBlank=%llu disc=%u", (unsigned long long)e->frame, e->disc);
        } else {
            s->script_buttons = e->buttons;
            cpu_log(c, "INPUT VBlank=%llu buttons=%04x", (unsigned long long)e->frame, e->buttons);
        }
    }
    s->pads[0].buttons = s->script_buttons;
}

unsigned sio_input_disc(cpu_t *c) {
    sio_state_t *s = state(c, 0);
    unsigned disc = s ? s->disc_request : 0;
    if (s) s->disc_request = 0;
    return disc;
}

#if defined(FF8_SAVESTATES)
#define FF8_SS_PRIVATE
#include "savestate_sio.c"
#endif

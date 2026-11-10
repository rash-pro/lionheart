#include "hw.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

/* An absent SPU stream is a weak no-op; once linked, it owns the PCM queue. */
#if defined(__GNUC__) || defined(__clang__)
void spu_cd_input(struct spu *, const int16_t *, unsigned) __attribute__((weak));
#else
void spu_cd_input(struct spu *, const int16_t *, unsigned);
#endif

static int32_t audio_shift(int32_t n, unsigned bits) {
    return n >= 0 ? n / (1 << bits) : -(int32_t)(((uint32_t)-n + ((1u << bits) - 1)) >> bits);
}
static int16_t audio_clip(int32_t n) { return (int16_t)(n < -32768 ? -32768 : n > 32767 ? 32767 : n); }

void cdrom_audio_init(hw_cd_t *d) {
    memset(d->xa_history, 0, sizeof(d->xa_history));
    memset(d->xa_ring, 0, sizeof(d->xa_ring));
    d->xa_ring_pos = d->xa_sixstep = d->xa_active = d->xa_muted = 0;
    d->volume[0] = d->volume[2] = 0x80;
    d->volume[1] = d->volume[3] = 0;
    memcpy(d->pending_volume, d->volume, 4);
}

/* Each row is a tap, each column an output phase of the 6:7 XA filter.
 * Numeric coefficients from psx-spx, CDROM Format / XA interpolation. */
static const int16_t xa_filter[29][7] = {
    {0,0,0,0,-1,2,-5}, {0,0,0,-1,3,-8,0x11}, {0,0,-1,3,-8,0x10,-0x23},
    {0,-2,3,-8,0x11,-0x23,0x46}, {0,0,-2,6,-0x10,0x2b,-0x17},
    {-2,3,-5,5,0xa,0x1a,-0x44}, {0xa,-0x13,0x1f,-0x1b,0x6b,-0xeb,0x15b},
    {-0x22,0x3c,-0x4a,0xa6,-0x16d,0x27b,-0x347},
    {0x41,-0x4b,0xb3,-0x1a8,0x350,-0x548,0x80e},
    {-0x54,0xa2,-0x192,0x372,-0x623,0xafa,-0x1249},
    {0x34,-0xe3,0x2b1,-0x5bf,0xbcd,-0x16fa,0x3c07},
    {9,0x132,-0x39e,0x9b8,-0x1780,0x53e0,0x53e0},
    {-0x10a,-0x43,0x4f8,-0x11b4,0x6794,0x3c07,-0x16fa},
    {0x400,-0x267,-0x5a6,0x74bb,0x234c,-0x1249,0xafa},
    {-0xa78,0xc9d,0x7939,0xc9d,-0xa78,0x80e,-0x548},
    {0x234c,0x74bb,-0x5a6,-0x267,0x400,-0x347,0x27b},
    {0x6794,-0x11b4,0x4f8,-0x43,-0x10a,0x15b,-0xeb},
    {-0x1780,0x9b8,-0x39e,0x132,9,-0x44,0x1a},
    {0xbcd,-0x5bf,0x2b1,-0xe3,0x34,-0x17,0x2b},
    {-0x623,0x372,-0x192,0xa2,-0x54,0x46,-0x23},
    {0x350,-0x1a8,0xb3,-0x4b,0x41,-0x23,0x10},
    {-0x16d,0xa6,-0x4a,0x3c,-0x22,0x11,-8},
    {0x6b,-0x1b,0x1f,-0x13,0xa,-5,2}, {0xa,5,-5,3,-1,0,0},
    {-0x10,6,-2,0,0,0,0}, {0x11,-8,3,-2,1,0,0},
    {-8,3,-1,0,0,0,0}, {3,-1,0,0,0,0,0}, {-1,0,0,0,0,0,0}
};

/* Native-rate stereo frames; mono history is continuous across sound units. */
unsigned cdrom_decode_xa(hw_cd_t *d, const uint8_t *sector, int16_t *stereo) {
    static const int positive[4] = {0,60,115,98}, negative[4] = {0,0,-52,-55};
    unsigned coding = sector[19], channels = (coding & 1) ? 2 : 1;
    unsigned bits = (coding & 0x10) ? 8 : 4, units = 32 / bits;
    unsigned group, unit, sample, base = 0;
    if (coding & 0xaa) return 0;
    for (group = 0; group < 18; ++group) {
        const uint8_t *p = sector + 24 + group * 128;
        for (unit = 0; unit < units; ++unit) {
            unsigned channel = unit % channels, header = p[4 + unit], shift = header & 15, filter = (header >> 4) & 3;
            if (shift > 12) shift = 9;
            for (sample = 0; sample < 28; ++sample) {
                unsigned raw = p[16 + sample * 4 + unit / (8 / bits)];
                int value;
                if (bits == 4) { raw = (raw >> ((unit & 1) * 4)) & 15; value = raw & 8 ? (int)raw - 16 : (int)raw; }
                else value = raw & 128 ? (int)raw - 256 : (int)raw;
                value = audio_shift(value * (1 << (16 - bits)), shift) +
                        audio_shift(d->xa_history[channel][0] * positive[filter] +
                                    d->xa_history[channel][1] * negative[filter] + 32, 6);
                d->xa_history[channel][1] = d->xa_history[channel][0];
                d->xa_history[channel][0] = audio_clip(value);
                stereo[2 * (base + unit / channels * 28 + sample) + channel] = audio_clip(value);
                if (channels == 1) stereo[2 * (base + unit * 28 + sample) + 1] = audio_clip(value);
            }
        }
        base += units / channels * 28;
    }
    return base;
}

static void cd_audio(cpu_t *c, int16_t *samples, unsigned frames, int xa) {
    hw_cd_t *d = &c->hw->cd;
    unsigned i;
    for (i = 0; i < frames; ++i) {
        int l = samples[i * 2], r = samples[i * 2 + 1];
        samples[i * 2] = d->muted || (xa && d->xa_muted) ? 0 : audio_clip(audio_shift(l * d->volume[0] + r * d->volume[3], 7));
        samples[i * 2 + 1] = d->muted || (xa && d->xa_muted) ? 0 : audio_clip(audio_shift(l * d->volume[1] + r * d->volume[2], 7));
    }
    if (spu_cd_input) spu_cd_input(c->hw->spu, samples, frames);
}

static void xa_sector(cpu_t *c) {
    hw_cd_t *d = &c->hw->cd;
    int16_t pcm[4032 * 2], output[7 * 2];
    unsigned count, i, repeat, phase, tap, channel;
    if (!d->xa_active) {
        memset(d->xa_history, 0, sizeof(d->xa_history));
        memset(d->xa_ring, 0, sizeof(d->xa_ring));
        d->xa_ring_pos = d->xa_sixstep = 0;
        d->xa_file = d->sector[16]; d->xa_channel = d->sector[17]; d->xa_active = 1;
    }
    if (d->sector[16] != d->xa_file || d->sector[17] != d->xa_channel) { ++d->xa_filtered; return; }
    count = cdrom_decode_xa(d, d->sector, pcm);
    for (i = 0; i < count; ++i) for (repeat = 0; repeat < ((d->sector[19] & 4) ? 2u : 1u); ++repeat) {
        for (channel = 0; channel < 2; ++channel) d->xa_ring[channel][d->xa_ring_pos] = pcm[2 * i + channel];
        d->xa_ring_pos = (d->xa_ring_pos + 1) & 31;
        if (++d->xa_sixstep != 6) continue;
        d->xa_sixstep = 0;
        for (phase = 0; phase < 7; ++phase) for (channel = 0; channel < 2; ++channel) {
            int32_t sum = 0;
            for (tap = 0; tap < 29; ++tap)
                sum += audio_shift(d->xa_ring[channel][(d->xa_ring_pos - tap - 1) & 31] * xa_filter[tap][phase], 15);
            output[phase * 2 + channel] = audio_clip(sum);
        }
        cd_audio(c, output, 7, 1); d->xa_frames += 7;
    }
    ++d->xa_sectors;
    if (d->sector[18] & 0x80) d->xa_active = 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned bcd(unsigned n) { return (n / 10) * 16 + n % 10; }
static unsigned unbcd(unsigned n) { return (n >> 4) * 10 + (n & 15); }
static int valid_bcd(unsigned n) { return (n & 15) < 10 && (n >> 4) < 10; }
static void msf(unsigned lba, uint8_t *p) { p[0] = bcd(lba / 4500); p[1] = bcd(lba / 75 % 60); p[2] = bcd(lba % 75); }
static uint64_t interval(hw_cd_t *d) { return HW_CPU_HZ / (!d->playing && (d->mode & 0x80) ? 150 : 75); }

static int raw_sector(hw_cd_t *d, uint32_t lba, uint8_t *out) {
    uint64_t offset = ((uint64_t)d->offset + lba) * 2352;
    return d->file && lba < d->sectors && offset <= LONG_MAX && !fseek(d->file, (long)offset, SEEK_SET) && fread(out, 1, 2352, d->file) == 2352;
}
static int user_sector(hw_cd_t *d, uint32_t lba, uint8_t *out) {
    uint8_t raw[2352];
    if (!raw_sector(d, lba, raw)) return 0;
    memcpy(out, raw + (raw[15] == 1 ? 16 : 24), 2048);
    return 1;
}

static void img_table(hw_cd_t *d) {
    uint8_t pvd[2048], root[2048], table[2048];
    uint32_t root_lba, root_size, sector, pos;
    if (!user_sector(d, 16, pvd) || memcmp(pvd + 1, "CD001", 5)) return;
    root_lba = le32(pvd + 158); root_size = le32(pvd + 166);
    if (root_size > 1024 * 1024) return;
    for (sector = 0; sector < (root_size + 2047) / 2048; ++sector) {
        if (!user_sector(d, root_lba + sector, root)) return;
        for (pos = 0; pos + 34 <= 2048 && root[pos]; pos += root[pos]) {
            unsigned len = root[pos], namesize = root[pos + 32];
            if (len < 34 || pos + len > 2048 || namesize > len - 33) break;
            if (namesize >= 12 && !memcmp(root + pos + 33, "FF8DISC", 7) &&
                root[pos + 40] >= '1' && root[pos + 40] <= '4' && !memcmp(root + pos + 41, ".IMG", 4)) {
                d->img_lba = le32(root + pos + 2);
                if (!user_sector(d, d->img_lba, table)) return;
                for (pos = 0; pos < 256; ++pos) {
                    uint32_t lba = le32(table + pos * 8), size = le32(table + pos * 8 + 4);
                    if (!lba && !size) break;
                    if (lba >= d->sectors || (uint64_t)size > (uint64_t)(d->sectors - lba) * 2048) { d->img_count = 0; return; }
                    d->img[pos].lba = lba; d->img[pos].size = size;
                    ++d->img_count;
                }
                return;
            }
        }
    }
}

static int open_image(hw_cd_t *d, const char *path, char *error, size_t error_size) {
    char binary[4096], line[4096], name[3072], mode[32];
    FILE *file;
    long length;
    size_t n = strlen(path);
    unsigned index = 0;
    if (d->file) { snprintf(error, error_size, "disc already open"); return 0; }
    if (n >= sizeof(binary)) goto invalid;
    strcpy(binary, path);
    if (n >= 4 && (!strcmp(path + n - 4, ".cue") || !strcmp(path + n - 4, ".CUE"))) {
        unsigned files = 0, tracks = 0, found_index = 0;
        file = fopen(path, "r");
        if (!file) goto io_error;
        while (fgets(line, sizeof(line), file)) {
            unsigned track, m, s, f;
            if (sscanf(line, " FILE \"%3071[^\"]\" %31s", name, mode) == 2) {
                const char *slash = strrchr(path, '/');
#ifdef _WIN32
                const char *backslash = strrchr(path, '\\');
                if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
                size_t prefix = slash ? (size_t)(slash - path + 1) : 0;
                if (++files != 1 || strcmp(mode, "BINARY") || prefix + strlen(name) >= sizeof(binary)) { fclose(file); goto invalid; }
                if (name[0] == '/'
#ifdef _WIN32
                    || name[0] == '\\' || (name[0] && name[1] == ':')
#endif
                ) strcpy(binary, name);
                else { memcpy(binary, path, prefix); strcpy(binary + prefix, name); }
            } else if (sscanf(line, " TRACK %u %31s", &track, mode) == 2) {
                if (++tracks != 1 || track != 1 || (strcmp(mode, "MODE2/2352") && strcmp(mode, "MODE1/2352"))) { fclose(file); goto invalid; }
            } else if (sscanf(line, " INDEX 01 %u:%u:%u", &m, &s, &f) == 3) {
                if (s >= 60 || f >= 75 || m > 99) { fclose(file); goto invalid; }
                index = (m * 60 + s) * 75 + f; found_index = 1;
            }
        }
        fclose(file);
        if (files != 1 || tracks != 1 || !found_index) goto invalid;
    }
    file = fopen(binary, "rb");
    if (!file) goto io_error;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 || length % 2352 || (uint64_t)length / 2352 <= index) { fclose(file); goto invalid; }
    d->sectors = (uint32_t)(length / 2352) - index; d->offset = index;
    d->lba_reads = calloc(d->sectors, sizeof(uint32_t)); d->lba_dma = calloc(d->sectors, sizeof(uint32_t));
    if (!d->lba_reads || !d->lba_dma) {
        /* Per-LBA coverage is diagnostic. A transactional swap temporarily owns
         * two images, so its counters must not prevent insertion during playback. */
        free(d->lba_reads); free(d->lba_dma); d->lba_reads = d->lba_dma = NULL;
    }
    d->file = file; d->status = 2;
    img_table(d);
    return 1;
invalid:
    snprintf(error, error_size, "expected a single-track MODE1/2352 or MODE2/2352 CUE, or a raw 2352-byte BIN"); return 0;
io_error:
    snprintf(error, error_size, "%s", strerror(errno)); return 0;
}

int cdrom_open(cpu_t *c, const char *path, char *error, size_t error_size) {
    return open_image(&hw_get(c)->cd, path, error, error_size);
}

static void reply(cpu_t *c, unsigned irq, const uint8_t *data, unsigned size) {
    hw_cd_t *d = &c->hw->cd;
    memcpy(d->response, data, size); d->response_pos = 0; d->response_count = size;
    d->irq_flags = irq; ++d->interrupts[irq];
    cpu_log(c, "CD INT%u status=%02x cycle=%llu", irq, data[0], (unsigned long long)c->hw->now);
    if (d->irq_flags & d->irq_enable) hw_irq_raise(c, 2);
}
static void error_reply(cpu_t *c, uint8_t reason) {
    uint8_t data[2] = {c->hw->cd.status | 1, reason};
    reply(c, 5, data, 2);
}
static void stop_read(hw_cd_t *d) { d->reading = d->playing = 0; d->read_due = HW_NEVER; d->status &= ~0xe0; }

int cdrom_swap(cpu_t *c, const char *path, char *error, size_t error_size) {
    hw_cd_t *d = &hw_get(c)->cd, *image = calloc(1, sizeof(*image));
    if (!image) { snprintf(error, error_size, "cannot allocate disc state"); return 0; }
    if (!open_image(image, path, error, error_size)) { free(image); return 0; }
    /* Validate the replacement before touching the running drive. */
    if (d->file) fclose(d->file);
    free(d->lba_reads); free(d->lba_dma);
    d->file = image->file; d->offset = image->offset; d->sectors = image->sectors;
    d->lba_reads = image->lba_reads; d->lba_dma = image->lba_dma;
    d->img_lba = image->img_lba; d->img_count = image->img_count;
    memcpy(d->img, image->img, sizeof(d->img)); free(image);
    stop_read(d);
    d->command_due = d->second_due = HW_NEVER;
    d->param_count = d->response_pos = d->response_count = d->irq_flags = 0;
    d->sector_size = d->sector_valid = d->data_pos = d->data_size = d->request = 0;
    d->location = d->read_lba = d->location_pending = 0;
    d->xa_active = 0;
    d->status = 0x10; d->shell_open = d->lid_irq = 1;
    d->lid_due = c->hw->now + HW_CPU_HZ / 2;
    cpu_log(c, "CD LID open VBlank=%llu", (unsigned long long)c->hw->vblanks);
    cdrom_event(c); hw_reschedule(c);
    return 1;
}

static void command(cpu_t *c) {
    hw_cd_t *d = &c->hw->cd;
    uint8_t data[16] = {d->status}, *p = d->command_params;
    unsigned size = 1, expected = 0, cmd = d->command;
    d->command_due = HW_NEVER;
    if (cmd == 2) expected = 3;
    else if (cmd == 0xd) expected = 2;
    else if (cmd == 0xe || cmd == 0x14 || cmd == 0x19 || cmd == 0x12) expected = 1;
    else if (cmd == 3) expected = d->command_count <= 1 ? d->command_count : 0;
    if (d->command_count != expected) { error_reply(c, 0x20); return; }
    if ((d->shell_open || d->lid_due != HW_NEVER) && cmd != 1 && cmd != 0x19) {
        error_reply(c, 0x80); return;
    }
    switch (cmd) {
    case 1:
        if (!d->shell_open) d->status &= ~0x10u;
        break;
    case 2: {
        unsigned absolute;
        if (!valid_bcd(p[0]) || !valid_bcd(p[1]) || !valid_bcd(p[2]) || unbcd(p[1]) >= 60 || unbcd(p[2]) >= 75) { error_reply(c, 0x10); return; }
        absolute = (unbcd(p[0]) * 60 + unbcd(p[1])) * 75 + unbcd(p[2]);
        if (absolute < 150) { error_reply(c, 0x10); return; }
        d->location = absolute - 150; d->location_pending = 1; break;
    }
    case 3: case 6: case 0x1b:
        if (!d->file) { error_reply(c, 0x80); return; }
        if (d->location_pending) { d->read_lba = d->location; d->location_pending = 0; d->xa_active = 0; }
        d->playing = cmd == 3; d->reading = !d->playing;
        d->status = 2 | (d->playing ? 0x80 : 0x20);
        d->read_due = c->hw->now + interval(d); data[0] = d->status; break;
    case 7: case 8: case 9: case 0xa: case 0x15: case 0x16: case 0x1a: case 0x1e: case 0x12:
        if (cmd != 0x1a) stop_read(d);
        if (cmd == 0xa) { d->mode = 0; d->muted = 0; d->sector_valid = 0; d->data_size = d->data_pos = 0; cdrom_audio_init(d); }
        if (cmd == 0x15 || cmd == 0x16) {
            if (!d->file || d->location >= d->sectors) { error_reply(c, 0x80); return; }
            d->read_lba = d->location; d->location_pending = 0; d->status |= 0x40;
        }
        d->second = cmd;
        d->second_due = c->hw->now + (cmd == 9 ? interval(d) : cmd == 0xa ? HW_CPU_HZ / 10 : HW_CPU_HZ / 30);
        data[0] = d->status; break;
    case 0xb: d->muted = 1; break;
    case 0xc: d->muted = 0; break;
    case 0xd:
        if (d->filter_file != p[0] || d->filter_channel != p[1]) d->xa_active = 0;
        d->filter_file = p[0]; d->filter_channel = p[1]; break;
    case 0xe: d->mode = p[0]; break;
    case 0xf: data[1] = d->mode; data[2] = 0; data[3] = d->filter_file; data[4] = d->filter_channel; size = 5; break;
    case 0x10:
        if (!d->sector_size) { error_reply(c, 0x80); return; }
        memcpy(data, d->header, 8); size = 8; break;
    case 0x11:
        data[0] = data[1] = 1; msf(d->read_lba ? d->read_lba - 1 : 0, data + 2);
        msf((d->read_lba ? d->read_lba - 1 : 0) + 150, data + 5); size = 8; break;
    case 0x13:
        if (!d->file) { error_reply(c, 0x80); return; }
        data[1] = data[2] = 1; size = 3; break;
    case 0x14:
        if (!d->file || p[0] > 1) { error_reply(c, 0x10); return; }
        msf(p[0] ? 150 : d->sectors + 150, data + 1); size = 3; break;
    case 0x19:
        if (p[0] == 0x20) { data[0] = 0x98; data[1] = 0x06; data[2] = 0x10; data[3] = 0xc3; size = 4; }
        else if (p[0] == 4) { data[0] = 0; }
        else if (p[0] == 5) { data[0] = data[1] = 0; size = 2; }
        else { error_reply(c, 0x10); return; }
        break;
    default: error_reply(c, 0x40); return;
    }
    reply(c, 3, data, size);
}

void cdrom_event(cpu_t *c) {
    hw_cd_t *d = &c->hw->cd;
    if (d->lid_due <= c->hw->now) {
        if (d->shell_open) {
            d->shell_open = 0; d->lid_due = c->hw->now + HW_CPU_HZ / 2;
            cpu_log(c, "CD LID closed VBlank=%llu", (unsigned long long)c->hw->vblanks);
        } else { d->status |= 2; d->lid_due = HW_NEVER; }
    }
    if (d->lid_irq && !d->irq_flags) {
        uint8_t data[2] = {0x11, 8};
        d->lid_irq = 0; reply(c, 5, data, 2);
    }
    if (!d->irq_flags && d->command_due <= c->hw->now) command(c);
    if (!d->irq_flags && d->second_due <= c->hw->now) {
        uint8_t data[8] = {2, 0, 0x20, 0, 'S', 'C', 'E', 'A'};
        d->second_due = HW_NEVER;
        d->status = d->file ? (d->second == 8 ? 0 : 2) : 0x10;
        data[0] = d->status;
        if (d->second == 0x1a && !d->file) { data[0] = 8; data[1] = 0x40; reply(c, 5, data, 8); }
        else reply(c, 2, data, d->second == 0x1a ? 8 : 1);
    }
    if (d->read_due <= c->hw->now) {
        d->read_due += interval(d);
        if (!raw_sector(d, d->read_lba, d->sector)) {
            stop_read(d);
            if (!d->irq_flags) { uint8_t status = d->status; reply(c, 4, &status, 1); }
            return;
        }
        d->sector_lba = d->read_lba++;
        ++d->sectors_read;
        if (d->lba_reads) ++d->lba_reads[d->sector_lba];
        cpu_log(c, "CD SECTOR lba=%u cycle=%llu mode=%02x file=%u channel=%u submode=%02x coding=%02x tag=%02x%02x%02x%02x",
                d->sector_lba, (unsigned long long)c->hw->now, d->sector[15], d->sector[16], d->sector[17],
                d->sector[18], d->sector[19], d->sector[24], d->sector[25], d->sector[26], d->sector[27]);
        memcpy(d->header, d->sector + 12, 8);
        if (d->playing) {
            int16_t pcm[588 * 2];
            unsigned i;
            for (i = 0; i < 588 * 2; ++i) {
                unsigned v = d->sector[i * 2] | (unsigned)d->sector[i * 2 + 1] << 8;
                pcm[i] = (int16_t)(v < 32768 ? (int)v : (int)v - 65536);
            }
            cd_audio(c, pcm, 588, 0);
        } else if (d->sector[15] == 2 && (d->sector[18] & 0x44) == 0x44 && (d->mode & 0x48)) {
            if ((d->mode & 0x40) && (!(d->mode & 8) ||
                (d->sector[16] == d->filter_file && d->sector[17] == d->filter_channel))) xa_sector(c);
            else ++d->xa_filtered;
        } else {
            if (d->sector_valid) ++d->overruns;
            d->sector_size = (d->mode & 0x20) ? 2340 : 2048;
            memcpy(d->sector_buffer, d->sector, sizeof(d->sector_buffer));
            d->buffer_lba = d->sector_lba;
            d->sector_valid = 1;
        }
    }
    if (!d->irq_flags && d->sector_valid) {
        uint8_t status = d->status;
        reply(c, 1, &status, 1);
        /* The sector remains available for the request-register transfer. */
        d->sector_valid = 0;
    }
}

static uint8_t data_read(hw_cd_t *d, int dma) {
    if (d->data_pos >= d->data_size) return 0;
    if (dma) {
        ++d->bytes_dma;
        if (d->lba_dma && d->data_lba < d->sectors) {
            unsigned start = d->data_size == 2340 ? (d->data[3] == 1 ? 4 : 12) : 0;
            if (d->lba_dma && d->data_pos >= start && d->data_pos < start + 2048 && d->lba_dma[d->data_lba] == d->data_pos - start)
                ++d->lba_dma[d->data_lba];
        }
    }
    return d->data[d->data_pos++];
}
uint32_t cdrom_dma(cpu_t *c) {
    uint32_t word = 0; unsigned i;
    for (i = 0; i < 4; ++i) word |= (uint32_t)data_read(&c->hw->cd, 1) << (i * 8);
    return word;
}
uint8_t cdrom_read(cpu_t *c, unsigned port) {
    hw_cd_t *d = &c->hw->cd;
    switch (port) {
    case 0: return d->index | (!d->param_count ? 8 : 0) | (d->param_count < 16 ? 0x10 : 0) |
                   (d->response_pos < d->response_count ? 0x20 : 0) | (d->data_pos < d->data_size ? 0x40 : 0) |
                   (d->command_due != HW_NEVER ? 0x80 : 0);
    case 1: return d->response_pos < d->response_count ? d->response[d->response_pos++] : 0;
    case 2: return data_read(d, 0);
    default: return 0xe0 | (d->index & 1 ? d->irq_flags : d->irq_enable);
    }
}
void cdrom_write(cpu_t *c, unsigned port, uint8_t value) {
    hw_cd_t *d = &c->hw->cd;
    if (port == 0) { d->index = value & 3; return; }
    if (d->index == 0) {
        if (port == 1) {
            if (d->command_due != HW_NEVER) { cpu_log(c, "CD command while busy ignored=%02x", value); return; }
            ++d->commands[value]; d->command = value;
            memcpy(d->command_params, d->params, d->param_count); d->command_count = d->param_count; d->param_count = 0;
            d->command_due = c->hw->now + 25000;
            cpu_log(c, "CD CMD %02x params=%u [%02x %02x %02x] cycle=%llu", value, d->command_count,
                    d->command_params[0], d->command_params[1], d->command_params[2], (unsigned long long)c->hw->now);
        } else if (port == 2) { if (d->param_count < 16) d->params[d->param_count++] = value; }
        else if (port == 3) {
            d->request = !!(value & 0x80);
            if (!d->request) d->data_pos = d->data_size = 0;
            else if (d->data_pos >= d->data_size && d->sector_size) {
                unsigned offset = d->sector_size == 2340 ? 12 : d->sector_buffer[15] == 1 ? 16 : 24;
                memcpy(d->data, d->sector_buffer + offset, d->sector_size);
                d->data_size = d->sector_size; d->data_pos = 0; d->data_lba = d->buffer_lba;
                dma_kick(c);
            }
        }
    } else if (d->index == 1) {
        if (port == 2) { d->irq_enable = value & 31; if (d->irq_flags & d->irq_enable) hw_irq_raise(c, 2); }
        else if (port == 3) { d->irq_flags &= ~(value & 31); if (value & 0x40) d->param_count = 0; }
    } else if (d->index == 2) {
        if (port == 2) d->pending_volume[0] = value;
        else if (port == 3) d->pending_volume[1] = value;
    } else if (d->index == 3) {
        if (port == 1) d->pending_volume[2] = value;
        else if (port == 2) d->pending_volume[3] = value;
        else if (port == 3) {
            d->xa_muted = value & 1;
            if (value & 0x20) memcpy(d->volume, d->pending_volume, 4);
        }
    }
}

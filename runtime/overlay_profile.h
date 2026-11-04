/* Optional diagnostic counts; signatures describe immutable instruction words,
 * so writable overlay data does not invalidate its identity. */
typedef struct {
    int index;
    uint32_t base, size, count, address[16], word[16];
    uint64_t interpreted, recompiled;
} profile_overlay_t;
struct overlay_profile {
    profile_overlay_t overlays[CODE_OVERLAYS + 3];
    unsigned count, cached[CODE_PAGES];
    uint64_t epoch[CODE_PAGES];
    unsigned checkpoints;
};

static void overlay_profile_init(cpu_t *c) {
    const char *path = getenv("FF8_OVERLAY_CATALOG");
    FILE *file;
    struct overlay_profile *p;
    if (!path || !getenv("FF8_OVERLAY_PROFILE")) return;
    file = fopen(path, "r");
    if (!file) { fprintf(stderr, "Cannot open overlay profile catalog: %s\n", path); return; }
    p = calloc(1, sizeof(*p));
    if (!p) { fclose(file); return; }
    p->count = 3; p->overlays[0].index = -2; p->overlays[1].index = -1; p->overlays[2].index = -3;
    while (p->count < CODE_OVERLAYS + 3) {
        profile_overlay_t *o = &p->overlays[p->count];
        unsigned i, base, size, count;
        if (fscanf(file, "%d %x %u %u", &o->index, &base, &size, &count) != 4) break;
        if (size < 4 || base < 0x80000000u || (base & 0x1fffffffu) >= RAM_SIZE ||
            size > RAM_SIZE - (base & (RAM_SIZE - 1)) || !count || count > 16) break;
        o->base = base; o->size = size; o->count = count;
        for (i = 0; i < count; ++i) {
            unsigned address, word, offset;
            if (fscanf(file, "%x %x", &address, &word) != 2 || address < base ||
                address - base > size - 4 || (address & 3)) break;
            o->address[i] = address; o->word[i] = word;
            offset = address & (RAM_SIZE - 1);
            c->code_words[offset / 256] |= UINT64_C(1) << ((offset & 255) / 4);
            c->code_watched[offset >> CODE_PAGE_SHIFT] = 1;
        }
        if (i != count) break;
        ++p->count;
    }
    fclose(file); c->overlay_profile = p;
}

static int profile_matches(const cpu_t *c, const profile_overlay_t *o, uint32_t pc) {
    unsigned j;
    if (pc < o->base || pc - o->base >= o->size) return 0;
    for (j = 0; j < o->count; ++j) {
        uint32_t word;
        memcpy(&word, c->ram + (o->address[j] & (RAM_SIZE - 1)), 4);
        if (word != o->word[j]) return 0;
    }
    return 1;
}

void c_profile_count(cpu_t *c, uint32_t pc, uint32_t n, int compiled) {
    struct overlay_profile *p = c->overlay_profile;
    unsigned page = (pc & (RAM_SIZE - 1)) >> CODE_PAGE_SHIFT, index = 0, i;
    if (!p || !n) return;
    /* An impossible instruction PC distinguishes synthetic BIOS wait ticks
     * from decoded instructions in the legacy interpreter total. */
    if (pc == UINT32_MAX) index = 2;
    else if ((pc & 0x1fffffffu) >= 0x10000 && (pc & 0x1fffffffu) < 0x98000) index = 1;
    else if (p->epoch[page] == c->code_epoch) index = p->cached[page];
    else {
        i = p->cached[page];
        if (i > 2 && profile_matches(c, &p->overlays[i], pc)) index = i;
        else for (i = 3; i < p->count; ++i)
            if (profile_matches(c, &p->overlays[i], pc)) { index = i; break; }
        p->epoch[page] = c->code_epoch; p->cached[page] = index;
    }
    if (compiled) p->overlays[index].recompiled += n;
    else p->overlays[index].interpreted += n;
}

void c_profile_checkpoint(cpu_t *c, uint64_t vblank) {
    struct overlay_profile *p = c->overlay_profile;
    char path[1024];
    FILE *file;
    unsigned i;
    if (!p || (vblank != 1 && vblank % 1000)) return;
    if (snprintf(path, sizeof(path), "%s.timeline.csv", getenv("FF8_OVERLAY_PROFILE")) >= (int)sizeof(path)) return;
    file = fopen(path, p->checkpoints++ ? "a" : "w");
    if (!file) return;
    if (p->checkpoints == 1) fputs("vblank,index,interpreter,recompiled\n", file);
    for (i = 0; i < p->count; ++i) if (p->overlays[i].interpreted || p->overlays[i].recompiled)
        fprintf(file, "%llu,%d,%llu,%llu\n", (unsigned long long)vblank, p->overlays[i].index,
                (unsigned long long)p->overlays[i].interpreted, (unsigned long long)p->overlays[i].recompiled);
    fclose(file);
}

static void overlay_profile_save(cpu_t *c) {
    struct overlay_profile *p = c->overlay_profile;
    FILE *file;
    unsigned i;
    if (!p) return;
    file = fopen(getenv("FF8_OVERLAY_PROFILE"), "w");
    if (file) {
        fputs("index,interpreter,recompiled\n", file);
        for (i = 0; i < p->count; ++i) if (p->overlays[i].interpreted || p->overlays[i].recompiled)
            fprintf(file, "%d,%llu,%llu\n", p->overlays[i].index,
                    (unsigned long long)p->overlays[i].interpreted, (unsigned long long)p->overlays[i].recompiled);
        fclose(file);
    }
    free(p); c->overlay_profile = NULL;
}

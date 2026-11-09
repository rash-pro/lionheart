// One runtime build loaded from a tests/mdec_calendar_harness.c library. Tests
// compare two builds that differ in a flag, so each loads its own copy, private
// to itself through RTLD_LOCAL, with the two copies' C symbols kept apart.
#pragma once

#include "lh_test.h"

#include <cstdint>

struct CalendarModel {
    explicit CalendarModel(const lh::Library &lib)
        : create(lib.fn<void *(unsigned, unsigned, unsigned)>("calendar_create")),
          destroy(lib.fn<void(void *)>("calendar_destroy")),
          advance(lib.fn<void(void *, uint64_t)>("calendar_advance")),
          write(lib.fn<void(void *, uint32_t, unsigned, uint32_t)>("calendar_write")),
          read(lib.fn<uint32_t(void *, uint32_t, unsigned)>("calendar_read")),
          ram_write(lib.fn<void(void *, uint32_t, uint32_t)>("calendar_ram_write")),
          ram_read(lib.fn<uint32_t(void *, uint32_t)>("calendar_ram_read")),
          compare(lib.fn<int(void *, void *)>("calendar_compare")),
          spans(lib.fn<uint64_t(void *)>("calendar_spans")),
          spu_compare(lib.fn<int(void *, void *)>("calendar_spu_compare")),
          irq_count(lib.fn<unsigned()>("calendar_irq_count")),
          irq(lib.fn<uint64_t(unsigned, unsigned)>("calendar_irq")) {}

    void *(*create)(unsigned, unsigned, unsigned);
    void (*destroy)(void *);
    void (*advance)(void *, uint64_t);
    void (*write)(void *, uint32_t, unsigned, uint32_t);
    uint32_t (*read)(void *, uint32_t, unsigned);
    void (*ram_write)(void *, uint32_t, uint32_t);
    uint32_t (*ram_read)(void *, uint32_t);
    int (*compare)(void *, void *);
    uint64_t (*spans)(void *);
    int (*spu_compare)(void *, void *);
    unsigned (*irq_count)();
    uint64_t (*irq)(unsigned, unsigned);
};

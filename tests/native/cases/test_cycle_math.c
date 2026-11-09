#include "cycle_math.h"
uint64_t ratio(uint64_t n, unsigned i) { switch(i) {
case 0: return CYCLE_DIV(n, UINT64_C(263));
case 1: return CYCLE_DIV(n, UINT64_C(314));
case 2: return CYCLE_DIV(n, UINT64_C(526));
case 3: return CYCLE_DIV(n, UINT64_C(628));
case 4: return CYCLE_DIV(n, UINT64_C(62800));
case 5: return CYCLE_DIV(n, UINT64_C(63120000));
case 6: return CYCLE_DIV(n, UINT64_C(33868800));
case 7: return CYCLE_DIV(n, UINT64_C(33902668800));
} return 0; }
uint64_t product(uint64_t a, uint64_t b) { return cycle_mul(a,b); }

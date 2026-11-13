/* Smooth motion's pacing (backends/host/present_pace.h) on synthetic displays:
 * a grid of refreshes, whose period the pacer is told or must find, a
 * compositor that takes a picture only if it was handed over some time before
 * the refresh before its own (a deadline that may drift), reports that come a
 * little after each picture is shown and some that are off the grid, and a
 * picture cost that may change. Prints, per scenario, how many pictures were
 * shown on the refresh they were drawn for, the intervals between them in
 * refreshes, and how evenly the game they show is spaced against the display. */
#include "present_pace.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    double refresh;       /* the display's period */
    double drift;         /* the display's period against the host's clock, relative */
    int known, targets;   /* the pacer is told the period; presents can be held */
    double seconds;
    double deadline[2];   /* how long before the refresh before its own a picture must be handed over: from, to */
    double costs[3];      /* drawing cost in each third of the run */
    double stray;         /* share of reports off the grid */
} scenario_t;

typedef struct {
    double moment, shown;
    int stray;
} sighting_t;

static unsigned rng;
static double uniform(void) { rng = rng * 1103515245u + 12345u; return ((rng >> 8) & 0xffffff) / 16777216.0; }

static void run(const scenario_t *s) {
    present_pace_t pace;
    double period = s->refresh * (1 + s->drift), phase = 0.0037, now = 41253.9, free = now, from = now;
    double last_shown = 0, last_guest = 0, last_any = 0;
    unsigned uneven = 0;
    sighting_t queue[64];
    unsigned pending = 0, i, first = 0, intervals[8] = {0}, counted = 0, on_time = 0, after_warmup = 0;
    unsigned multiples[9] = {0};
    rng = 12345;
    present_pace_start(&pace, s->known ? period : s->refresh, s->known, 1, s->targets);
    while (now < from + s->seconds) {
        present_plan_t plan;
        double cost, repeat, handed, shown, deadline, third = (now - from) / s->seconds * 3;
        int k;
        /* Reports of pictures shown more than two milliseconds ago. */
        while (pending && queue[first % 64].shown + 0.002 <= now) {
            sighting_t *q = &queue[first++ % 64];
            /* Off the grid: another output's refresh, as some compositors report. */
            double report = q->stray ? q->shown - 0.63 * s->refresh : q->shown + 0.00005 * (uniform() - 0.5);
            int stray;
            present_pace_shown(&pace, q->moment, report, 0, &stray);
            --pending;
        }
        present_pace_plan(&pace, now, &plan);
        /* Woken for it up to two milliseconds late (the presenter takes up what
         * the guest queued in between), and planned again then. */
        if (plan.begin > now) { now = (plan.begin < free ? free : plan.begin) + 0.002 * uniform(); continue; }
        cost = s->costs[third < 1 ? 0 : third < 2 ? 1 : 2] * (0.9 + 0.2 * uniform());
        handed = now + cost;
        if (plan.handover > handed) handed = plan.handover;
        /* Each copy of the picture costs a little under a millisecond. */
        repeat = 0.0008 * (0.8 + 0.4 * uniform());
        present_pace_done(&pace, &plan, cost, repeat, handed + plan.repeats * repeat);
        deadline = s->deadline[0] + (s->deadline[1] - s->deadline[0]) * (now - from) / s->seconds;
        {
            /* The first refresh no earlier than the target whose refresh before
             * comes after the deadline. */
            double r = phase + ceil((handed + deadline + period - phase) / period) * period;
            if (plan.target > 0)
                while (r < plan.target - 1e-9) r += period;
            /* A compositor with nothing new for two refreshes takes one more to
             * wake up. */
            if (last_any > 0 && r - last_any > 2.5 * period) r += period;
            shown = r;
        }
        k = (int)floor((shown - plan.moment) / s->refresh + 0.5);
        if (pace.pictures > 30) {
            ++after_warmup;
            if (k == 0) ++on_time;
            if (last_shown > 0) {
                unsigned n = (unsigned)floor((shown - last_shown) / period + 0.5);
                ++intervals[n < 7 ? n : 7]; ++counted;
                /* The game shown against the display: equal steps, except where
                 * the latency follows a change. */
                if (fabs((plan.guest - last_guest) - (shown - last_shown)) > 0.0005) ++uneven;
            }
            ++multiples[plan.multiple < 8 ? plan.multiple : 8];
        }
        last_shown = shown; last_guest = plan.guest;
        last_any = shown + plan.repeats * period;
        if (pending < 64) {
            sighting_t *q = &queue[(first + pending++) % 64];
            q->moment = plan.moment; q->shown = shown;
            q->stray = uniform() < s->stray;
        }
        handed += plan.repeats * repeat;
        now = handed > free ? handed : free;
        free = now;
    }
    printf("%s: pictures=%u on_time=%.2f%%", s->name, after_warmup, after_warmup ? 100.0 * on_time / after_warmup : 0.0);
    printf(" intervals=");
    for (i = 0; i < 8; ++i) if (intervals[i]) printf("%u:%.2f%%,", i, 100.0 * intervals[i] / counted);
    printf(" multiples=");
    for (i = 1; i < 9; ++i) if (multiples[i]) printf("%u:%u,", i, multiples[i]);
    printf(" changes=%lu late=%lu stray=%lu skipped=%lu uneven=%.2f%%\n", pace.changes, pace.late, pace.stray, pace.skipped,
           counted ? 100.0 * uneven / counted : 0.0);
}

int main(int argc, char **argv) {
    static const scenario_t scenarios[] = {
        /* Fixed grids at 60, 120 and 144 Hz, a cheap picture, the deadline
         * drifting by most of a refresh, a fifth of the reports off the grid,
         * the display's clock a little off the host's. */
        {"60", 1 / 60.0, 5e-5, 1, 1, 60, {0.0002, 0.012}, {0.003, 0.003, 0.003}, 0.2},
        {"120", 1 / 120.0, -5e-5, 1, 1, 60, {0.0002, 0.006}, {0.003, 0.003, 0.003}, 0.2},
        {"144", 1 / 144.0, 3e-5, 1, 1, 60, {0.0002, 0.006}, {0.0035, 0.0035, 0.0035}, 0.3},
        /* A picture that does not fit a refresh at 144 Hz: every second. */
        {"144-battle", 1 / 144.0, 3e-5, 1, 1, 60, {0.0005, 0.001}, {0.0085, 0.0085, 0.0085}, 0.3},
        /* From every refresh to every second and back. */
        {"144-change", 1 / 144.0, 0, 1, 1, 60, {0.0005, 0.001}, {0.0035, 0.0085, 0.0035}, 0.1},
        /* A picture that takes more than two refreshes: every few, with copies between. */
        {"144-heavy", 1 / 144.0, 0, 1, 1, 60, {0.0005, 0.001}, {0.016, 0.016, 0.016}, 0.1},
        /* A virtual output whose period the device does not say, a little off
         * the nominal one. */
        {"virtual", 1 / 60.0, 1.3e-3, 0, 1, 60, {0.0002, 0.002}, {0.003, 0.003, 0.003}, 0},
        /* No targets: handed over in the refresh before the one before. */
        {"untargeted", 1 / 60.0, 0, 1, 0, 60, {0.0002, 0.0002}, {0.003, 0.003, 0.003}, 0},
    };
    unsigned i;
    for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); ++i)
        if (argc < 2 || !strcmp(argv[1], scenarios[i].name)) run(&scenarios[i]);
    return 0;
}

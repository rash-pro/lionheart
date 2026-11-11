#include "present_pace.h"
#include <math.h>
#include <string.h>

/* How far a report may be from the grid and still be on it. Reports a little
 * off are noise; some compositors report a few pictures at another output's
 * refresh, well off the grid (strays), which must not move it. */
static double tolerance(const present_pace_t *p) {
    double t = p->timed ? 0.1 * p->refresh : 0.25 * p->refresh;
    return t > 0.0005 ? t : 0.0005;
}

/* How far a is from the grid point nearest it, with the grid's phase at b. */
static double off_grid(double a, double b, double refresh) {
    double d = fmod(a - b, refresh);
    if (d > refresh / 2) d -= refresh;
    if (d < -refresh / 2) d += refresh;
    return d;
}

static double nearest(const present_pace_t *p, double t) {
    return t - off_grid(t, p->phase, p->refresh);
}

/* The first refresh at or after t. */
static double at_or_after(const present_pace_t *p, double t) {
    double r = nearest(p, t);
    return r < t - 1e-9 ? r + p->refresh : r;
}

void present_pace_start(present_pace_t *p, double refresh, int known, int timed, int targets) {
    memset(p, 0, sizeof(*p));
    p->refresh = p->nominal = refresh > 0 ? refresh : 1.0 / 60;
    p->known = known; p->timed = timed; p->targets = targets;
    p->multiple = 1;
    if (!targets) {
        /* A picture handed over during the refresh before the one before its
         * own is taken by the compositor for its own: handed over in the middle
         * of that period, and moved within it as pictures come early or late. */
        p->lead_floor = 1.5 * p->refresh;
    } else {
        /* Held until half a refresh before its own, and handed over before the
         * refresh before it, when the compositor takes what is due: the picture
         * is the compositor's from then, and shown on its own refresh even
         * where the grid is known only to half a refresh. */
        p->lead_floor = p->refresh + 0.002;
    }
    p->lead = p->lead_floor;
    /* Until the grid is known, the host's own clock is the grid. */
    p->locked = !timed;
}

/* Locks onto the grid where most of the reports lie. */
static void lock(present_pace_t *p) {
    unsigned i, j, n = p->sampled < 16 ? p->sampled : 16, best = 0, support = 0;
    double tol = tolerance(p), sum = 0;
    if (n < 8) return;
    for (i = 0; i < n; ++i) {
        unsigned count = 0;
        for (j = 0; j < n; ++j) if (fabs(off_grid(p->samples[j], p->samples[i], p->refresh)) <= tol) ++count;
        if (count > support) { support = count; best = i; }
    }
    if (support * 4 < n * 3) return;
    for (j = 0; j < n; ++j) {
        double d = off_grid(p->samples[j], p->samples[best], p->refresh);
        if (fabs(d) <= tol) sum += d;
    }
    p->phase = p->samples[best] + sum / support;
    p->locked = 1; p->astray = 0;
}

/* About the most a picture took lately: the second most of the last sixteen,
 * so that one slow picture (a load, a page fault) does not move it. */
static double second_most(const double *costs, unsigned long count) {
    unsigned i, n = count < 16 ? (unsigned)count : 16;
    double first = 0, second = 0;
    for (i = 0; i < n; ++i) {
        if (costs[i] > first) { second = first; first = costs[i]; }
        else if (costs[i] > second) second = costs[i];
    }
    return n > 1 ? second : first;
}

/* The latest a picture for moment can be handed over and still be shown then. */
static double deadline(const present_pace_t *p, double moment) {
    return moment - p->refresh - 0.0005;
}

void present_pace_plan(present_pace_t *p, double now, present_plan_t *plan) {
    const double guard = 0.0005;
    double ahead = p->lead + p->cost + guard, moment;
    /* On the grid as it is known now. Begun a little late, a picture still
     * makes its refresh, with less margin; too late to be handed over in time,
     * it is for the first refresh it can still make. */
    moment = p->next > 0 ? nearest(p, p->next) : at_or_after(p, now + ahead);
    if (p->next > 0 && now + p->cost > deadline(p, moment)) {
        double first = at_or_after(p, now + ahead);
        if (first > moment) p->skipped += (unsigned long)floor((first - moment) / p->refresh + 0.5);
        moment = first;
    }
    /* Kept until it is drawn, so that waking for it finds the same moment. */
    p->next = moment;
    /* The game a picture shows is that of the moment it is begun, near enough;
     * held at the same distance from the moment, so that pictures evenly
     * spaced on the display show the game evenly spaced. */
    if (p->latency <= 0 || fabs(ahead - p->latency) > p->refresh) p->latency = ahead;
    else p->latency += 0.05 * (ahead - p->latency);
    plan->moment = moment;
    plan->begin = moment - ahead;
    plan->guest = moment - p->latency;
    plan->multiple = p->multiple;
    plan->target = p->targets ? moment - p->refresh / 2 : 0;
    plan->handover = p->targets ? 0 : moment - p->lead;
    plan->repeats = p->targets ? p->multiple - 1 : 0;
    if (plan->repeats > 4) plan->repeats = 4;
}

/* What a picture every m refreshes costs in a stretch of m refreshes: the
 * picture and the copies until the next. */
static double load(const present_pace_t *p, unsigned m) {
    return p->cost + (p->targets ? (m - 1) * p->repeat : 0);
}

void present_pace_done(present_pace_t *p, const present_plan_t *plan, double cost, double repeat, double now) {
    unsigned m;
    ++p->pictures;
    p->costs[p->pictures % 16] = cost;
    p->cost = second_most(p->costs, p->pictures);
    if (plan->repeats) {
        /* Copies are short and many: what they take on average. */
        unsigned i, n;
        p->repeat_costs[++p->repeated % 16] = repeat;
        n = p->repeated < 16 ? (unsigned)p->repeated : 16;
        for (p->repeat = 0, i = 0; i < n; ++i) p->repeat += p->repeat_costs[i] / n;
    }
    /* A picture every refresh where its cost fits the period with a margin,
     * every second (or more) where it does not. Up at once, as a picture that
     * does not fit makes the next late; down only after two seconds that
     * would have fitted, so that a few cheap pictures do not change it. */
    for (m = 1; m < 8 && load(p, m) > 0.75 * m * p->refresh; ++m) {}
    if (m > p->multiple && load(p, p->multiple) > 0.85 * p->multiple * p->refresh) {
        p->multiple = m; ++p->changes; p->calm = 0;
    } else if (m < p->multiple && load(p, p->multiple - 1) < 0.65 * (p->multiple - 1) * p->refresh) {
        if (p->calm <= 0) p->calm = now;
        else if (now - p->calm >= 2.0) { --p->multiple; ++p->changes; p->calm = 0; }
    } else p->calm = 0;
    /* Back towards the least lead once nothing has been late for a while. */
    if (p->lead > p->lead_floor && now - p->late_at > 5.0) {
        p->lead -= 0.0005;
        if (p->lead < p->lead_floor) p->lead = p->lead_floor;
        p->late_at = now;
    }
    p->next = plan->moment + p->multiple * p->refresh;
}

int present_pace_shown(present_pace_t *p, double moment, double shown, int repeat, int *stray) {
    double tol = tolerance(p);
    int late;
    *stray = 0;
    if (!p->locked) {
        p->samples[p->sampled++ % 16] = shown;
        lock(p);
        return 0;
    }
    {
        double off = off_grid(shown, p->phase, p->refresh), own = nearest(p, moment), at = shown - off;
        if (fabs(off) <= tol) {
            /* On the grid: it follows the display, which may drift against the
             * host's clock; where the device does not say the display's period,
             * so does the period, from the time between pictures on the grid. */
            if (!p->known && p->last_on_grid > 0) {
                double refreshes = floor((at - p->last_on_grid) / p->refresh + 0.5);
                if (refreshes >= 1 && refreshes <= 8) {
                    p->refresh += 0.02 * ((shown - p->last_shown) / refreshes - p->refresh);
                    if (p->refresh < 0.99 * p->nominal) p->refresh = 0.99 * p->nominal;
                    if (p->refresh > 1.01 * p->nominal) p->refresh = 1.01 * p->nominal;
                }
            }
            p->last_on_grid = at; p->last_shown = shown;
            /* Anchored at this refresh, so that the period found matters only
             * over the few refreshes ahead. */
            p->phase = at + 0.05 * off;
            p->astray = 0;
        } else {
            /* A report off the grid (a compositor may report a picture at
             * another output's refresh): it says nothing of when this picture
             * was shown. Many in a row and the grid is found again. */
            *stray = 1;
            if (!repeat) ++p->stray;
            if (++p->astray >= 24) { p->locked = 0; p->sampled = 0; }
            return 0;
        }
        late = (int)floor((at - own) / p->refresh + 0.5);
    }
    if (repeat) return late;
    if (late > 0) {
        ++p->late;
        /* Late: handed over too close to the compositor's deadline. Earlier,
         * where a picture can be held until its refresh. */
        if (p->targets) {
            p->lead += 0.5 * p->refresh;
            if (p->lead > p->lead_floor + 2 * p->refresh) p->lead = p->lead_floor + 2 * p->refresh;
        } else if (p->lead < 2 * p->refresh - 0.0005) p->lead = p->lead_floor = p->lead + 0.25 * p->refresh;
        p->late_at = shown;
    } else if (late < 0) {
        ++p->early;
        /* Without targets, a picture shown a refresh early was handed over
         * before the compositor's deadline for the refresh before the one
         * before its own: later. */
        if (!p->targets && p->lead > p->refresh + 0.25 * p->refresh) p->lead = p->lead_floor = p->lead - 0.25 * p->refresh;
    } else ++p->on_time;
    return late;
}

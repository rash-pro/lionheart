#ifndef HOST_PRESENT_PACE_H
#define HOST_PRESENT_PACE_H
/* Smooth motion's pacing (docs/frame-present.md): which refresh of the display
 * each picture is for, when to begin drawing it and when to hand it over, from
 * when the display showed the pictures before. Times are seconds on the
 * caller's clock. Nothing here sleeps or asks the device: the host feeds it
 * what it sees, so that it can be checked on synthetic displays.
 *
 * A picture is drawn for the moment it will be shown, so pictures may come on
 * every refresh or every few without changing how fast the game moves; the
 * multiple follows what a picture costs. */

typedef struct {
    /* What the display can do, as the host found it. */
    double refresh;     /* refresh period */
    double nominal;     /* the period the host started from */
    int known;          /* the device said the period (else it follows the reports) */
    int timed;          /* reports are when the display showed a picture (else when it was noticed) */
    int targets;        /* a present can be held until a time */
    /* The grid: refreshes at phase + k * refresh, once locked. */
    double phase;
    int locked;
    double samples[16];
    double last_on_grid, last_shown;  /* the latest report on the grid: its refresh, and as reported */
    unsigned sampled, astray;
    /* Pictures. */
    unsigned multiple;  /* a picture every multiple-th refresh */
    double next;        /* the moment of the next picture, 0 before the first */
    double lead;        /* how long before its moment a picture is handed over, at the latest */
    double lead_floor;  /* lead when nothing is late */
    double costs[16];   /* what the last pictures took to draw and hand over */
    double cost;        /* about the most a picture took lately */
    double repeat_costs[16], repeat;  /* what a copy of a picture took lately (plan's repeats), on average */
    unsigned long repeated;
    double latency;     /* from beginning a picture to its moment, smoothed: the game time a picture shows */
    double calm;        /* since when fewer refreshes per picture would do, 0 if they would not */
    double late_at;     /* when a picture was last late */
    /* Totals. */
    unsigned long pictures, on_time, late, early, stray, skipped, changes;
} present_pace_t;

typedef struct {
    double begin;       /* begin drawing at (may be past) */
    double moment;      /* the refresh it is for */
    double target;      /* held until then (0: not held) */
    double handover;    /* handed over at (0: as soon as it is drawn) */
    double guest;       /* the moment whose game it shows */
    unsigned multiple;
    /* Copies of the picture for the refreshes before the next, each held until
     * its own: a compositor that has nothing new for a refresh may need one more
     * to wake up, which would make the next picture late. */
    unsigned repeats;
} present_plan_t;

/* Starts over with a display (see the fields above for the flags). */
void present_pace_start(present_pace_t *, double refresh, int known, int timed, int targets);
/* The next picture, as things stand at now. */
void present_pace_plan(present_pace_t *, double now, present_plan_t *);
/* The planned picture was drawn and handed over, which took cost (not counting
 * any wait for its hand-over or its repeats), and its repeats repeat each; now
 * is when that ended. */
void present_pace_done(present_pace_t *, const present_plan_t *, double cost, double repeat, double now);
/* A picture for moment was seen at shown (repeat: a copy of the picture before,
 * which counts for nothing but the grid). Returns how many refreshes after its
 * own it was shown (0 on time, negative early).
 * stray is set, and 0 returned, for a report off the display's grid, which
 * says nothing of when the picture was shown. */
int present_pace_shown(present_pace_t *, double moment, double shown, int repeat, int *stray);
#endif

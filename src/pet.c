/*
 * pet.c - a desktop pet for the ATK-MP157 (STM32MP157) RGB screen. v4
 *
 * Architecture (validated with probe.c):
 *   - /dev/dri/card0, DRM master
 *   - the currently unused overlay plane (crtc_id == 0) is driven with the
 *     classic SETPLANE ioctl, ARGB8888 dumb buffer, double buffered
 *   - the plane covers the whole screen; only the pet, the butterfly, the
 *     food and the launcher icon have non-zero alpha, so the ALIENTEK Qt
 *     desktop shows through everywhere else - nothing else is disturbed
 *   - touch input comes straight from /dev/input/event0 (Goodix GT9147)
 *
 * Two modes:
 *   LAUNCHER - a cat-face app icon sits in the bottom-right corner. The
 *              touchscreen is NOT grabbed, so the Qt desktop stays fully
 *              usable. Tap the icon to launch the pet.
 *   PET      - the pet is alive and grabs the touchscreen exclusively, so
 *              touching it never reaches the UI behind it.
 *
 * Pet interactions:
 *   tap the pet        -> hop + hearts (3 quick taps -> it falls asleep)
 *   drag the pet       -> it follows your finger
 *   tap the desktop    -> drops cat food; the pet walks over and eats
 *   a butterfly        -> flutters by now and then; the pet may chase it
 *   long-press 1.2s    -> the pet exits back to the launcher icon
 *   asleep             -> grab released; the desktop is usable again,
 *                         touching the pet wakes it
 *
 * Build: arm-linux-gnueabihf-gcc -static -Os -o pet pet.c -lm
 * Stop:  Ctrl-C / SIGTERM (plane is detached cleanly on exit)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <linux/input.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

#ifndef DRM_MODE_CONNECTED
#define DRM_MODE_CONNECTED 1
#endif

/* ------------------------------------------------------------------ */
#define PET_BASE   112
#define PET_SCALE  1.571f                  /* -> 176 px on screen */
#define PET_W      ((int)(PET_BASE * PET_SCALE))
#define PET_H      PET_W
#define ICON_SZ    76
#define FPS        12
#define FRAME_NS   (1000000000L / FPS)

/* colours (straight alpha) */
#define FUR        245, 166,  66
#define FUR_DARK   214, 138,  44
#define CREAM      255, 238, 205
#define EYE         62,  54,  48
#define COLLAR     226,  72,  62
#define BELL       255, 214,  80

typedef enum { ST_IDLE, ST_WALK, ST_SIT, ST_SLEEP, ST_DRAG, ST_HAPPY,
               ST_CHASE, ST_EAT } pet_state;
typedef enum { MODE_LAUNCHER, MODE_PET } app_mode;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

/* ------------------------------------------------------------------ */
/* software renderer into an ARGB8888 dumb buffer                     */
typedef struct {
    uint32_t *px;
    int stride;
    int w, h;
} surface;

static float PS = PET_SCALE;
#define SX(v) ((v) * PS)

static void blend(surface *s, int x, int y, int a, int r, int g, int b)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h || a <= 0)
        return;
    uint32_t *d = &s->px[y * s->stride + x];
    uint32_t dc = *d;
    int da = (dc >> 24) & 0xff;
    int dr = (dc >> 16) & 0xff;
    int dg = (dc >> 8) & 0xff;
    int db = dc & 0xff;
    int oa = a + da * (255 - a) / 255;
    if (oa <= 0) { *d = 0; return; }
    if (oa > 255) oa = 255;
    int orr = (r * a + dr * da * (255 - a) / 255) / oa;
    int og  = (g * a + dg * da * (255 - a) / 255) / oa;
    int ob  = (b * a + db * da * (255 - a) / 255) / oa;
    *d = ((uint32_t)oa << 24) | ((uint32_t)orr << 16) | ((uint32_t)og << 8) | (uint32_t)ob;
}

static void ellipse(surface *s, float cx, float cy, float rx, float ry,
                    int a, int r, int g, int b)
{
    if (rx <= 0 || ry <= 0) return;
    int x0 = (int)(cx - rx) - 1, x1 = (int)(cx + rx) + 1;
    int y0 = (int)(cy - ry) - 1, y1 = (int)(cy + ry) + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = (x - cx) / rx, dy = (y - cy) / ry;
            float d2 = dx * dx + dy * dy;
            if (d2 <= 1.0f) {
                int aa = (int)(a * (1.0f - (d2 > 0.72f ? (d2 - 0.72f) / 0.28f * 0.4f : 0.0f)));
                if (aa > 0) blend(s, x, y, aa, r, g, b);
            }
        }
}

static void circle(surface *s, float cx, float cy, float r, int a, int R, int G, int B)
{
    ellipse(s, cx, cy, r, r, a, R, G, B);
}

static void seg(surface *s, float x0, float y0, float x1, float y1,
                float w, int a, int r, int g, int b)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    int steps = (int)(len / 1.5f) + 1;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / steps;
        circle(s, x0 + dx * t, y0 + dy * t, w, a, r, g, b);
    }
}

static void tri(surface *s, float x0, float y0, float x1, float y1,
                float x2, float y2, int a, int r, int g, int b)
{
    int minx = (int)fminf(fminf(x0, x1), x2) - 1, maxx = (int)fmaxf(fmaxf(x0, x1), x2) + 1;
    int miny = (int)fminf(fminf(y0, y1), y2) - 1, maxy = (int)fmaxf(fmaxf(y0, y1), y2) + 1;
    float d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (fabsf(d) < 1e-6f) return;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            float l1 = ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) / d;
            float l2 = ((y2 - y0) * (x - x2) + (x0 - x2) * (y - y2)) / d;
            if (l1 >= 0 && l2 >= 0 && l1 + l2 <= 1)
                blend(s, x, y, a, r, g, b);
        }
}

/* ------------------------------------------------------------------ */
/* entities                                                            */
typedef struct {
    float x, y;
    float tx, ty;
    pet_state st;
    double state_time;
    double idle_time;
    double blink_at, blink_t0;
    double happy_t0, z_t0, sit_t0;
    double chew_t0;
    int facing;
    float walk_phase;
    float hop;
    float squash;
    float ear_twitch;
    int look_x, look_y;
    bool dragging;
    float grab_dx, grab_dy;
} pet;

typedef struct {
    float x, y, vx, vy;
    double born;
    bool alive;
    float phase;
} butterfly;

typedef struct {
    float x, y;
    float amount;
    bool active;
} food;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

/* ------------------------------------------------------------------ */
/* the launcher icon: a round app button with a cat face               */
static float icon_x, icon_y;

static void draw_launcher_icon(surface *s, double t)
{
    float pulse = 1.0f + sinf((float)t * 2.2f) * 0.035f;
    float cx = icon_x + ICON_SZ / 2, cy = icon_y + ICON_SZ / 2;
    float R = ICON_SZ / 2 * pulse;

    /* drop shadow */
    circle(s, cx + 2, cy + 4, R, 70, 0, 0, 0);
    /* dark round button */
    circle(s, cx, cy, R, 215, 38, 40, 52);
    circle(s, cx, cy, R, 255, 70, 74, 92);
    circle(s, cx, cy, R - 3, 255, 38, 40, 52);
    /* cat face */
    float fr = R * 0.52f;                 /* face radius */
    float fy = cy + R * 0.08f;
    tri(s, cx - fr * 0.85f, fy - fr * 0.55f, cx - fr * 0.38f, fy - fr * 1.35f, cx - fr * 0.05f, fy - fr * 0.55f, 255, FUR_DARK);
    tri(s, cx + fr * 0.85f, fy - fr * 0.55f, cx + fr * 0.38f, fy - fr * 1.35f, cx + fr * 0.05f, fy - fr * 0.55f, 255, FUR_DARK);
    tri(s, cx - fr * 0.68f, fy - fr * 0.58f, cx - fr * 0.38f, fy - fr * 1.12f, cx - fr * 0.22f, fy - fr * 0.58f, 255, 255, 170, 185);
    tri(s, cx + fr * 0.68f, fy - fr * 0.58f, cx + fr * 0.38f, fy - fr * 1.12f, cx + fr * 0.22f, fy - fr * 0.58f, 255, 255, 170, 185);
    circle(s, cx, fy + fr * 0.1f, fr, 255, FUR);
    /* eyes (blink slowly) */
    bool blink = fmodf((float)t, 3.7f) < 0.12f;
    if (blink) {
        seg(s, cx - fr * 0.55f, fy - fr * 0.1f, cx - fr * 0.15f, fy - fr * 0.1f, R * 0.045f, 255, EYE);
        seg(s, cx + fr * 0.15f, fy - fr * 0.1f, cx + fr * 0.55f, fy - fr * 0.1f, R * 0.045f, 255, EYE);
    } else {
        circle(s, cx - fr * 0.35f, fy - fr * 0.12f, R * 0.085f, 255, EYE);
        circle(s, cx + fr * 0.35f, fy - fr * 0.12f, R * 0.085f, 255, EYE);
        circle(s, cx - fr * 0.38f, fy - fr * 0.17f, R * 0.032f, 235, 255, 255, 255);
        circle(s, cx + fr * 0.32f, fy - fr * 0.17f, R * 0.032f, 235, 255, 255, 255);
    }
    /* nose + w mouth */
    tri(s, cx - R * 0.035f, fy + fr * 0.22f, cx + R * 0.035f, fy + fr * 0.22f, cx, fy + fr * 0.36f, 255, 70, 55, 55);
    seg(s, cx - R * 0.07f, fy + fr * 0.45f, cx - R * 0.02f, fy + fr * 0.58f, R * 0.028f, 230, 90, 60, 60);
    seg(s, cx + R * 0.07f, fy + fr * 0.45f, cx + R * 0.02f, fy + fr * 0.58f, R * 0.028f, 230, 90, 60, 60);
    /* whiskers */
    seg(s, cx - fr * 0.75f, fy + fr * 0.25f, cx - fr * 1.25f, fy + fr * 0.12f, R * 0.022f, 160, 255, 255, 255);
    seg(s, cx - fr * 0.75f, fy + fr * 0.4f, cx - fr * 1.25f, fy + fr * 0.48f, R * 0.022f, 160, 255, 255, 255);
    seg(s, cx + fr * 0.75f, fy + fr * 0.25f, cx + fr * 1.25f, fy + fr * 0.12f, R * 0.022f, 160, 255, 255, 255);
    seg(s, cx + fr * 0.75f, fy + fr * 0.4f, cx + fr * 1.25f, fy + fr * 0.48f, R * 0.022f, 160, 255, 255, 255);
}

/* ------------------------------------------------------------------ */
static void draw_pet(surface *s, pet *p, double t)
{
    float ox = p->x, oy = p->y;
    float bob = 0.0f;

    if (p->st == ST_WALK || p->st == ST_CHASE)
        bob = sinf(p->walk_phase * 2) * 2.5f;
    else if (p->st == ST_SLEEP)
        bob = sinf((float)t * 1.2f) * 1.5f;
    else if (p->st == ST_SIT)
        bob = sinf((float)t * 1.6f) * 0.8f;
    else
        bob = sinf((float)t * 2.0f) * 1.0f;

    float sq = p->squash;

    ellipse(s, ox + SX(56), oy + SX(106), SX(26) - p->hop * 0.12f, SX(6), 40, 0, 0, 0);

    float cx = ox + SX(56), cy = oy + SX(60) + bob + p->hop;
    bool sitting = (p->st == ST_SIT || p->st == ST_EAT);
    if (sitting) cy += SX(5);

    /* tail */
    float wag = sinf((float)t * (p->st == ST_WALK || p->st == ST_CHASE ? 7.0f : 2.2f)) * 0.5f;
    float tdir = p->facing;
    float bx = cx + SX(26) * tdir, by = cy + SX(12);
    float mx = bx + SX(16) * tdir, my = by - SX(10) + wag * SX(8);
    float ex = mx + SX(14) * tdir + wag * SX(6), ey = my - SX(12);
    for (int i = 0; i <= 12; i++) {
        float u = (float)i / 12, v = 1 - u;
        float x = v * v * bx + 2 * v * u * mx + u * u * ex;
        float y = v * v * by + 2 * v * u * my + u * u * ey;
        circle(s, x, y, SX(6.5f) - u * SX(2), 255, FUR);
    }
    circle(s, ex, ey, SX(4), 255, CREAM);

    /* legs */
    float swing = (p->st == ST_WALK || p->st == ST_CHASE) ? sinf(p->walk_phase) * SX(5) : 0;
    float lift1 = (p->st == ST_WALK || p->st == ST_CHASE) ? fabsf(cosf(p->walk_phase)) * SX(2) : 0;
    float lift2 = (p->st == ST_WALK || p->st == ST_CHASE) ? fabsf(sinf(p->walk_phase)) * SX(2) : 0;
    if (sitting) {
        ellipse(s, cx - SX(9), cy + SX(24), SX(6), SX(5), 255, FUR_DARK);
        ellipse(s, cx + SX(9), cy + SX(24), SX(6), SX(5), 255, FUR_DARK);
    } else {
        ellipse(s, cx - SX(13) + swing, cy + SX(26) - lift1, SX(8), SX(7.5f), 255, FUR_DARK);
        ellipse(s, cx + SX(13) - swing, cy + SX(26) - lift2, SX(8), SX(7.5f), 255, FUR_DARK);
    }

    /* body */
    if (sitting) {
        ellipse(s, cx, cy + SX(2), SX(27) / sq, SX(29) * sq, 255, FUR);
        ellipse(s, cx, cy + SX(10), SX(16) / sq, SX(14) * sq, 255, CREAM);
    } else {
        ellipse(s, cx, cy, SX(30) / sq, SX(27) * sq, 255, FUR);
        ellipse(s, cx, cy + SX(8), SX(17) / sq, SX(12) * sq, 255, CREAM);
    }

    /* ears */
    float et = p->facing * SX(4) + p->ear_twitch * SX(3);
    tri(s, cx - SX(24), cy - SX(18), cx - SX(10) + et, cy - SX(40), cx - SX(4), cy - SX(16), 255, FUR_DARK);
    tri(s, cx + SX(24), cy - SX(18), cx + SX(10) + et, cy - SX(40), cx + SX(4), cy - SX(16), 255, FUR_DARK);
    tri(s, cx - SX(19), cy - SX(19), cx - SX(10) + et, cy - SX(33), cx - SX(9), cy - SX(19), 255, 255, 170, 185);
    tri(s, cx + SX(19), cy - SX(19), cx + SX(10) + et, cy - SX(33), cx + SX(9), cy - SX(19), 255, 255, 170, 185);

    /* collar + bell */
    for (int x = 30; x <= 82; x++) {
        float xx = cx + SX(x - 56);
        float yy = cy + SX(20) + SX((x - 56) * (x - 56) * 0.0022f);
        for (int k = 0; k < 5; k++) blend(s, (int)xx, (int)yy + k, 255, COLLAR);
    }
    circle(s, cx, cy + SX(26), SX(5.5f), 255, BELL);
    seg(s, cx - SX(3), cy + SX(27), cx + SX(3), cy + SX(27), SX(1), 255, EYE);
    circle(s, cx - SX(1.5f), cy + SX(24.5f), SX(1.2f), 220, 255, 255, 255);

    /* face */
    float ey_y = cy - SX(4);
    float ex_l = cx - SX(11), ex_r = cx + SX(11);
    bool blink = (t - p->blink_t0) < 0.14f;
    bool sleeping = (p->st == ST_SLEEP);
    bool eating = (p->st == ST_EAT);
    int lookx = p->look_x, looky = p->look_y;

    if (sleeping || blink) {
        seg(s, ex_l - SX(5), ey_y, ex_l + SX(5), ey_y, SX(1.6f), 255, EYE);
        seg(s, ex_r - SX(5), ey_y, ex_r + SX(5), ey_y, SX(1.6f), 255, EYE);
        if (sleeping) {
            seg(s, ex_l + SX(2), ey_y - SX(4), ex_l + SX(6), ey_y + SX(1), SX(1.2f), 200, EYE);
            seg(s, ex_r - SX(2), ey_y - SX(4), ex_r - SX(6), ey_y + SX(1), SX(1.2f), 200, EYE);
        }
    } else {
        circle(s, ex_l, ey_y, SX(6), 255, EYE);
        circle(s, ex_r, ey_y, SX(6), 255, EYE);
        circle(s, ex_l + lookx * SX(2), ey_y + looky * SX(1.6f), SX(2.6f), 255, 20, 20, 24);
        circle(s, ex_r + lookx * SX(2), ey_y + looky * SX(1.6f), SX(2.6f), 255, 20, 20, 24);
        circle(s, ex_l - SX(2), ey_y - SX(2.4f), SX(1.7f), 235, 255, 255, 255);
        circle(s, ex_r - SX(2), ey_y - SX(2.4f), SX(1.7f), 235, 255, 255, 255);
    }

    circle(s, ex_l - SX(6), ey_y + SX(10), SX(4.5f), 92, 255, 130, 140);
    circle(s, ex_r + SX(6), ey_y + SX(10), SX(4.5f), 92, 255, 130, 140);

    tri(s, cx - SX(2), cy + SX(4), cx + SX(2), cy + SX(4), cx, cy + SX(7), 255, 70, 55, 55);
    if (eating) {
        float mo = 2.5f + fabsf(sinf((float)(t - p->chew_t0) * 9.0f)) * 3.0f;
        ellipse(s, cx, cy + SX(10) + mo * 0.3f, SX(4.5f), SX(mo), 235, 120, 60, 60);
    } else if (p->st == ST_HAPPY || p->st == ST_DRAG) {
        ellipse(s, cx, cy + SX(11), SX(4.5f), SX(3.5f), 230, 120, 60, 60);
    } else if (sleeping) {
        seg(s, cx - SX(3), cy + SX(10), cx + SX(3), cy + SX(10), SX(1.2f), 220, 120, 70, 70);
    } else {
        seg(s, cx - SX(4), cy + SX(8), cx - SX(1), cy + SX(11), SX(1.2f), 230, 90, 60, 60);
        seg(s, cx + SX(4), cy + SX(8), cx + SX(1), cy + SX(11), SX(1.2f), 230, 90, 60, 60);
    }

    seg(s, cx - SX(20), cy + SX(6), cx - SX(32), cy + SX(3), SX(0.8f), 170, 255, 255, 255);
    seg(s, cx - SX(20), cy + SX(9), cx - SX(32), cy + SX(10), SX(0.8f), 170, 255, 255, 255);
    seg(s, cx + SX(20), cy + SX(6), cx + SX(32), cy + SX(3), SX(0.8f), 170, 255, 255, 255);
    seg(s, cx + SX(20), cy + SX(9), cx + SX(32), cy + SX(10), SX(0.8f), 170, 255, 255, 255);

    if (sleeping) {
        for (int i = 0; i < 3; i++) {
            float ph = fmodf((float)(t - p->z_t0) * 0.6f + i * 0.33f, 1.0f);
            float zx = cx + SX(30) + ph * SX(14), zy = cy - SX(34) - ph * SX(26);
            int za = (int)(255 * (1.0f - ph));
            float zs = SX(5) + ph * SX(3);
            if (za > 8) {
                seg(s, zx - zs, zy - zs * 0.6f, zx + zs, zy - zs * 0.6f, SX(1.3f), za, 110, 130, 190);
                seg(s, zx + zs, zy - zs * 0.6f, zx - zs, zy + zs * 0.6f, SX(1.3f), za, 110, 130, 190);
                seg(s, zx - zs, zy + zs * 0.6f, zx + zs, zy + zs * 0.6f, SX(1.3f), za, 110, 130, 190);
            }
        }
    }

    if (p->st == ST_HAPPY) {
        for (int i = 0; i < 3; i++) {
            float ph = fmodf((float)(t - p->happy_t0) * 0.9f + i * 0.3f, 1.0f);
            float hx = cx - SX(16) + i * SX(16) + sinf(ph * 6.0f) * SX(4);
            float hy = cy - SX(40) - ph * SX(30);
            int ha = (int)(255 * (1.0f - ph));
            float hs = SX(3) + ph * SX(2);
            if (ha > 8) {
                circle(s, hx - hs * 0.5f, hy, hs * 0.55f, ha, 255, 105, 120);
                circle(s, hx + hs * 0.5f, hy, hs * 0.55f, ha, 255, 105, 120);
                tri(s, hx - hs, hy + hs * 0.2f, hx + hs, hy + hs * 0.2f, hx, hy + hs * 1.4f, ha, 255, 105, 120);
            }
        }
    }
}

static void draw_butterfly(surface *s, butterfly *b, double t)
{
    if (!b->alive) return;
    float flap = 0.55f + 0.45f * fabsf(sinf((float)t * 22 + b->phase));
    float bx = b->x, by = b->y + sinf((float)t * 5 + b->phase) * 3.0f;
    ellipse(s, bx - 6 * flap, by - 3, 7 * flap, 5, 235, 255, 255, 255);
    ellipse(s, bx + 6 * flap, by - 3, 7 * flap, 5, 235, 255, 255, 255);
    ellipse(s, bx - 4 * flap, by + 3, 4.5f * flap, 3.5f, 220, 200, 220, 255);
    ellipse(s, bx + 4 * flap, by + 3, 4.5f * flap, 3.5f, 220, 200, 220, 255);
    circle(s, bx - 6 * flap, by - 4, 1.6f, 230, 120, 140, 220);
    circle(s, bx + 6 * flap, by - 4, 1.6f, 230, 120, 140, 220);
    ellipse(s, bx, by, 1.6f, 4.5f, 255, 70, 60, 60);
    seg(s, bx - 2.5f, by - 4.5f, bx - 4.5f, by - 7, 0.7f, 220, 70, 60, 60);
    seg(s, bx + 2.5f, by - 4.5f, bx + 4.5f, by - 7, 0.7f, 220, 70, 60, 60);
}

static void draw_food(surface *s, food *f)
{
    if (!f->active || f->amount <= 0) return;
    float x = f->x, y = f->y;
    ellipse(s, x, y + 4, 14, 9, 255, 210, 70, 60);
    ellipse(s, x, y + 2, 11, 6, 255, 240, 240, 235);
    int n = (int)(f->amount * 7.0f + 0.5f);
    for (int i = 0; i < n; i++) {
        float a = i * 2.39996f;
        circle(s, x + cosf(a) * 6.5f, y - 2 + sinf(a) * 2.5f, 2.2f, 255, 150, 100, 60);
    }
}

/* ------------------------------------------------------------------ */
/* DRM                                                                 */
static int drm_fd = -1;
static uint32_t plane_id, crtc_id;
static uint32_t fb_ids[2];
static uint32_t dumb_handles[2];
static uint32_t *fb_ptr[2];
static uint32_t fb_pitch[2];
static size_t fb_size[2];
static int cur_buf;
static int screen_w = 1024, screen_h = 600;

static int drm_setup(void)
{
    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("open card0"); return -1; }
    if (ioctl(drm_fd, DRM_IOCTL_SET_MASTER, 0) < 0)
        fprintf(stderr, "pet: SET_MASTER failed (%s), continuing\n", strerror(errno));

    struct drm_mode_get_plane_res pr;
    memset(&pr, 0, sizeof(pr));
    if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0 || pr.count_planes == 0) {
        fprintf(stderr, "pet: no planes\n"); return -1;
    }
    uint32_t *pids = calloc(pr.count_planes, sizeof(uint32_t));
    pr.plane_id_ptr = (uint64_t)(uintptr_t)pids;
    ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
    plane_id = 0;
    for (uint32_t i = 0; i < pr.count_planes; i++) {
        struct drm_mode_get_plane p;
        memset(&p, 0, sizeof(p));
        p.plane_id = pids[i];
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETPLANE, &p) == 0 && p.crtc_id == 0) {
            plane_id = pids[i];
            break;
        }
    }
    free(pids);
    if (!plane_id) { fprintf(stderr, "pet: no free overlay plane\n"); return -1; }

    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    uint32_t *cids = calloc(res.count_crtcs ? res.count_crtcs : 1, sizeof(uint32_t));
    res.crtc_id_ptr = (uint64_t)(uintptr_t)cids;
    ioctl(drm_fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    crtc_id = 0;
    for (uint32_t i = 0; i < res.count_crtcs; i++) {
        struct drm_mode_crtc cr;
        memset(&cr, 0, sizeof(cr));
        cr.crtc_id = cids[i];
        if (ioctl(drm_fd, DRM_IOCTL_MODE_GETCRTC, &cr) == 0 && cr.mode_valid) {
            crtc_id = cids[i];
            screen_w = cr.mode.hdisplay;
            screen_h = cr.mode.vdisplay;
            break;
        }
    }
    free(cids);
    if (!crtc_id) { fprintf(stderr, "pet: no active crtc\n"); return -1; }
    fprintf(stderr, "pet: plane %u crtc %u mode %dx%d\n", plane_id, crtc_id, screen_w, screen_h);

    for (int i = 0; i < 2; i++) {
        struct drm_mode_create_dumb cd;
        memset(&cd, 0, sizeof(cd));
        cd.width = screen_w;
        cd.height = screen_h;
        cd.bpp = 32;
        if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
            perror("pet: CREATE_DUMB"); return -1;
        }
        dumb_handles[i] = cd.handle;
        fb_pitch[i] = cd.pitch;
        fb_size[i] = cd.size;
        struct drm_mode_fb_cmd2 fb;
        memset(&fb, 0, sizeof(fb));
        fb.width = screen_w;
        fb.height = screen_h;
        fb.pixel_format = DRM_FORMAT_ARGB8888;
        fb.handles[0] = cd.handle;
        fb.pitches[0] = cd.pitch;
        if (ioctl(drm_fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
            perror("pet: ADDFB2"); return -1;
        }
        fb_ids[i] = fb.fb_id;
        struct drm_mode_map_dumb md;
        memset(&md, 0, sizeof(md));
        md.handle = cd.handle;
        if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
            perror("pet: MAP_DUMB"); return -1;
        }
        fb_ptr[i] = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, md.offset);
        if (fb_ptr[i] == MAP_FAILED) { perror("pet: mmap"); return -1; }
    }
    fprintf(stderr, "pet: %dx%d ARGB8888 full-screen overlay ready\n", screen_w, screen_h);
    return 0;
}

static void plane_show(int buf)
{
    struct drm_mode_set_plane sp;
    memset(&sp, 0, sizeof(sp));
    sp.plane_id = plane_id;
    sp.crtc_id = crtc_id;
    sp.fb_id = fb_ids[buf];
    sp.crtc_x = 0;
    sp.crtc_y = 0;
    sp.crtc_w = screen_w;
    sp.crtc_h = screen_h;
    sp.src_x = 0;
    sp.src_y = 0;
    sp.src_w = screen_w << 16;
    sp.src_h = screen_h << 16;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_SETPLANE, &sp) < 0)
        perror("pet: SETPLANE");
}

static void plane_hide(void)
{
    struct drm_mode_set_plane sp;
    memset(&sp, 0, sizeof(sp));
    sp.plane_id = plane_id;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_SETPLANE, &sp) < 0)
        perror("pet: SETPLANE(disable)");
}

static void drm_cleanup(void)
{
    plane_hide();
    for (int i = 0; i < 2; i++) {
        if (fb_ptr[i] && fb_ptr[i] != MAP_FAILED) munmap(fb_ptr[i], fb_size[i]);
        if (fb_ids[i]) ioctl(drm_fd, DRM_IOCTL_MODE_RMFB, &fb_ids[i]);
        if (dumb_handles[i]) {
            struct drm_mode_destroy_dumb dd = { dumb_handles[i] };
            ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        }
    }
    ioctl(drm_fd, DRM_IOCTL_DROP_MASTER, 0);
    close(drm_fd);
}

/* ------------------------------------------------------------------ */
/* touch                                                               */
static int touch_open(void)
{
    for (int idx = 0; idx < 6; idx++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", idx);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        char name[256] = "";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        if (strcasestr(name, "touch") || strcasestr(name, "Goodix")) {
            fprintf(stderr, "pet: touch device %s (%s)\n", path, name);
            return fd;
        }
        close(fd);
    }
    return -1;
}

typedef struct {
    bool active;
    int x, y;
} touch_t;

static int g_grabbed = 0;

static void set_grab(int fd, int on)
{
    if (fd < 0) return;
    if (ioctl(fd, EVIOCGRAB, on ? 1 : 0) < 0) {
        fprintf(stderr, "pet: EVIOCGRAB(%d) failed: %s\n", on, strerror(errno));
        return;
    }
    g_grabbed = on ? 1 : 0;
    fprintf(stderr, "pet: touchscreen %s\n", on ? "grabbed (desktop locked)" : "released (desktop usable)");
}

static void touch_poll(int fd, touch_t *t, bool *pressed, bool *released,
                       int *down_x, int *down_y, double *down_t, double tnow)
{
    struct input_event ev[64];
    ssize_t n = read(fd, ev, sizeof(ev));
    if (n <= 0) return;
    for (ssize_t i = 0; i + (ssize_t)sizeof(ev[0]) <= n; i += sizeof(ev[0])) {
        struct input_event *e = &ev[i / sizeof(ev[0])];
        if (e->type == EV_ABS) {
            switch (e->code) {
            case ABS_X:
            case ABS_MT_POSITION_X:
                t->x = e->value;
                if (t->x > screen_w - 1) t->x = screen_w - 1;
                if (t->x < 0) t->x = 0;
                break;
            case ABS_Y:
            case ABS_MT_POSITION_Y:
                t->y = e->value;
                if (t->y > screen_h - 1) t->y = screen_h - 1;
                if (t->y < 0) t->y = 0;
                break;
            case ABS_MT_TRACKING_ID:
                if (e->value == -1) {
                    if (t->active) *released = true;
                    t->active = false;
                } else if (!t->active) {
                    t->active = true;
                    *pressed = true;
                    *down_x = t->x; *down_y = t->y; *down_t = tnow;
                }
                break;
            }
        } else if (e->type == EV_KEY && e->code == BTN_TOUCH) {
            if (e->value == 1 && !t->active) {
                t->active = true;
                *pressed = true;
                *down_x = t->x; *down_y = t->y; *down_t = tnow;
            } else if (e->value == 0 && t->active) {
                t->active = false;
                *released = true;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
int main(void)
{
    int lock_fd = open("/tmp/.pet.lock", O_CREAT | O_RDWR, 0666);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "pet: another instance is running\n");
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    srand((unsigned)time(NULL));

    if (drm_setup() < 0) return 1;
    int tfd = touch_open();

    icon_x = (float)(screen_w - ICON_SZ - 24);
    icon_y = (float)(screen_h - ICON_SZ - 24);

    pet p;
    memset(&p, 0, sizeof(p));
    p.x = screen_w / 2 - PET_W / 2;
    p.y = screen_h - PET_H - 24;
    p.tx = p.x; p.ty = p.y;
    p.st = ST_IDLE;
    p.blink_at = 3.0;
    p.squash = 1.0f;
    p.facing = 1;

    butterfly bf;
    memset(&bf, 0, sizeof(bf));
    bf.born = -100;
    double bf_respawn = now_s() + 6.0 + frand() * 8.0;

    food fd;
    memset(&fd, 0, sizeof(fd));

    app_mode mode = MODE_LAUNCHER;
    touch_t ts = { false, 0, 0 };
    bool pressed = false, released = false;
    bool want_exit = false;
    int down_x = 0, down_y = 0;
    double down_t = 0;
    int tap_count = 0;
    double last_tap_t = -10;

    surface surf = { fb_ptr[0], (int)(fb_pitch[0] / 4), screen_w, screen_h };
    double t_prev = now_s();

    fprintf(stderr, "pet: launcher icon ready - tap it to summon the pet!\n");

    while (!g_stop) {
        double t = now_s();
        float dt = (float)(t - t_prev);
        if (dt > 0.25f) dt = 0.25f;
        t_prev = t;

        pressed = released = false;
        if (tfd >= 0)
            touch_poll(tfd, &ts, &pressed, &released, &down_x, &down_y, &down_t, t);

        p.state_time += dt;
        p.idle_time += dt;
        if (p.ear_twitch != 0) {
            p.ear_twitch *= 0.92f;
            if (fabsf(p.ear_twitch) < 0.05f) p.ear_twitch = 0;
        }
        if (t > p.blink_at) {
            p.blink_t0 = t;
            p.blink_at = t + 2.5 + frand() * 3.0;
        }

        /* ================= LAUNCHER MODE ================= */
        if (mode == MODE_LAUNCHER) {
            if (pressed) {
                if (down_x >= icon_x - 6 && down_x <= icon_x + ICON_SZ + 6 &&
                    down_y >= icon_y - 6 && down_y <= icon_y + ICON_SZ + 6) {
                    /* summon the pet right out of the icon */
                    mode = MODE_PET;
                    fd.active = false;
                    p.x = icon_x + ICON_SZ / 2 - PET_W / 2;
                    p.y = icon_y + ICON_SZ / 2 - PET_H / 2;
                    p.tx = p.x; p.ty = p.y;
                    p.st = ST_HAPPY;
                    p.happy_t0 = t;
                    p.state_time = 0;
                    p.idle_time = 0;
                    set_grab(tfd, 1);
                    fprintf(stderr, "pet: hello! (long-press me to send me back)\n");
                }
                /* other touches belong to the desktop */
            }
        }

        /* ================= PET MODE ================= */
        else {
            /* ---- touch input ---- */
            if (pressed) {
                p.idle_time = 0;
                if (p.st == ST_SLEEP) {
                    /* asleep + ungranted: only a touch on the pet wakes it */
                    if (down_x >= p.x - 10 && down_x <= p.x + PET_W + 10 &&
                        down_y >= p.y - 10 && down_y <= p.y + PET_H + 10) {
                        set_grab(tfd, 1);
                        p.st = ST_DRAG;
                        p.dragging = true;
                        p.grab_dx = (float)down_x - p.x;
                        p.grab_dy = (float)down_y - p.y;
                    }
                } else if (down_x >= p.x - 10 && down_x <= p.x + PET_W + 10 &&
                           down_y >= p.y - 10 && down_y <= p.y + PET_H + 10) {
                    p.st = ST_DRAG;
                    p.dragging = true;
                    p.grab_dx = (float)down_x - p.x;
                    p.grab_dy = (float)down_y - p.y;
                } else {
                    fd.x = (float)down_x;
                    fd.y = (float)down_y;
                    fd.amount = 1.0f;
                    fd.active = true;
                    if (p.st != ST_WALK && p.st != ST_EAT) {
                        p.st = ST_WALK;
                        p.state_time = 0;
                    }
                    fprintf(stderr, "pet: food at %d,%d\n", down_x, down_y);
                }
            }
            if (released) {
                if (p.dragging) {
                    p.dragging = false;
                    if (want_exit) {
                        /* long-pressed: back to the launcher icon */
                        mode = MODE_LAUNCHER;
                        set_grab(tfd, 0);
                        p.st = ST_IDLE;
                        p.state_time = 0;
                        fd.active = false;
                        want_exit = false;
                        tap_count = 0;
                        fprintf(stderr, "pet: bye! tap the icon to call me again\n");
                    } else if (t - down_t < 0.45f) {
                        /* a tap on the pet: hop, or sleep after 3 quick taps */
                        if (t - last_tap_t < 0.9f) tap_count++;
                        else tap_count = 1;
                        last_tap_t = t;
                        if (tap_count >= 3) {
                            p.st = ST_SLEEP;
                            p.z_t0 = t;
                            p.state_time = 0;
                            tap_count = 0;
                            set_grab(tfd, 0);
                            fprintf(stderr, "pet: zzz... (desktop usable, tap me to wake)\n");
                        } else {
                            p.st = ST_HAPPY;
                            p.happy_t0 = t;
                            p.state_time = 0;
                        }
                    } else {
                        p.st = ST_IDLE;
                        p.state_time = 0;
                    }
                }
                p.idle_time = 0;
                want_exit = false;
            }

            /* ---- butterfly lifecycle ---- */
            if (!bf.alive && t > bf_respawn) {
                bf.alive = true;
                bf.born = t;
                bf.phase = frand() * 6.28f;
                int side = rand() % 2;
                bf.x = side ? -20 : (float)(screen_w + 20);
                bf.y = 60 + frand() * (screen_h - 200);
                bf.vx = side ? 40 + frand() * 40 : -(40 + frand() * 40);
                bf.vy = (frand() - 0.5f) * 30;
                bf_respawn = t + 12 + frand() * 10;
                fprintf(stderr, "pet: a butterfly appears!\n");
            }
            if (bf.alive) {
                bf.x += bf.vx * dt;
                bf.y += bf.vy * dt + sinf((float)t * 5 + bf.phase) * 12.0f * dt;
                bf.vy += (frand() - 0.5f) * 60.0f * dt;
                if (bf.vy > 40) bf.vy = 40;
                if (bf.vy < -40) bf.vy = -40;
                if (bf.y < 30) { bf.y = 30; bf.vy = fabsf(bf.vy); }
                if (bf.y > screen_h - 60) { bf.y = screen_h - 60; bf.vy = -fabsf(bf.vy); }
                if (t - bf.born > 14) {
                    bf.alive = false;
                    fprintf(stderr, "pet: the butterfly left\n");
                }
                if (bf.x < -40 || bf.x > screen_w + 40)
                    bf.alive = false;
            }

            /* ---- pet state machine ---- */
            float pet_cx = p.x + PET_W / 2, pet_cy = p.y + PET_H / 2;

            switch (p.st) {
            case ST_IDLE:
                p.tx = p.x; p.ty = p.y;
                if (bf.alive) {
                    float dx = bf.x - pet_cx, dy = bf.y - pet_cy;
                    if (sqrtf(dx * dx + dy * dy) < 200 && frand() < 0.03f) {
                        p.st = ST_CHASE;
                        p.state_time = 0;
                        fprintf(stderr, "pet: chasing the butterfly!\n");
                        break;
                    }
                }
                if (p.idle_time > 8.0) {
                    float r = frand();
                    if (r < 0.25f) {
                        p.st = ST_SIT; p.sit_t0 = t; p.state_time = 0;
                    } else if (r < 0.50f) {
                        p.st = ST_SLEEP; p.z_t0 = t; p.state_time = 0;
                        set_grab(tfd, 0);
                        fprintf(stderr, "pet: sleep (desktop unlocked)\n");
                    } else {
                        p.tx = frand() * (screen_w - PET_W);
                        p.ty = 30 + frand() * (screen_h - PET_H - 60);
                        p.st = ST_WALK; p.state_time = 0;
                        fprintf(stderr, "pet: walk to %d,%d\n", (int)p.tx, (int)p.ty);
                    }
                }
                break;

            case ST_WALK: {
                float gx = fd.active ? fd.x - PET_W / 2 : p.tx;
                float gy = fd.active ? fd.y - PET_H / 2 : p.ty;
                float dx = gx - p.x, dy = gy - p.y;
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist < 4.0f) {
                    if (fd.active) {
                        p.st = ST_EAT; p.chew_t0 = t; p.state_time = 0;
                        fprintf(stderr, "pet: eating\n");
                    } else {
                        p.st = ST_IDLE; p.state_time = 0; p.idle_time = 0;
                    }
                } else {
                    float sp = 80.0f * dt;
                    if (sp > dist) sp = dist;
                    p.x += dx / dist * sp;
                    p.y += dy / dist * sp;
                    if (fabsf(dx) > 2) p.facing = dx > 0 ? 1 : -1;
                    p.walk_phase += dt * 10.0f;
                    p.look_x = dx > 0 ? 1 : -1;
                    p.look_y = 0;
                }
                break;
            }

            case ST_SIT:
                if (frand() < 0.004f) p.ear_twitch = (frand() - 0.5f) * 2.0f;
                p.look_x = 0; p.look_y = 0;
                if (p.state_time > 4.0 + frand() * 3.0) {
                    p.st = ST_IDLE; p.state_time = 0; p.idle_time = 0;
                }
                break;

            case ST_SLEEP:
                p.look_x = 0; p.look_y = 0;
                break;

            case ST_DRAG:
                p.x = (float)ts.x - p.grab_dx;
                p.y = (float)ts.y - p.grab_dy;
                p.look_x = ts.x > pet_cx ? 1 : -1;
                p.look_y = ts.y > pet_cy ? 1 : -1;
                if (t - down_t > 1.2f)
                    want_exit = true;      /* hold the pet to send it back */
                break;

            case ST_HAPPY:
                p.hop = sinf((float)(t - p.happy_t0) / 0.55f * 3.14159265f) * 16.0f;
                if (p.state_time > 1.1f) {
                    p.hop = 0;
                    p.st = ST_IDLE; p.state_time = 0;
                }
                break;

            case ST_CHASE: {
                if (!bf.alive) {
                    p.st = ST_IDLE; p.state_time = 0;
                    break;
                }
                float dx = bf.x - pet_cx, dy = bf.y - (pet_cy - 20);
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist < 26.0f) {
                    bf.alive = false;
                    bf_respawn = t + 8 + frand() * 8;
                    p.st = ST_HAPPY;
                    p.happy_t0 = t;
                    p.state_time = 0;
                    fprintf(stderr, "pet: caught the butterfly!\n");
                } else {
                    float sp = (110.0f + dist * 0.15f) * dt;
                    if (sp > dist) sp = dist;
                    p.x += dx / dist * sp;
                    p.y += (dy - 20) / dist * sp;
                    if (fabsf(dx) > 2) p.facing = dx > 0 ? 1 : -1;
                    p.walk_phase += dt * 14.0f;
                    p.look_x = dx > 0 ? 1 : -1;
                    p.look_y = dy > 0 ? 1 : -1;
                }
                break;
            }

            case ST_EAT: {
                p.look_x = fd.x > pet_cx ? 1 : -1;
                p.look_y = 1;
                if (frand() < 0.05f) p.ear_twitch = (frand() - 0.5f) * 1.5f;
                fd.amount -= dt * 0.45f;
                if (fd.amount <= 0.05f) {
                    fd.active = false;
                    fd.amount = 0;
                    p.st = ST_HAPPY;
                    p.happy_t0 = t;
                    p.state_time = 0;
                    p.idle_time = 0;
                    fprintf(stderr, "pet: yummy!\n");
                } else if (p.state_time > 25.0f) {
                    fd.active = false;
                    p.st = ST_IDLE; p.state_time = 0;
                }
                break;
            }
            }

            /* clamp on screen */
            if (p.x < 0) p.x = 0;
            if (p.y < 0) p.y = 0;
            if (p.x > screen_w - PET_W) p.x = screen_w - PET_W;
            if (p.y > screen_h - PET_H) p.y = screen_h - PET_H;
        }

        p.squash = 1.0f + sinf((float)t * 2.0f) * 0.025f;

        /* ---- render ---- */
        surf.px = fb_ptr[cur_buf];
        surf.stride = (int)(fb_pitch[cur_buf] / 4);
        memset(surf.px, 0, (size_t)fb_pitch[cur_buf] * screen_h);
        if (mode == MODE_PET) {
            draw_food(&surf, &fd);
            draw_butterfly(&surf, &bf, t);
            draw_pet(&surf, &p, t);
        } else {
            draw_launcher_icon(&surf, t);
        }
        plane_show(cur_buf);
        cur_buf ^= 1;

        struct timespec sl = { 0, FRAME_NS };
        nanosleep(&sl, NULL);
    }

    fprintf(stderr, "pet: stopping\n");
    set_grab(tfd, 0);
    drm_cleanup();
    if (lock_fd >= 0) close(lock_fd);
    return 0;
}

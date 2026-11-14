// The small cores' gravity in single-core mode, ported from the Windows panel (win/panel.c: orbit_k, bead_step,
// fly_step). Each small core is a body: a strong spring holds it on its orbit round the main core (its gravity,
// Settings > Motion > Gravity) and a weaker one draws it along the orbit to its place. While the cluster is carried
// only gravity holds them, so they slide round to the side away from the motion and trail behind; pulled too far
// (carried very fast) gravity lets go, and the small core flies on with its speed, slowing slowly and bouncing off
// the screen's edges, until the main core comes close. Used by mac/panel.m (on the screen) and mac/settings.m (the
// preview). Coordinates are points with y up, as in Cocoa (the Windows code has y down).
#ifndef VIGIL_ORBIT_H
#define VIGIL_ORBIT_H
#include <math.h>

typedef struct { float px, py, vx, vy, w, rest; int free, init; } Orbit;
typedef struct { float K, C, KT, CT, BREAK, CATCH; } OrbitK;
static const float GRAVITIES[4] = {1, 2, 4, 8}; // Floaty .. Firm: the orbit spring's strength, times the lightest
static const float ORBIT_BOUNCE = 0.7f;         // how much speed a bounce off a screen edge keeps

// the springs: onto the orbit (gravity, strong) and along it to its place (weaker), where gravity lets go, where
// the main core catches a loose one; for an orbit rx across with small cores `small` wide
static inline OrbitK orbit_k(float gravity, float rx, float small) {
    OrbitK k = {150 * gravity, 19.6f * sqrtf(gravity), 36, 11, 3.2f * rx, 1.35f * rx + small};
    return k;
}

// One step of a small core: a bead on a ring (rx, ry) round (cx, cy), held onto the ring and drawn along it
// toward its place at angle a (tx, ty) with strength kt (0: it slides freely); the others on the ring push it
// off when they touch. Returns its distance from the centre (past k->BREAK it should let go).
static inline float bead_step(Orbit *ob, float cx, float cy, float tx, float ty, double a, float rx, float ry, float small,
                              float kt, const OrbitK *k, Orbit **ring, int rn, float dt) {
    float d = 0;
    for (int st = 0; st < 4; st++) {
        float sub = dt / 4, dx = ob->px - cx, dy = ob->py - cy;
        d = hypotf(dx, dy);
        if (d < 1) { dx = tx - cx; dy = ty - cy; d = fmaxf(1, hypotf(dx, dy)); } // right on the centre: out towards its place
        float nx = dx / d, ny = dy / d, txn = -ny, tyn = nx; // outwards, and along the ring the way its angle grows
        double th = atan2(dy, dx), da = remainder(a - th, 2 * M_PI);
        float rad = hypotf(rx * (float)cos(th), ry * (float)sin(th));
        float vr = ob->vx * nx + ob->vy * ny, vt = ob->vx * txn + ob->vy * tyn;
        float fr = k->K * (rad - d) - k->C * vr, ft = kt * (float)da * rad - k->CT * vt;
        float bx = 0, by = 0;
        for (int j = 0; j < rn; j++) {
            if (ring[j] == ob) continue;
            float ex = ob->px - ring[j]->px, ey = ob->py - ring[j]->py, e = hypotf(ex, ey), gap = small * 1.05f - e;
            if (gap > 0 && e > .01f) { bx += k->K * gap * ex / e; by += k->K * gap * ey / e; }
        }
        ob->vx += (fr * nx + ft * txn + bx) * sub; ob->vy += (fr * ny + ft * tyn + by) * sub;
        ob->px += ob->vx * sub; ob->py += ob->vy * sub;
    }
    return d;
}

// a loose small core: it flies on, slowing a little (the caller bounces it off its walls)
static inline void fly_step(Orbit *ob, float dt) {
    float fr = expf(-dt * 0.9f);
    ob->vx *= fr; ob->vy *= fr; ob->px += ob->vx * dt; ob->py += ob->vy * dt;
    if (hypotf(ob->vx, ob->vy) < 4) { ob->vx = ob->vy = 0; ob->rest += dt; } else ob->rest = 0;
}
#endif

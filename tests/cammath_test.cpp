// Native checks for src/cammath.h (the camera maths and the flight modes). Run with tests/run.sh.
#include "cammath.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace pm;

static int g_fail;
#define CHECK(cond, ...)                              \
    do {                                              \
        if (!(cond)) {                                \
            printf("FAIL line %d: ", __LINE__);       \
            printf(__VA_ARGS__);                      \
            printf("\n");                             \
            g_fail++;                                 \
        }                                             \
    } while (0)

static bool near(Vec3 a, Vec3 b, float eps = 1e-4f) { return len(a - b) < eps; }
static bool orthonormal(const Basis& b) {
    return fabsf(len(b.r) - 1) < 1e-4f && fabsf(len(b.u) - 1) < 1e-4f && fabsf(len(b.f) - 1) < 1e-4f &&
           fabsf(dot(b.r, b.u)) < 1e-4f && fabsf(dot(b.r, b.f)) < 1e-4f && fabsf(dot(b.u, b.f)) < 1e-4f;
}
static bool same(const Basis& a, const Basis& b, float eps = 1e-3f) {
    return near(a.r, b.r, eps) && near(a.u, b.u, eps) && near(a.f, b.f, eps);
}

// One 6DOF frame as camera_frame does it: turn the axes, back to angles, axes again.
struct Cam { float yaw = 0, pitch = 0, roll = 0; };
static Basis step(Cam& c, float yaw_t, float pitch_t, float roll_t) {
    Basis b = make_basis(c.yaw, c.pitch, c.roll);
    turn(b.f, b.r, yaw_t);
    turn(b.f, b.u, pitch_t);
    turn(b.r, b.u, roll_t);
    basis_angles(b, c.yaw, c.pitch, c.roll);
    return make_basis(c.yaw, c.pitch, c.roll);
}

int main() {
    const float PI = 3.14159265f;

    // The old free-camera axes (right from world up x forward) for roll 0.
    for (float yaw = -3; yaw <= 3; yaw += 0.7f)
        for (float pitch = -1.5f; pitch <= 1.5f; pitch += 0.5f) {
            Basis b = make_basis(yaw, pitch, 0);
            Vec3 f = look_dir(yaw, pitch);
            Vec3 r = norm(cross(Vec3{0, 1, 0}, f));
            CHECK(near(b.r, r) && near(b.f, f) && near(b.u, cross(f, r)), "old axes yaw %f pitch %f", yaw, pitch);
        }

    // Angles -> axes -> angles -> axes gives the same axes, anywhere (poles included).
    srand(1);
    for (int i = 0; i < 2000; i++) {
        float yaw = (rand() / (float)RAND_MAX - 0.5f) * 2 * PI;
        float pitch = (rand() / (float)RAND_MAX - 0.5f) * PI;
        float roll = (rand() / (float)RAND_MAX - 0.5f) * 2 * PI;
        if (i % 10 == 0) pitch = (i % 20 == 0) ? PI / 2 : -PI / 2;
        Basis a = make_basis(yaw, pitch, roll);
        CHECK(orthonormal(a), "orthonormal %f %f %f", yaw, pitch, roll);
        float y2 = yaw, p2, r2;
        basis_angles(a, y2, p2, r2);
        CHECK(same(a, make_basis(y2, p2, r2)), "round trip %f %f %f -> %f %f %f", yaw, pitch, roll, y2, p2, r2);
    }

    // A full loop: pitching up 360 degrees in small frames goes over the top and back.
    {
        Cam c;
        c.yaw = 0.3f;
        Basis start = make_basis(c.yaw, c.pitch, c.roll), b = start;
        bool went_up = false, went_behind = false;
        for (int i = 0; i < 720; i++) {
            b = step(c, 0, PI / 360, 0);
            CHECK(orthonormal(b), "loop frame %d", i);
            if (b.f.y > 0.999f) went_up = true;
            if (i == 269) went_behind = dot(b.f, start.f) < -0.7f && b.f.y > 0.6f && b.u.y < 0;  // 135 degrees: up, behind, upside down
        }
        CHECK(went_up && went_behind, "loop: up %d behind %d", went_up, went_behind);
        CHECK(same(b, start, 1e-2f), "loop ends where it began");
    }

    // RB banks right: the camera's right side goes down.
    {
        Cam c;
        Basis b = step(c, 0, 0, -0.3f);
        CHECK(b.r.y < -0.2f, "RB lowers the right side (r.y %f)", b.r.y);
    }

    // No fixed horizon: banked 90 degrees right, stick right turns toward the ground.
    {
        Cam c;
        for (int i = 0; i < 30; i++) step(c, 0, 0, -PI / 60);
        Basis b = step(c, 0.2f, 0, 0);
        CHECK(b.f.y < -0.15f, "turning right while on its side looks down (f.y %f)", b.f.y);
        // Pitching up from there turns toward world right (old right is now world up).
        Cam d = c;
        Basis b2 = step(d, 0, 0.2f, 0);
        CHECK(b2.f.x < -0.15f || b2.f.x > 0.15f, "pitch while banked turns sideways (f.x %f)", b2.f.x);
    }

    // Many random frames never drift from orthonormal or go NaN.
    {
        Cam c;
        for (int i = 0; i < 100000; i++) {
            Basis b = step(c, (rand() / (float)RAND_MAX - 0.5f) * 0.1f, (rand() / (float)RAND_MAX - 0.5f) * 0.1f,
                           (rand() / (float)RAND_MAX - 0.5f) * 0.1f);
            if (!orthonormal(b) || c.yaw != c.yaw || c.pitch != c.pitch || c.roll != c.roll) {
                CHECK(false, "random frames broke at %d", i);
                break;
            }
        }
    }

    // Seamoth: 1 s of full thrust, then let go. Same path at 30, 60 and 144 fps; it glides
    // on after the stick is released and stops dead within about 2 s.
    {
        const float top = 4.0f;
        Vec3 end[3];
        float glide[3], stop_t[3];
        const float rates[3] = {30, 60, 144};
        for (int k = 0; k < 3; k++) {
            float dt = 1 / rates[k];
            Vec3 pos{0, 0, 0}, vel{0, 0, 0}, at_release{};
            stop_t[k] = -1;
            int frames = (int)(4 * rates[k] + 0.5f), thrust = (int)(rates[k] + 0.5f);
            for (int i = 0; i < frames; i++) {
                Vec3 want = i < thrust ? Vec3{0, 0, top} : Vec3{0, 0, 0};
                if (i == thrust) at_release = pos;
                seamoth_step(pos, vel, want, top, dt);
                if (i >= thrust && stop_t[k] < 0 && len(vel) == 0) stop_t[k] = (i + 1) * dt - 1;
            }
            end[k] = pos;
            glide[k] = pos.z - at_release.z;
        }
        // Exact per frame, except the dead stop lands on a frame boundary.
        CHECK(near(end[0], end[1], 0.01f) && near(end[1], end[2], 0.01f), "same glide at any fps: %f %f %f",
              end[0].z, end[1].z, end[2].z);
        CHECK(glide[1] > 1.5f && glide[1] < 2.2f, "glides on about top x tau after release (%f m)", glide[1]);
        CHECK(stop_t[1] > 1.5f && stop_t[1] < 2.2f, "stops within ~2 s (%f s)", stop_t[1]);
        // Speeding up: 86% of top speed after tau, nearly all of it after 3 tau.
        Vec3 pos{0, 0, 0}, vel{0, 0, 0};
        for (int i = 0; i < 30; i++) seamoth_step(pos, vel, Vec3{top, 0, 0}, top, 1 / 60.0f);
        CHECK(fabsf(vel.x - top * (1 - expf(-1))) < 0.01f, "86%% after tau (%f)", vel.x);
        for (int i = 0; i < 60; i++) seamoth_step(pos, vel, Vec3{top, 0, 0}, top, 1 / 60.0f);
        CHECK(vel.x > 0.95f * top && vel.x <= top, "near top speed after 3 tau (%f)", vel.x);
        // Turning: the old velocity bends round toward the new direction, no snap.
        seamoth_step(pos, vel, Vec3{0, 0, top}, top, 1 / 60.0f);
        CHECK(vel.x > 3.5f && vel.z > 0 && vel.z < 0.2f, "turns smoothly (%f, %f)", vel.x, vel.z);
        // A huge frame (a hitch) doesn't overshoot.
        Vec3 p2{0, 0, 0}, v2{top, 0, 0};
        seamoth_step(p2, v2, Vec3{0, 0, 0}, top, 5.0f);
        CHECK(len(v2) == 0 && p2.x > 0 && p2.x <= top * SEAMOTH_TAU + 1e-4f, "a 5 s hitch glides at most top x tau (%f)", p2.x);
    }

    // The eased look settles on the stick's rate the same way at any frame rate.
    {
        float a = 0, b = 0;
        for (int i = 0; i < 6; i++) a = ease_to(a, 1.8f, SEAMOTH_LOOK_TAU, 1 / 60.0f);
        for (int i = 0; i < 24; i++) b = ease_to(b, 1.8f, SEAMOTH_LOOK_TAU, 1 / 240.0f);
        CHECK(fabsf(a - b) < 1e-4f && a > 1.2f && a < 1.8f, "look ease %f %f", a, b);
    }

    // Flight mode: right steps Default -> 6DOF -> Seamoth -> Default, left goes back.
    {
        int f = FLIGHT_DEFAULT;
        const int want_right[] = {FLIGHT_6DOF, FLIGHT_SEAMOTH, FLIGHT_DEFAULT};
        for (int w : want_right) {
            f = flight_cycle(f, 1);
            CHECK(f == w, "right gives %d, not %d", f, w);
        }
        CHECK(flight_cycle(FLIGHT_DEFAULT, -1) == FLIGHT_SEAMOTH, "left from Default wraps to Seamoth");
        CHECK(flight_cycle(FLIGHT_SEAMOTH, -1) == FLIGHT_6DOF, "left from Seamoth is 6DOF");
        CHECK(!strcmp(flight_name(FLIGHT_DEFAULT), "Default") && !strcmp(flight_name(FLIGHT_6DOF), "6DOF") &&
                  !strcmp(flight_name(FLIGHT_SEAMOTH), "Seamoth"),
              "flight names");
    }

    printf(g_fail ? "%d failed\n" : "cammath: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}

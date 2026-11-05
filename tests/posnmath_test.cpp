// Native checks for src/posnmath.h (the Position panel's maths). Run with tests/run.sh.
#include "posnmath.h"
#include <stdio.h>

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

static const float D = 3.14159265f / 180;
static bool near(float a, float b, float tol = 1e-4f) { return fabsf(a - b) <= tol; }
static bool mat_near(const Mat4& a, const Mat4& b, float tol = 1e-4f) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (!near(a.m[i][j], b.m[i][j], tol)) return false;
    return true;
}
// A point (row vector) through a matrix.
static void xf(const Mat4& m, const float p[3], float out[3]) {
    for (int j = 0; j < 3; j++) out[j] = p[0] * m.m[0][j] + p[1] * m.m[1][j] + p[2] * m.m[2][j] + m.m[3][j];
}

// Measured numbers: her matrix in Majula (PlayerCtrl+0x60), facing yaw -111 degrees.
static const Mat4 MAJULA_M = {{{-0.358453f, 0, 0.933548f, 0}, {0, 1, 0, 0}, {-0.933548f, 0, -0.358453f, 0},
                                {-18.6846f, 6.18799f, -20.3257f, 1}}};

int main() {
    // No offset: drawn exactly where she is.
    CHECK(mat_near(posn_apply(MAJULA_M, PosnOffset{}), MAJULA_M, 1e-5f), "zero offset changes nothing");

    // Moves are in her own frame: X her right, Y up, Z forward.
    {
        PosnOffset o;
        o.x = 1;
        Mat4 m = posn_apply(MAJULA_M, o);
        CHECK(near(m.m[3][0], -18.6846f - 0.358453f) && near(m.m[3][2], -20.3257f + 0.933548f), "X goes along her right");
        o = {};
        o.y = 2;
        m = posn_apply(MAJULA_M, o);
        CHECK(near(m.m[3][1], 6.18799f + 2) && near(m.m[3][0], -18.6846f) && near(m.m[3][2], -20.3257f), "Y goes straight up");
        o = {};
        o.z = 1;
        m = posn_apply(MAJULA_M, o);
        CHECK(near(m.m[3][0], -18.6846f - 0.933548f) && near(m.m[3][2], -20.3257f - 0.358453f), "Z goes forward");
    }

    // Turns are about the pivot (hips), so the pivot stays where it was.
    {
        float hip[3] = {0, POSN_PIVOT, 0}, a[3], b[3];
        PosnOffset o;
        o.pitch = 25 * D;
        o.yaw = -70 * D;
        o.roll = 40 * D;
        xf(MAJULA_M, hip, a);
        xf(posn_apply(MAJULA_M, o), hip, b);
        for (int i = 0; i < 3; i++) CHECK(near(a[i], b[i]), "pivot stays put (%d): %f vs %f", i, a[i], b[i]);
        // Still a rotation: rows unit length and square to each other.
        Mat4 m = posn_apply(MAJULA_M, o);
        for (int i = 0; i < 3; i++) {
            float l = m.m[i][0] * m.m[i][0] + m.m[i][1] * m.m[i][1] + m.m[i][2] * m.m[i][2];
            CHECK(near(l, 1), "row %d unit", i);
        }
        float d = m.m[0][0] * m.m[1][0] + m.m[0][1] * m.m[1][1] + m.m[0][2] * m.m[1][2];
        CHECK(near(d, 0), "rows square");
    }

    // Signs: positive roll raises her right side; positive pitch tips her face up; positive yaw
    // turns her forward toward her right.
    {
        PosnOffset o;
        o.roll = 30 * D;
        Mat4 m = posn_apply(mat_identity(), o);
        CHECK(m.m[0][1] > 0.4f, "roll raises the right (%f)", m.m[0][1]);
        o = {};
        o.pitch = 30 * D;
        m = posn_apply(mat_identity(), o);
        CHECK(m.m[2][1] > 0.4f, "pitch tips the face up (%f)", m.m[2][1]);
        o = {};
        o.yaw = 30 * D;
        m = posn_apply(mat_identity(), o);
        CHECK(m.m[2][0] > 0.4f, "yaw turns forward to the right (%f)", m.m[2][0]);
    }

    // Lying her down: pitch -90 leaves her face down, her up pointing forward.
    {
        PosnOffset o;
        o.pitch = -90 * D;
        Mat4 m = posn_apply(mat_identity(), o);
        CHECK(near(m.m[2][1], -1) && near(m.m[1][2], 1), "pitch -90: face down");
    }

    // Weapons go with her: something at her hand stays at her hand.
    {
        Mat4 hand = mat_mul(mat_mul(mat_rot_x(0.3f), mat_translate(0.3f, 1.1f, 0.2f)), MAJULA_M);  // hand, in the world
        PosnOffset o;
        o.x = 0.5f;
        o.y = 1.5f;
        o.roll = 35 * D;
        o.yaw = 10 * D;
        Mat4 moved = posn_carry(hand, MAJULA_M, o);
        Mat4 local = mat_mul(hand, mat_rigid_inverse(MAJULA_M));  // hand in her frame
        Mat4 local2 = mat_mul(moved, mat_rigid_inverse(posn_apply(MAJULA_M, o)));
        CHECK(mat_near(local, local2), "the hand keeps its place on her");
        CHECK(mat_near(mat_mul(MAJULA_M, mat_rigid_inverse(MAJULA_M)), mat_identity()), "rigid inverse");
        CHECK(mat_near(posn_carry(hand, MAJULA_M, PosnOffset{}), hand), "no offset: the hand stays");
    }

    if (g_fail) {
        printf("posnmath: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("posnmath: ok\n");
    return 0;
}

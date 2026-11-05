// The Position panel's maths, without Windows headers, so tests/posnmath_test.cpp can check it
// natively: the character's model matrix moved and turned by the panel's offsets.
//
// The game's matrices are row-major with row vectors (a point p goes to p * M): rows are
// right, up, forward and position, Y up (the camera's layout, README "The camera hook").
// The offsets are in the character's own frame: X along her right, Y up, Z forward; pitch
// about her right axis, yaw about her up axis, roll about her forward axis, all turning about
// a pivot POSN_PIVOT metres above her feet (about the hips), so a roll tips her over sideways
// rather than about her toes.
#pragma once
#include <math.h>

namespace pm {

struct Mat4 { float m[4][4]; };

struct PosnOffset {
    float x = 0, y = 0, z = 0;           // metres
    float pitch = 0, yaw = 0, roll = 0;  // radians
    bool zero() const { return x == 0 && y == 0 && z == 0 && pitch == 0 && yaw == 0 && roll == 0; }
};

const float POSN_PIVOT = 0.9f;

inline Mat4 mat_mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
    return r;
}
inline Mat4 mat_identity() {
    Mat4 r = {};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1;
    return r;
}
inline Mat4 mat_translate(float x, float y, float z) {
    Mat4 r = mat_identity();
    r.m[3][0] = x;
    r.m[3][1] = y;
    r.m[3][2] = z;
    return r;
}
// Rotations for row vectors, right-handed about each axis.
inline Mat4 mat_rot_x(float a) {
    Mat4 r = mat_identity();
    float c = cosf(a), s = sinf(a);
    r.m[1][1] = c; r.m[1][2] = s;
    r.m[2][1] = -s; r.m[2][2] = c;
    return r;
}
inline Mat4 mat_rot_y(float a) {
    Mat4 r = mat_identity();
    float c = cosf(a), s = sinf(a);
    r.m[0][0] = c; r.m[0][2] = -s;
    r.m[2][0] = s; r.m[2][2] = c;
    return r;
}
inline Mat4 mat_rot_z(float a) {  // positive raises the right side
    Mat4 r = mat_identity();
    float c = cosf(a), s = sinf(a);
    r.m[0][0] = c; r.m[0][1] = s;
    r.m[1][0] = -s; r.m[1][1] = c;
    return r;
}

// The local change: about the pivot, roll first, then pitch, then yaw (so yaw stays about
// her own up axis however she's tipped), then the move.
inline Mat4 posn_local(const PosnOffset& o) {
    Mat4 r = mat_translate(0, -POSN_PIVOT, 0);
    r = mat_mul(r, mat_rot_z(o.roll));
    r = mat_mul(r, mat_rot_x(-o.pitch));  // positive pitch leans her back (face up), as the camera's pitch looks up
    r = mat_mul(r, mat_rot_y(o.yaw));
    r = mat_mul(r, mat_translate(o.x, o.y + POSN_PIVOT, o.z));
    return r;
}

// `world`: her model matrix as the game has it; the result is where she's drawn.
inline Mat4 posn_apply(const Mat4& world, const PosnOffset& o) { return mat_mul(posn_local(o), world); }

// A rigid matrix's inverse (rotation rows orthonormal): the transposed rotation, and the
// position turned back.
inline Mat4 mat_rigid_inverse(const Mat4& m) {
    Mat4 r = mat_identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = m.m[j][i];
    for (int j = 0; j < 3; j++) r.m[3][j] = -(m.m[3][0] * r.m[0][j] + m.m[3][1] * r.m[1][j] + m.m[3][2] * r.m[2][j]);
    return r;
}
// Something placed in the world by her (`thing`, e.g. a weapon in her hand), moved with her:
// `world` is her matrix, the result is where `thing` goes once she is drawn at
// posn_apply(world, o).
inline Mat4 posn_carry(const Mat4& thing, const Mat4& world, const PosnOffset& o) {
    return mat_mul(mat_mul(thing, mat_rigid_inverse(world)), posn_apply(world, o));
}

}  // namespace pm

// The Pose panel's logic, without Windows headers, so tests/posemath_test.cpp can check it
// natively: which pose plays, where its clock is, and how far the player's animation
// network should advance each update to scrub, freeze, rewind or loop it.
//
// Going back in time plays a gesture backwards: the network is updated by a negative step
// (up to POSE_SEEK_STEP each update), mirroring how going forward plays it, within the
// limits in pose_step (no node goes back past its own start). Otherwise (`reverse` off:
// the call wasn't found; an event action; from near the gesture's end) going back replays
// it: the game only starts a gesture or event action from its own idle (a second request
// while one plays is ignored), so it first fast-forwards the current one to its end, then
// asks for the pose again and plays it up to the wanted time. Another pose always goes
// that way.
// A request the game takes while the network is frozen is lost (and leaves the character
// unable to gesture until it moves), so the network keeps running until the pose is in.
#pragma once
#include <math.h>
#include <stdint.h>

namespace pm {

// ---- turning the character: its orientation is a quaternion (x, y, z, w), Y up
struct Quat { float x, y, z, w; };
inline Quat quat_mul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat quat_yaw(float rad) { return {0, sinf(rad / 2), 0, cosf(rad / 2)}; }
inline Quat quat_norm(Quat q) {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-6f) return {0, 0, 0, 1};
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}
// `base` turned by `rad` about the world's up axis.
inline Quat turned(Quat base, float rad) { return quat_norm(quat_mul(quat_yaw(rad), base)); }

// ---- the poses
enum PoseKind : uint8_t {
    POSE_NONE,     // the character as the game has it
    POSE_GESTURE,  // a gesture: plays once, then the character is back in its idle
    POSE_EVENT,    // an event action (kneeling, praying...): holds until it is ended
};
struct PoseDef {
    PoseKind kind;
    int cat;           // the game's gesture or event action category
    float dur;         // seconds: the Frame row's range
    const char* name;
    float hold = -1;   // picked, it plays to this fraction of dur and holds there (-1: plays on)
    int clip = 0;      // a gesture played with another clip in its place (pose.cpp's CLIPS; 0: its own)
};

const float POSE_FF_STEP = 0.25f;     // seconds per update when fast-forwarding to an end
const float POSE_SEEK_STEP = 0.1f;    // seconds per update when playing up to a frame
const float POSE_SETTLE_G = 0.8f;     // after a gesture's clip: until the game takes requests again
const float POSE_SETTLE_E = 3.0f;     // after ending an event action: its end clip, then idle
                                      // (at the game's speed: its action state waits on game
                                      // time, and a request made before it's idle is lost)
const float POSE_GIVE_UP = 1.5f;      // a request the game hasn't taken by then is dropped
const int POSE_WARM = 3;              // updates the network runs after the game takes a request
const float POSE_REV_MARGIN = 0.05f;  // played backwards: the youngest clip stays this far in
const float POSE_REV_NEAR = 0.2f;     // ...and a frame up to this much before that is held there
const float POSE_EXIT_GAP = 0.3f;     // a clip this much younger than the pose: the blend out began
const float POSE_EXIT_MARGIN = 0.25f; // backwards only from this long before the blend out...
const float POSE_REV_TAIL = 1.0f;     // ...or, not seen yet, this long before the clip's end
const int POSE_MAX = 64;

enum PosePhase : uint8_t {
    PH_IDLE,       // no pose: the game's own animation, untouched
    PH_REQUESTED,  // asked the game for `cur`; waiting for it to take the request
    PH_PLAYING,    // `cur` plays (or is held: frozen)
    PH_ENDING,     // fast-forwarding `cur` to its end before asking for `want`
};
enum PoseAct : uint8_t { ACT_NONE, ACT_REQUEST, ACT_END_EVENT, ACT_CANCEL };

struct PoseSM {
    // Asked for by the panel:
    int want = 0;          // pose index (0: none)
    float target = -1;     // play up to this time into `want`, then freeze (-1: none)
    bool frozen = false;
    // Where it is:
    int cur = 0;
    PosePhase phase = PH_IDLE;
    float clock = 0;       // seconds into `cur` (or since the request, while REQUESTED)
    float end_at = 0;      // ENDING: until clock reaches this
    int warm = 0;
    float start_clock = 0; // the clock once the warm-up updates are done: no frame before it
    bool reverse = false;  // the network can be set back in time (else going back replays)
    float young = 1e9f;    // seconds into its clip the network's youngest clip is: no node may
                           // go back past its own start (the game crashed), so no further back
    float exit[POSE_MAX];  // per pose: when the game began blending it into the idle (seen
                           // once it played there; <= 0: not seen yet)
    PoseSM() {
        for (float& e : exit) e = 0;
    }
};

struct PoseOut {
    float dt;        // what the player's network advances by this update (0: held)
    PoseAct act;     // what to do to the game's action state first
    int pose;        // ACT_REQUEST / ACT_END_EVENT / ACT_CANCEL: which pose
    float back = 0;  // > 0: instead, the network goes back this many seconds
};

inline float pose_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Ends `cur` (an event action gets told to end) and fast-forwards to where the game is idle.
inline PoseOut pose_begin_end(PoseSM& s, const PoseDef* defs, float dt) {
    const PoseDef& d = defs[s.cur];
    s.phase = PH_ENDING;
    if (d.kind == POSE_EVENT) {
        s.end_at = s.clock + POSE_SETTLE_E;
        return {dt, ACT_END_EVENT, s.cur};
    }
    s.end_at = (s.clock > d.dur ? s.clock : d.dur) + POSE_SETTLE_G;
    return {dt, ACT_NONE, 0};
}

// One update of the player's animation network. dt: the game's own step; pending: the
// request made with ACT_REQUEST is still waiting in the game's action state.
inline PoseOut pose_step(PoseSM& s, const PoseDef* defs, float dt, bool pending) {
    if (dt <= 0) dt = 1 / 60.0f;
    switch (s.phase) {
    case PH_IDLE:
        if (!s.want) return {dt, ACT_NONE, 0};
        s.cur = s.want;
        s.phase = PH_REQUESTED;
        s.clock = 0;
        return {dt, ACT_REQUEST, s.cur};

    case PH_REQUESTED:
        if (s.want != s.cur && !s.want) {  // released before it started
            s.phase = PH_IDLE;
            int c = s.cur;
            s.cur = 0;
            return {dt, pending ? ACT_CANCEL : ACT_NONE, c};
        }
        if (pending) {
            s.clock += dt;
            if (s.clock < POSE_GIVE_UP) return {dt, ACT_NONE, 0};
            int c = s.cur;  // the game won't play it here (a menu open, mid-roll...)
            s.phase = PH_IDLE;
            s.cur = s.want = 0;
            s.target = -1;
            return {dt, ACT_CANCEL, c};
        }
        s.phase = PH_PLAYING;
        s.clock = 0;
        s.warm = POSE_WARM;
        [[fallthrough]];  // this update already plays it
    case PH_PLAYING: {
        const PoseDef& d = defs[s.cur];
        if (!s.want) {  // handed back: an event action ends, a gesture plays out at normal speed
            s.phase = PH_IDLE;
            int c = s.cur;
            s.cur = 0;
            s.frozen = false;
            s.target = -1;
            return {dt, d.kind == POSE_EVENT ? ACT_END_EVENT : ACT_NONE, c};
        }
        if (s.want != s.cur) return pose_begin_end(s, defs, dt);
        if (!s.warm && s.cur < POSE_MAX && s.clock > s.start_clock + POSE_EXIT_GAP && s.young < s.clock - POSE_EXIT_GAP) {
            float e = s.clock - s.young;  // a new node in the network: the game is blending it out
            if (s.exit[s.cur] <= 0 || e < s.exit[s.cur]) s.exit[s.cur] = e;
        }
        if (s.target >= 0 && s.target < s.clock - 0.001f && !s.warm) {
            // Backwards only for a gesture (an event action stepped back crashed the game,
            // even inside its clip), and only from before the game began blending it into the
            // idle (stepped back from about there, the game crashed: the blend just begun, or
            // about to), and no node of the network may go back past its own start: no further
            // back than its youngest clip, less a margin. A frame further back than that (by
            // more than POSE_REV_NEAR) replays.
            float floor_t = s.clock - s.young + POSE_REV_MARGIN;
            float seen = s.cur < POSE_MAX ? s.exit[s.cur] : 0;
            float from = seen > 0 ? seen - POSE_EXIT_MARGIN : d.dur - POSE_REV_TAIL;
            if (s.reverse && d.kind == POSE_GESTURE && s.clock <= from && s.target >= floor_t - POSE_REV_NEAR) {
                float goal = s.target > s.start_clock ? s.target : s.start_clock;  // no further back than the warm-up
                if (goal < floor_t) goal = floor_t;
                float back = s.clock - goal;
                if (back > POSE_SEEK_STEP) back = POSE_SEEK_STEP;
                if (back > 0.001f) {
                    s.clock -= back;
                    return {0, ACT_NONE, 0, back};
                }
                s.target = s.clock;
            } else if (s.clock > s.start_clock + 0.001f) {
                return pose_begin_end(s, defs, dt);  // replay
            } else {
                s.target = s.clock;  // a frame inside the warm-up: as near as it gets
            }
        }
        float step;
        if (s.warm > 0) {
            s.warm--;
            step = dt < POSE_SEEK_STEP ? dt : POSE_SEEK_STEP;
            if (!s.warm) s.start_clock = s.clock + step;
        } else if (s.target >= 0) {
            step = s.target - s.clock;
            if (step > POSE_SEEK_STEP) step = POSE_SEEK_STEP;
            if (step <= 0.001f) {
                s.target = -1;
                s.frozen = true;
                step = 0;
            }
        } else if (s.frozen) {
            step = 0;
        } else {
            step = dt;
            if (d.kind == POSE_GESTURE && s.clock >= d.dur + POSE_SETTLE_G) {  // played out: again
                s.phase = PH_REQUESTED;
                s.clock = 0;
                return {dt, ACT_REQUEST, s.cur};
            }
        }
        s.clock += step;
        return {step, ACT_NONE, 0};
    }

    case PH_ENDING: {
        float left = s.end_at - s.clock;
        if (left > 0.001f) {
            float fast = defs[s.cur].kind == POSE_EVENT ? dt : POSE_FF_STEP;
            float step = left < fast ? left : fast;
            s.clock += step;
            return {step, ACT_NONE, 0};
        }
        if (!s.want) {
            s.phase = PH_IDLE;
            s.cur = 0;
            return {dt, ACT_NONE, 0};
        }
        s.cur = s.want;
        s.phase = PH_REQUESTED;
        s.clock = 0;
        return {dt, ACT_REQUEST, s.cur};
    }
    }
    return {dt, ACT_NONE, 0};
}

// The panel picks pose i: it plays on, or (a pose with a hold point) plays to there and holds.
inline void pose_pick(PoseSM& s, const PoseDef* defs, int i) {
    s.want = i;
    s.frozen = false;
    s.target = i && defs[i].hold >= 0 ? defs[i].hold * defs[i].dur : -1;
}

// The Frame row: how far into the pose (0..1) the panel shows.
inline float pose_shown_fraction(const PoseSM& s, const PoseDef* defs) {
    int i = s.want;
    if (!i || defs[i].dur <= 0) return 0;
    if (s.target >= 0) return pose_clampf(s.target / defs[i].dur, 0, 1);
    if (s.cur != i || s.phase != PH_PLAYING) return 0;
    return pose_clampf(s.clock / defs[i].dur, 0, 1);
}

}  // namespace pm

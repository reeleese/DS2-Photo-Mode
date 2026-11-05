// Native checks for src/posemath.h (the Pose panel's logic). Run with tests/run.sh.
#include "posemath.h"
#include <stdio.h>

using namespace pm;

struct Vec3f { float x, y, z; };

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

static const PoseDef DEFS[] = {
    {POSE_NONE, 0, 0, "None"},
    {POSE_GESTURE, 30, 4.0f, "Bow"},
    {POSE_EVENT, 110, 5.0f, "Kneel"},
    {POSE_GESTURE, 300, 4.0f, "Sun"},
};
static const float DT = 1 / 60.0f;

// The game's side of a request: taken on the update after it's made.
struct Game {
    bool pending = false;
    int requests = 0, ends = 0, cancels = 0, last = 0;
};
static PoseOut step(PoseSM& s, Game& g) {
    PoseOut o = pose_step(s, DEFS, DT, g.pending);
    g.pending = false;  // taken (it was made on an earlier update)
    if (o.act == ACT_REQUEST) {
        g.pending = true;
        g.requests++;
        g.last = o.pose;
    }
    if (o.act == ACT_END_EVENT) g.ends++;
    if (o.act == ACT_CANCEL) g.cancels++;
    return o;
}
// Runs until the pose is playing (request made and taken); returns the updates it took.
static int start(PoseSM& s, Game& g) {
    int n = 0;
    while (!(s.phase == PH_PLAYING && s.cur == s.want) && n < 1000) {
        step(s, g);
        n++;
    }
    return n;
}
// One update with the network's youngest clip as the game would sample it: the pose's own
// clip (as far in as the clock) until `exit`, where the game starts blending it into the idle
// (a new clip, from then on).
static PoseOut young_step(PoseSM& s, Game& g, float exit) {
    if (s.phase == PH_PLAYING) s.young = s.clock < exit ? s.clock : s.clock - exit;
    return step(s, g);
}
static Vec3f rot(Quat q, Vec3f v) {  // q v q*
    Quat p = quat_mul(quat_mul(q, Quat{v.x, v.y, v.z, 0}), Quat{-q.x, -q.y, -q.z, q.w});
    return {p.x, p.y, p.z};
}

int main() {
    // Turning: +90 degrees takes forward (+Z) to +X, and back again; length stays 1.
    {
        Quat q = turned(Quat{0, 0, 0, 1}, 3.14159265f / 2);
        Vec3f f = rot(q, {0, 0, 1});
        CHECK(fabsf(f.x - 1) < 1e-4f && fabsf(f.y) < 1e-4f && fabsf(f.z) < 1e-4f, "+90 turns +Z to +X (%f %f %f)", f.x, f.y, f.z);
        Quat b = quat_norm(Quat{0.01f, 0.6f, -0.02f, 0.8f});
        Quat back = turned(turned(b, 1.2f), -1.2f);
        CHECK(fabsf(back.x - b.x) + fabsf(back.y - b.y) + fabsf(back.z - b.z) + fabsf(back.w - b.w) < 1e-4f, "turn and turn back");
        Quat t = turned(b, 2.5f);
        CHECK(fabsf(t.x * t.x + t.y * t.y + t.z * t.z + t.w * t.w - 1) < 1e-4f, "unit length");
        Vec3f up = rot(turned(b, 2.5f), {0, 1, 0}), up0 = rot(b, {0, 1, 0});
        CHECK(fabsf(up.y - up0.y) < 1e-4f, "a turn keeps her lean");
    }

    // Nothing selected: the game's own step, untouched.
    {
        PoseSM s;
        Game g;
        for (int i = 0; i < 10; i++) {
            PoseOut o = step(s, g);
            CHECK(o.dt == DT && o.act == ACT_NONE, "idle passes the game's dt");
        }
    }

    // Picking a gesture asks for it, and the network keeps running until it's in.
    {
        PoseSM s;
        Game g;
        s.want = 1;
        PoseOut o = step(s, g);
        CHECK(o.act == ACT_REQUEST && o.pose == 1 && o.dt > 0, "first update asks");
        s.frozen = true;  // even when Freeze is pressed straight away
        for (int i = 0; i < POSE_WARM + 1; i++) {
            o = step(s, g);
            CHECK(o.dt > 0, "update %d after the request runs (dt %f)", i, o.dt);
        }
        o = step(s, g);
        CHECK(o.dt == 0, "then it holds");
        CHECK(g.requests == 1, "one request (%d)", g.requests);
    }

    // Scrubbing forward plays up to the frame in small steps, then holds.
    {
        PoseSM s;
        Game g;
        s.want = 1;
        start(s, g);
        s.target = 2.0f;
        float maxdt = 0;
        int n = 0;
        while (!s.frozen && n++ < 1000) {
            PoseOut o = step(s, g);
            if (o.dt > maxdt) maxdt = o.dt;
        }
        CHECK(s.frozen && fabsf(s.clock - 2.0f) < 0.01f, "held at 2.0 (clock %f)", s.clock);
        CHECK(maxdt <= POSE_SEEK_STEP + 1e-5f, "steps of at most %f (%f)", POSE_SEEK_STEP, maxdt);
        CHECK(step(s, g).dt == 0 && step(s, g).dt == 0, "stays held");
        CHECK(fabsf(pose_shown_fraction(s, DEFS) - 0.5f) < 0.01f, "Frame shows 50%%");
        CHECK(g.requests == 1, "no second request going forward");
    }

    // Scrubbing back: fast-forward to the end, ask again, play up to the frame.
    {
        PoseSM s;
        Game g;
        s.want = 1;
        start(s, g);
        s.target = 3.0f;
        while (!s.frozen) step(s, g);
        s.target = 1.0f;
        CHECK(fabsf(pose_shown_fraction(s, DEFS) - 0.25f) < 0.01f, "Frame shows the new frame straight away");
        float ff = 0;
        int n = 0;
        while (!(s.frozen && s.target < 0) && n++ < 1000) {
            PoseOut o = step(s, g);
            if (s.phase == PH_ENDING && o.dt > ff) ff = o.dt;
        }
        CHECK(g.requests == 2, "asked again (%d)", g.requests);
        CHECK(ff > 0 && ff <= POSE_FF_STEP + 1e-5f, "fast-forward steps of at most %f (%f)", POSE_FF_STEP, ff);
        CHECK(fabsf(s.clock - 1.0f) < 0.01f && s.frozen, "held at 1.0 (clock %f)", s.clock);
        CHECK(n < 120, "within two seconds at 60 fps (%d updates)", n);
    }

    // Scrubbing back to the very start: replayed once, held just after the warm-up.
    {
        PoseSM s;
        Game g;
        s.want = 1;
        start(s, g);
        s.target = 2.0f;
        while (!s.frozen) step(s, g);
        s.target = 0;
        int n = 0;
        while (!(s.frozen && s.target < 0) && n++ < 1000) step(s, g);
        CHECK(g.requests == 2, "asked again once (%d)", g.requests);
        CHECK(s.frozen && s.clock < 0.1f, "held at the start (clock %f)", s.clock);
        for (int i = 0; i < 100; i++) step(s, g);
        CHECK(g.requests == 2 && s.phase == PH_PLAYING, "and stays there (%d requests)", g.requests);
    }

    // Playing on: a gesture plays out, settles, and starts again.
    {
        PoseSM s;
        Game g;
        s.want = 3;
        start(s, g);
        int n = 0;
        while (g.requests < 2 && n++ < 2000) {
            PoseOut o = step(s, g);
            CHECK(o.dt == DT, "plays at the game's speed");
        }
        CHECK(g.requests == 2 && n >= (int)((4.0f + POSE_SETTLE_G) / DT) - 2, "loops after its clip and the settle (%d)", n);
    }

    // An event action holds; switching away ends it first, then asks for the next.
    {
        PoseSM s;
        Game g;
        s.want = 2;
        start(s, g);
        for (int i = 0; i < 600; i++) step(s, g);  // 10 s: no loop, it holds
        CHECK(g.requests == 1 && g.ends == 0, "held without asking again");
        s.want = 1;
        PoseOut o = step(s, g);
        CHECK(o.act == ACT_END_EVENT && o.pose == 2, "ends the event action");
        int n = 0;
        while (s.phase == PH_ENDING && n++ < 1000) {
            o = step(s, g);
            CHECK(o.dt <= DT + 1e-6f, "an event action ends at the game's speed (%f)", o.dt);
        }
        CHECK(n * DT >= POSE_SETTLE_E - 0.05f, "and waits %f s for it (%f)", POSE_SETTLE_E, n * DT);
        start(s, g);
        CHECK(g.requests == 2 && g.last == 1 && s.cur == 1, "then the gesture");
    }

    // Photo mode off (want 0): an event action ends, the game's speed comes back, nothing else.
    {
        PoseSM s;
        Game g;
        s.want = 2;
        start(s, g);
        s.target = 2.0f;
        while (!s.frozen) step(s, g);
        s.want = 0;
        s.target = -1;
        s.frozen = false;
        PoseOut o = step(s, g);
        CHECK(o.act == ACT_END_EVENT && o.dt == DT && s.phase == PH_IDLE, "ended at once, normal speed");
        o = step(s, g);
        CHECK(o.act == ACT_NONE && o.dt == DT, "then untouched");
    }

    // A gesture handed back mid-pose plays out at the game's speed (no fast-forward).
    {
        PoseSM s;
        Game g;
        s.want = 1;
        start(s, g);
        s.frozen = true;
        step(s, g);
        s.want = 0;
        s.frozen = false;
        PoseOut o = step(s, g);
        CHECK(o.act == ACT_NONE && o.dt == DT && s.phase == PH_IDLE, "idle at once");
    }

    // A request the game never takes is dropped, and the panel goes back to None.
    {
        PoseSM s;
        s.want = 1;
        PoseOut o = pose_step(s, DEFS, DT, false);
        int n = 0;
        while (o.act != ACT_CANCEL && n++ < 1000) o = pose_step(s, DEFS, DT, true);
        CHECK(o.act == ACT_CANCEL && o.pose == 1, "cancelled");
        CHECK(n * DT >= POSE_GIVE_UP - 0.05f && n * DT <= POSE_GIVE_UP + 0.05f, "after %f s (%f)", POSE_GIVE_UP, n * DT);
        CHECK(s.want == 0 && s.phase == PH_IDLE, "back to None");
    }

    // Playing backwards: going back steps the network back (no new request), in steps of
    // at most POSE_SEEK_STEP, and holds at the frame.
    {
        PoseSM s;
        Game g;
        s.reverse = true;
        s.want = 1;
        start(s, g);
        s.target = 2.5f;
        while (!s.frozen) young_step(s, g, 99);
        s.target = 1.0f;
        float maxback = 0, maxdt = 0;
        int n = 0;
        while (!(s.frozen && s.target < 0) && n++ < 1000) {
            PoseOut o = young_step(s, g, 99);
            if (o.back > maxback) maxback = o.back;
            if (o.dt > maxdt) maxdt = o.dt;
        }
        CHECK(g.requests == 1, "no new request (%d)", g.requests);
        CHECK(maxback > 0 && maxback <= POSE_SEEK_STEP + 1e-5f, "back in steps of at most %f (%f)", POSE_SEEK_STEP, maxback);
        CHECK(maxdt == 0, "never forward meanwhile (%f)", maxdt);
        CHECK(fabsf(s.clock - 1.0f) < 0.01f && s.frozen, "held at 1.0 (clock %f)", s.clock);
        CHECK(n <= 17, "1.5 s back in 15 updates or so (%d)", n);
        PoseOut o = young_step(s, g, 99);
        CHECK(o.dt == 0 && o.back == 0, "and stays held");
        // ...and forward again, as before.
        s.target = 2.0f;
        while (!(s.frozen && s.target < 0)) young_step(s, g, 99);
        CHECK(fabsf(s.clock - 2.0f) < 0.01f && g.requests == 1, "forward again (clock %f)", s.clock);
    }

    // Backwards to the very start: no further back than the warm-up.
    {
        PoseSM s;
        Game g;
        s.reverse = true;
        s.want = 1;
        start(s, g);
        s.target = 2.0f;
        while (!s.frozen) young_step(s, g, 99);
        s.target = 0;
        int n = 0;
        while (!(s.frozen && s.target < 0) && n++ < 1000) young_step(s, g, 99);
        CHECK(g.requests == 1, "no new request (%d)", g.requests);
        CHECK(fabsf(s.clock - s.start_clock) < 0.01f, "held at the warm-up's end (clock %f, start %f)", s.clock, s.start_clock);
    }

    // Near its end (past POSE_REV_TAIL before it, the blend out not seen yet), going back replays.
    {
        PoseSM s;
        Game g;
        s.reverse = true;
        s.want = 1;
        start(s, g);
        s.target = 3.5f;
        while (!s.frozen) young_step(s, g, 99);
        s.target = 2.0f;
        int n = 0;
        while (!(s.frozen && s.target < 0) && n++ < 1000) young_step(s, g, 99);
        CHECK(g.requests == 2, "replayed (%d requests)", g.requests);
        CHECK(fabsf(s.clock - 2.0f) < 0.01f, "held at 2.0 (clock %f)", s.clock);
    }

    // The blend into the idle, once seen, is where backwards stops: from before it (by
    // POSE_EXIT_MARGIN), backwards; from after, a replay.
    {
        PoseSM s;
        Game g;
        s.reverse = true;
        s.want = 3;
        start(s, g);
        const float E = 2.5f;  // the game blends "Sun" out here
        s.target = 3.0f;
        while (!s.frozen) young_step(s, g, E);
        CHECK(fabsf(s.exit[3] - E) < 0.05f, "blend out seen at %f (%f)", E, s.exit[3]);
        s.target = 1.0f;  // from 3.0, past it: replays
        while (!(s.frozen && s.target < 0)) young_step(s, g, E);
        CHECK(g.requests == 2, "replayed from past the blend out (%d)", g.requests);
        s.target = 2.2f;  // forward, then back from 2.2 (before 2.5 - margin): backwards
        while (!(s.frozen && s.target < 0)) young_step(s, g, E);
        s.target = 1.2f;
        while (!(s.frozen && s.target < 0)) young_step(s, g, E);
        CHECK(g.requests == 2 && fabsf(s.clock - 1.2f) < 0.01f, "backwards from before it (%d, clock %f)", g.requests, s.clock);
    }

    // No node may go back past its own start: a clip that began 0.25 s into the pose (too
    // soon after it to count as the blend out) is where going back stops.
    {
        PoseSM s;
        Game g;
        s.reverse = true;
        s.want = 1;
        start(s, g);
        const float begun = 0.25f;
        auto young_at = [&]() {
            if (s.phase == PH_PLAYING) s.young = s.clock > begun ? s.clock - begun : s.clock;
            return step(s, g);
        };
        s.target = 2.0f;
        while (!s.frozen) young_at();
        s.target = 0.15f;
        int n = 0;
        while (!(s.frozen && s.target < 0) && n++ < 1000) young_at();
        CHECK(g.requests == 1 && fabsf(s.clock - (begun + POSE_REV_MARGIN)) < 0.01f, "held at the limit (%d, clock %f)", g.requests,
              s.clock);
        CHECK(s.exit[1] <= 0, "not taken for the blend out (%f)", s.exit[1]);
    }

    // An event action never plays backwards: it replays.
    {
        PoseSM s;
        Game g;
        s.reverse = true;
        s.want = 2;
        start(s, g);
        s.target = 2.0f;
        while (!s.frozen) young_step(s, g, 99);
        s.target = 1.0f;
        int n = 0;
        bool back = false;
        while (!(s.frozen && s.target < 0) && n++ < 2000) back |= young_step(s, g, 99).back > 0;
        CHECK(!back && g.requests == 2 && g.ends == 1, "replayed (%d requests, %d ends)", g.requests, g.ends);
    }

    // A pose with a hold point plays to it and holds there when picked.
    {
        static const PoseDef H[] = {{POSE_NONE, 0, 0, "None"}, {POSE_GESTURE, 130, 2.0f, "Sit", 0.75f, 3}};
        PoseSM s;
        pose_pick(s, H, 1);
        CHECK(s.want == 1 && fabsf(s.target - 1.5f) < 1e-5f && !s.frozen, "plays to 75%% (target %f)", s.target);
        pose_pick(s, DEFS, 1);
        CHECK(s.target < 0, "a pose without one plays on");
        pose_pick(s, DEFS, 0);
        CHECK(s.want == 0 && s.target < 0, "None");
    }

    printf(g_fail ? "%d failed\n" : "posemath: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}

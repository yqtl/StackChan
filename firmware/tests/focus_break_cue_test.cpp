#include "break_cue.hpp"

#include <cassert>
#include <cstdint>

using namespace focus_timer;

namespace {

constexpr uint64_t kMs = 1000ULL;

// Advances through turning and nodding; returns the time listening starts.
uint64_t run_to_listening(BreakCue& cue, uint64_t now_us, int* down_moves = nullptr)
{
    int downs = 0;
    for (int i = 0; i < 2000 && cue.stage() != CueStage::Listening; ++i) {
        now_us += 20 * kMs;
        const CueCommand command = cue.update(now_us, false);
        if (command.move && command.pitch == kCueNodPitch) {
            ++downs;
        }
    }
    assert(cue.stage() == CueStage::Listening);
    if (down_moves) {
        *down_moves = downs;
    }
    return now_us;
}

}  // namespace

int main()
{
    {
        BreakCue cue;
        const CueCommand first = cue.begin(2, 0);
        assert(cue.stage() == CueStage::Turning);
        assert(first.move && first.yaw == kCueFaceYaw && first.pitch == kCueFacePitch);
        assert(first.light && first.rgb == kCueBreakColor);
        assert(!first.start_break);

        // Turning waits for the minimum time, then for motion to settle.
        assert(!cue.update(200 * kMs, false).move);
        assert(cue.stage() == CueStage::Turning);
        assert(!cue.update(500 * kMs, true).move);
        assert(cue.stage() == CueStage::Turning);
        const CueCommand nod = cue.update(600 * kMs, false);
        assert(cue.stage() == CueStage::Nodding);
        assert(nod.move && nod.pitch == kCueNodPitch);

        int downs = 0;
        uint64_t now = run_to_listening(cue, 600 * kMs, &downs);
        assert(downs + 1 == 2);  // The entry command is the first nod.

        // Early taps are ignored; matching taps start the break after settling.
        cue.on_tap(now);
        cue.on_tap(now + 300 * kMs);
        assert(cue.taps() == 2);
        assert(!cue.update(now + 1200 * kMs, false).start_break);
        const CueCommand matched = cue.update(now + 1300 * kMs, false);
        assert(matched.start_break);
        assert(matched.light && matched.rgb == kCueMatchColor);
        assert(cue.celebrating());

        now += 1300 * kMs;
        CueCommand settle;
        for (int i = 0; i < 100 && cue.active(); ++i) {
            now += 20 * kMs;
            settle = cue.update(now, false);
        }
        assert(!cue.active());
        assert(settle.move && settle.yaw == 0 && settle.pitch == 0);
        assert(settle.light && settle.rgb == kCueOffColor);
    }

    {
        // A turn that never reports settled still continues.
        BreakCue cue;
        cue.begin(3, 0);
        cue.update(kCueTurnMaximumUs - 1, true);
        assert(cue.stage() == CueStage::Turning);
        cue.update(kCueTurnMaximumUs, true);
        assert(cue.stage() == CueStage::Nodding);
        int downs = 0;
        run_to_listening(cue, kCueTurnMaximumUs, &downs);
        assert(downs + 1 == 3);
    }

    {
        // Taps during the nods are not counted.
        BreakCue cue;
        cue.begin(2, 0);
        cue.update(kCueTurnMinimumUs, false);
        assert(cue.stage() == CueStage::Nodding);
        cue.on_tap(kCueTurnMinimumUs + 10 * kMs);
        assert(cue.taps() == 0);
    }

    {
        // Wrong count shakes the head, then replays the same rhythm.
        BreakCue cue;
        cue.begin(2, 0);
        uint64_t now = run_to_listening(cue, 0);
        cue.on_tap(now);
        const CueCommand missed = cue.update(now + kCueTapSettleUs, false);
        assert(cue.stage() == CueStage::Missed);
        assert(!missed.start_break);
        assert(missed.light && missed.rgb == kCueMissColor);
        assert(missed.move && missed.yaw == -kCueShakeYaw);
        now += kCueTapSettleUs;
        const CueCommand right = cue.update(now + kCueShakeStepUs, false);
        assert(right.move && right.yaw == kCueShakeYaw);
        cue.update(now + kCueMissUs, false);
        assert(cue.stage() == CueStage::Nodding);
        assert(cue.expected() == 2);
        now = run_to_listening(cue, now + kCueMissUs);
        cue.on_tap(now);
        cue.on_tap(now + 200 * kMs);
        cue.on_tap(now + 400 * kMs);
        assert(!cue.update(now + 400 * kMs + kCueTapSettleUs, false).start_break);
        assert(cue.stage() == CueStage::Missed);
    }

    {
        // Silence rests the robot; it calls again later, a limited number of times.
        BreakCue cue;
        cue.begin(1, 0);
        assert(cue.calls() == 1);
        uint64_t now = run_to_listening(cue, 0);
        const CueCommand rest = cue.update(now + kCueListenTimeoutUs, false);
        assert(cue.stage() == CueStage::Resting);
        assert(rest.move && rest.pitch == 0);
        assert(rest.light && rest.rgb == kCueRestColor);
        now += kCueListenTimeoutUs;
        assert(!cue.update(now + kCueRecallUs - 1, false).move);
        const CueCommand recall = cue.update(now + kCueRecallUs, false);
        assert(cue.stage() == CueStage::Turning);
        assert(recall.light && recall.rgb == kCueBreakColor);
        assert(cue.calls() == 2);

        now = run_to_listening(cue, now + kCueRecallUs);
        cue.update(now + kCueListenTimeoutUs, false);
        now += kCueListenTimeoutUs;
        cue.update(now + kCueRecallUs, false);
        assert(cue.calls() == kCueMaximumCalls);
        now = run_to_listening(cue, now + kCueRecallUs);
        cue.update(now + kCueListenTimeoutUs, false);
        now += kCueListenTimeoutUs;
        cue.update(now + 10 * kCueRecallUs, false);
        assert(cue.stage() == CueStage::Resting);  // No more calls on its own.

        // A tap while resting replays the rhythm.
        const CueCommand replay = cue.on_tap(now + 10 * kCueRecallUs);
        assert(replay.move && cue.stage() == CueStage::Turning);
    }

    {
        // Nod counts are clamped and cancel stops everything.
        BreakCue cue;
        cue.begin(0, 0);
        assert(cue.expected() == kCueMinimumNods);
        cue.begin(9, 0);
        assert(cue.expected() == kCueMaximumNods);
        cue.cancel();
        assert(!cue.active());
        assert(!cue.update(10 * kCueRecallUs, false).move);
        assert(!cue.on_tap(10 * kCueRecallUs).move);
    }

    return 0;
}

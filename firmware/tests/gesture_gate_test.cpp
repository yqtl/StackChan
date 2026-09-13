#include <cassert>
#include <iostream>
#include "gesture_gate.hpp"

int main()
{
    // A first positive frame never fires, even long after boot. Require fresh
    // evidence spanning the debounce interval, including its exact boundary.
    GestureGate gate;
    assert(!gate.update(10000, true, false));
    assert(!gate.update(10100, true, false));
    assert(!gate.update(10249, true, false));
    assert(gate.update(10250, true, false));
    for (int t = 10500; t <= 13000; t += 250) assert(!gate.update(t, true, false));

    // A brief dropout or a different pose must not rearm the trigger. The
    // classifier can be skipped while latched: only no_hand is significant.
    assert(!gate.update(13250, false, true));
    assert(!gate.update(13500, false, true));
    for (int t = 13750; t <= 15000; t += 250) assert(!gate.update(t, false, false));
    assert(gate.latched());
    for (int t = 15250; t < 16250; t += 250) assert(!gate.update(t, false, true));
    assert(gate.latched());
    assert(!gate.update(16250, false, true));
    assert(!gate.latched());
    assert(!gate.update(16500, true, false));
    assert(gate.update(16750, true, false));

    // No-hand, a different pose or low confidence between positives must
    // restart confirmation rather than accumulate unrelated observations.
    for (bool no_hand : {false, true}) {
        GestureGate interrupted;
        assert(!interrupted.update(0, true, false));
        assert(!interrupted.update(200, false, no_hand));
        assert(!interrupted.update(250, true, false));
        assert(!interrupted.update(499, true, false));
        assert(interrupted.update(500, true, false));
    }

    // At both the old hardware cadence and a faster cadence, two positives
    // suffice; the old one-second hold needed three ~830 ms frames.
    for (int cadence : {250, 400, 830}) {
        GestureGate hardware;
        assert(!hardware.update(0, true, false));
        assert(hardware.update(cadence, true, false));
        assert(!hardware.update(2 * cadence, true, false));
    }

    GestureGate stalled;
    assert(!stalled.update(0, true, false));
    const auto resumed = GestureGate::maximum_gap_ms + 1;
    assert(!stalled.update(resumed, true, false)); // Gap is not positive evidence.
    assert(stalled.update(resumed + 250, true, false));
    assert(!stalled.update(resumed + 500, false, true));
    assert(!stalled.update(resumed + 2000, false, true)); // Nor hand-removal evidence.
    assert(stalled.latched());
    assert(!stalled.update(resumed + 2999, false, true));
    assert(stalled.latched());
    assert(!stalled.update(resumed + 3000, false, true));
    assert(!stalled.latched());

    GestureGate clock;
    assert(!clock.update(500, true, false));
    assert(!clock.update(500, true, false)); // Duplicate observation.
    assert(!clock.update(400, true, false)); // Clock reversal restarts the hold.
    assert(!clock.update(649, true, false));
    assert(clock.update(650, true, false));

    std::cout << "PASS: fast confirmation, rearming, interruptions, stalls and clock boundaries\n";
}

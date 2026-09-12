#include <cassert>
#include <iostream>
#include "gesture_gate.hpp"

int main() {
    GestureGate gate;
    for (int t = 0; t < 1000; t += 250) assert(!gate.update(t, true, false));
    assert(gate.update(1000, true, false));
    for (int t = 1250; t <= 5000; t += 250) assert(!gate.update(t, true, false));
    // A brief detection dropout must not rearm the trigger.
    assert(!gate.update(5250, false, true));
    assert(!gate.update(5500, false, true));
    assert(!gate.update(5750, true, false));
    assert(gate.latched());
    // A different pose is not evidence that the hand has been lowered.
    for (int t = 6000; t <= 7500; t += 250) assert(!gate.update(t, false, false));
    assert(gate.latched());
    for (int t = 7750; t <= 8750; t += 250) assert(!gate.update(t, false, true));
    assert(!gate.latched());
    for (int t = 9000; t < 10000; t += 250) assert(!gate.update(t, true, false));
    assert(gate.update(10000, true, false));

    GestureGate interrupted;
    assert(!interrupted.update(0, true, false));
    assert(!interrupted.update(500, true, false));
    assert(!interrupted.update(750, false, false));
    assert(!interrupted.update(1000, true, false));
    assert(!interrupted.update(1500, true, false));
    assert(interrupted.update(2000, true, false));

    GestureGate stale;
    assert(!stale.update(0, true, false));
    assert(!stale.update(2000, true, false)); // No observations during the gap.
    assert(!stale.update(2500, true, false));
    assert(stale.update(3000, true, false));
    assert(!stale.update(3250, false, true));
    assert(!stale.update(5000, false, true)); // Gap cannot count as hand removal.
    assert(stale.latched());

    std::cout << "PASS: hold, repeated gestures, rearming, dropouts and stale observations\n";
}

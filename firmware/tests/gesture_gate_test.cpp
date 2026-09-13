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

    // Hardware produces one classified frame every ~830 ms. These are fresh
    // observations and must be able to complete the one-second hold.
    GestureGate hardware;
    assert(!hardware.update(0, true, false));
    assert(!hardware.update(830, true, false));
    assert(hardware.update(1660, true, false));
    assert(!hardware.update(2490, true, false));
    assert(!hardware.update(2905, false, true));
    assert(!hardware.update(3320, false, true));
    assert(!hardware.update(3735, false, true));
    assert(hardware.latched());
    assert(!hardware.update(4150, false, true));
    assert(!hardware.latched());
    assert(!hardware.update(4980, true, false));
    assert(!hardware.update(5810, true, false));
    assert(hardware.update(6640, true, false));

    GestureGate stalled;
    assert(!stalled.update(0, true, false));
    const auto resumed = GestureGate::maximum_gap_ms + 1;
    assert(!stalled.update(resumed, true, false));
    assert(!stalled.update(resumed + 830, true, false));
    assert(stalled.update(resumed + 1660, true, false));

    std::cout << "PASS: hold, rearming, dropouts, stale observations and 830 ms frames\n";
}

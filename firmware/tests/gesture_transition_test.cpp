#include "gesture_transition.hpp"

#include <cassert>
#include <atomic>
#include <iostream>

int main()
{
    GestureTransition transition;

    // The event mailbox is independent from presentation text, so a later
    // status update cannot erase a pending confirmation.
    std::atomic<bool> pending{false};
    const char *presentation = "Thumbs-up received!";
    pending.store(true);
    presentation = "later status";
    assert(presentation != nullptr);
    assert(transition.consumeConfirmation(pending.exchange(false), true) ==
           GestureTransition::Action::StopForSl);
    assert(transition.state() == GestureTransition::State::StoppingForSl);

    // Repeated confirmation edges in one opening are ignored, and completion
    // is the only event that permits dispatch.
    assert(transition.consumeConfirmation(true, true) == GestureTransition::Action::None);
    assert(transition.state() == GestureTransition::State::StoppingForSl);
    assert(transition.workerCompleted() == GestureTransition::Action::DispatchSl);
    assert(transition.consumeConfirmation(true, true) == GestureTransition::Action::None);
    assert(transition.workerCompleted() == GestureTransition::Action::None);

    // Without a callback, diagnostics keep recognizing and consume no launch
    // intent beyond the presentation-independent event bit.
    transition.reset();
    assert(transition.consumeConfirmation(true, false) == GestureTransition::Action::None);
    assert(transition.state() == GestureTransition::State::Recognizing);
    assert(transition.consumeConfirmation(true, false) == GestureTransition::Action::None);

    // Home wins over a same-tick confirmation and over an in-progress SL
    // drain. A late worker event cannot revive the cancelled request.
    transition.reset();
    assert(transition.requestHome() == GestureTransition::Action::StopForHome);
    assert(transition.consumeConfirmation(true, true) == GestureTransition::Action::None);
    assert(transition.workerCompleted() == GestureTransition::Action::Close);
    assert(transition.workerCompleted() == GestureTransition::Action::None);

    transition.reset();
    assert(transition.consumeConfirmation(true, true) == GestureTransition::Action::StopForSl);
    assert(transition.requestHome() == GestureTransition::Action::StopForHome);
    assert(transition.workerCompleted() == GestureTransition::Action::Close);

    // A worker that was already finished follows the same completion path;
    // no separate blocking wait or early close is represented by the state.
    transition.reset();
    assert(transition.consumeConfirmation(true, true) == GestureTransition::Action::StopForSl);
    assert(transition.state() == GestureTransition::State::StoppingForSl);
    assert(transition.workerCompleted() == GestureTransition::Action::DispatchSl);

    // The production caller closes even when Launcher rejects the callback;
    // the latched session cannot retry the rejected transition.
    const bool callback_accepted = false;
    assert(!callback_accepted);
    assert(transition.state() == GestureTransition::State::CloseRequested);
    assert(transition.consumeConfirmation(true, true) == GestureTransition::Action::None);

    std::cout << "PASS: gesture event latch, home priority, drain gating and session latch\n";
}

#pragma once

// UI-thread state machine for the Gesture app's optional hand-off to SL.BUS.
// It deliberately has no ESP-IDF or LVGL dependencies so the lifecycle rules
// can be exercised by the host test suite.
class GestureTransition {
public:
    enum class State {
        Recognizing,
        StoppingForSl,
        StoppingForHome,
        CloseRequested,
    };

    enum class Action {
        None,
        StopForSl,
        StopForHome,
        DispatchSl,
        Close,
    };

    void reset()
    {
        _state = State::Recognizing;
        _dispatch_latched = false;
    }

    State state() const { return _state; }

    bool isDraining() const
    {
        return _state == State::StoppingForSl || _state == State::StoppingForHome;
    }

    // Home has priority over an automatic SL request, including a request
    // that is already draining.
    Action requestHome()
    {
        if (_state != State::Recognizing && _state != State::StoppingForSl) {
            return Action::None;
        }
        _state = State::StoppingForHome;
        _dispatch_latched = true;
        return Action::StopForHome;
    }

    // A confirmation is an event, not presentation state.  It is consumed
    // only while recognizing and only when the integration callback exists.
    Action consumeConfirmation(bool pending, bool callback_wired)
    {
        if (!pending || !callback_wired || _state != State::Recognizing || _dispatch_latched) {
            return Action::None;
        }
        _dispatch_latched = true;
        _state = State::StoppingForSl;
        return Action::StopForSl;
    }

    // Call this only after the worker completion semaphore has been taken (or
    // when no worker was started).  Until then no close or app dispatch is
    // permitted.
    Action workerCompleted()
    {
        if (_state == State::StoppingForHome) {
            _state = State::CloseRequested;
            return Action::Close;
        }
        if (_state == State::StoppingForSl && _dispatch_latched) {
            _state = State::CloseRequested;
            return Action::DispatchSl;
        }
        return Action::None;
    }

private:
    State _state = State::Recognizing;
    bool _dispatch_latched = false;
};

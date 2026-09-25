/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace sl_bus {

class SlBusCloseState {
public:
    enum class State {
        Running,
        Draining,
        CloseRequested,
    };

    enum class Action {
        None,
        Stop,
        Close,
    };

    void reset()
    {
        _state = State::Running;
        _worker_completed = false;
    }

    State state() const { return _state; }

    bool is_draining() const { return _state == State::Draining; }

    Action request_home()
    {
        if (_state != State::Running) {
            return Action::None;
        }
        _state = State::Draining;
        return Action::Stop;
    }

    Action worker_completed()
    {
        if (_state == State::Running) {
            _worker_completed = true;
            return Action::None;
        }
        if (_state == State::Draining) {
            _worker_completed = true;
            _state = State::CloseRequested;
            return Action::Close;
        }
        return Action::None;
    }

private:
    State _state = State::Running;
    bool _worker_completed = false;
};

}  // namespace sl_bus

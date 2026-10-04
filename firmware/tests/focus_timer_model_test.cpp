#include "focus_timer_model.hpp"

#include <cassert>
#include <cstdint>
#include <limits>

using focus_timer::Phase;
using focus_timer::State;
using focus_timer::TimerModel;
using focus_timer::kMicrosecondsPerSecond;

int main()
{
    {
        TimerModel timer;
        assert(timer.start(Phase::Focus, 60, 100));
        assert(timer.deadline_us() == 60 * kMicrosecondsPerSecond + 100);
        assert(timer.remaining_seconds(15 * kMicrosecondsPerSecond + 100) == 45);
        assert(!timer.update(20 * kMicrosecondsPerSecond + 100));
        assert(!timer.update(59 * kMicrosecondsPerSecond + 100));
        assert(timer.update(60 * kMicrosecondsPerSecond + 100));
        assert(timer.state() == State::Completed);
        assert(!timer.update(600 * kMicrosecondsPerSecond));
        assert(timer.phase() == Phase::Focus);  // No automatic phase advancement.
    }

    {
        TimerModel timer;
        const uint64_t start_us = (static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 2500ULL) * 1000ULL;
        assert(timer.start(Phase::Break, 60, start_us));
        assert(timer.deadline_us() == start_us + 60 * kMicrosecondsPerSecond);
        assert(!timer.update(start_us + 59999999ULL));
        assert(timer.update(start_us + 60 * kMicrosecondsPerSecond));
    }

    {
        TimerModel timer;
        assert(timer.start(Phase::Focus, 60, 1000));
        assert(timer.pause(10 * kMicrosecondsPerSecond + 1000));
        assert(timer.state() == State::Paused);
        const auto remaining = timer.remaining_us(999999999999ULL);
        assert(remaining == 50 * kMicrosecondsPerSecond);
        assert(!timer.update(999999999999ULL));
        assert(timer.resume(2000000000000ULL));
        assert(timer.deadline_us() == 2000000000000ULL + remaining);
        assert(!timer.update(timer.deadline_us() - 1));
        assert(timer.update(timer.deadline_us()));
    }

    {
        TimerModel timer;
        assert(timer.start(Phase::Focus, focus_timer::kFocusMaximumSeconds, 0));
        assert(timer.extend(kMicrosecondsPerSecond));
        assert(timer.remaining_seconds(kMicrosecondsPerSecond) == focus_timer::kFocusMaximumSeconds);
        assert(!timer.extend(kMicrosecondsPerSecond));
        assert(timer.pause(2 * kMicrosecondsPerSecond));
        assert(timer.extend(999999999999ULL));  // Paused extensions do not depend on wall time.
        assert(timer.state() == State::Paused);
        assert(timer.remaining_seconds(0) == focus_timer::kFocusMaximumSeconds);

        assert(timer.start(Phase::Break, focus_timer::kBreakMaximumSeconds, 3 * kMicrosecondsPerSecond) == false);
    }

    {
        TimerModel timer;
        assert(timer.start(Phase::Break, 1, 0));
        assert(timer.update(kMicrosecondsPerSecond));
        assert(timer.extend(2 * kMicrosecondsPerSecond));
        assert(timer.state() == State::Running);
        assert(timer.phase() == Phase::Break);
        assert(timer.remaining_seconds(2 * kMicrosecondsPerSecond) == 300);
        assert(timer.deadline_us() == 302 * kMicrosecondsPerSecond);
    }

    {
        TimerModel timer;
        assert(timer.start(Phase::Focus, 1, 0));
        assert(!timer.update(kMicrosecondsPerSecond, true));
        assert(timer.state() == State::Ready);
        assert(timer.deadline_us() == 0);
        assert(!timer.update(kMicrosecondsPerSecond));

        assert(timer.start(Phase::Focus, 1, 0));
        timer.cancel();
        TimerModel reopened;
        assert(reopened.state() == State::Ready);
        assert(reopened.duration_us() == 0);
    }

    return 0;
}

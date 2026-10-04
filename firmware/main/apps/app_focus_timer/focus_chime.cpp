/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "focus_chime.h"

#include <board.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <hal/board/cores3_audio_codec.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int kChimeDurationMs = 300;
constexpr int kNoteDurationMs = 150;
constexpr int kMaximumChunkSamples = 512;
constexpr float kPiTimesTwo = 6.28318530718f;
constexpr float kAmplitude = 4300.0f;

}  // namespace

FocusChime::FocusChime() : _done(xSemaphoreCreateBinary())
{
}

FocusChime::~FocusChime()
{
    shutdown();
    if (_done) {
        vSemaphoreDelete(_done);
        _done = nullptr;
    }
}

bool FocusChime::start()
{
    if (_active || !_done) {
        return false;
    }
    xSemaphoreTake(_done, 0);
    _cancel_requested.store(false);
    _result.store(Result::None);
    _active = true;
    if (xTaskCreatePinnedToCore(worker, "focus_chime", 4096, this, 3, nullptr, 0) != pdPASS) {
        _active = false;
        _result.store(Result::Failed);
        return false;
    }
    return true;
}

void FocusChime::request_cancel()
{
    _cancel_requested.store(true);
}

bool FocusChime::poll_finished(Result* result)
{
    if (!_active || xSemaphoreTake(_done, 0) != pdTRUE) {
        return false;
    }
    _active = false;
    if (result) {
        *result = _result.load();
    }
    return true;
}

void FocusChime::shutdown()
{
    request_cancel();
    if (_active && _done) {
        xSemaphoreTake(_done, portMAX_DELAY);
        _active = false;
    }
}

void FocusChime::worker(void* context)
{
    auto* chime = static_cast<FocusChime*>(context);
    const Result result = chime->play();
    chime->_result.store(result);

    // The app may be destroyed as soon as it observes this signal. Do not
    // access the chime object after giving the semaphore.
    const SemaphoreHandle_t done = chime->_done;
    xSemaphoreGive(done);
    vTaskDelete(nullptr);
}

FocusChime::Result FocusChime::play()
{
    auto* codec = static_cast<CoreS3AudioCodec*>(Board::GetInstance().GetAudioCodec());
    if (!codec || codec->output_sample_rate() <= 0 || codec->output_enabled()) {
        return Result::Failed;
    }
    if (_cancel_requested.load()) {
        return Result::Cancelled;
    }
    if (!codec->TryAppEnableOutput(true)) {
        return Result::Failed;
    }

    const uint32_t sample_rate = static_cast<uint32_t>(codec->output_sample_rate());
    const uint32_t samples_per_note = std::max<uint32_t>(1, sample_rate * kNoteDurationMs / 1000U);
    const uint32_t total_samples = std::max<uint32_t>(1, sample_rate * kChimeDurationMs / 1000U);
    const uint32_t ramp_samples = std::max<uint32_t>(1, sample_rate / 100U);
    const uint32_t chunk_samples = std::max<uint32_t>(1, std::min<uint32_t>(sample_rate / 100U,
                                                                           kMaximumChunkSamples));
    std::vector<int16_t> buffer;
    buffer.reserve(chunk_samples);

    const int64_t playback_start_us = esp_timer_get_time();
    uint32_t offset = 0;
    Result result = Result::Played;
    while (offset < total_samples) {
        if (_cancel_requested.load()) {
            result = Result::Cancelled;
            break;
        }

        const uint32_t count = std::min(chunk_samples, total_samples - offset);
        buffer.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t sample = offset + i;
            const uint32_t note_sample = sample % samples_per_note;
            const float frequency = sample < samples_per_note ? 523.25f : 659.25f;
            float envelope = 1.0f;
            if (note_sample < ramp_samples) {
                envelope = static_cast<float>(note_sample) / ramp_samples;
            } else if (samples_per_note - note_sample <= ramp_samples) {
                envelope = static_cast<float>(samples_per_note - note_sample) / ramp_samples;
            }
            const float angle = kPiTimesTwo * frequency * static_cast<float>(note_sample) / sample_rate;
            buffer[i] = static_cast<int16_t>(kAmplitude * envelope * std::sin(angle));
        }

        if (!codec->TryAppOutputData(buffer)) {
            result = Result::Failed;
            break;
        }
        offset += count;

        // Keep playback close to real time even on codec drivers that accept
        // a short PCM buffer faster than the samples can be heard.
        const int64_t chunk_deadline_us = playback_start_us +
            static_cast<int64_t>(offset) * 1000000LL / sample_rate;
        while (!_cancel_requested.load() && esp_timer_get_time() < chunk_deadline_us) {
            vTaskDelay(1);
        }
    }

    if (!codec->TryAppEnableOutput(false)) {
        result = Result::Failed;
    }
    if (_cancel_requested.load() && result != Result::Failed) {
        return Result::Cancelled;
    }
    return result;
}

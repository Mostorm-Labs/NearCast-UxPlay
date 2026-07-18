#pragma once

#include <algorithm>
#include <cstdint>

namespace uxplay_audio_recovery {

static constexpr std::uint64_t kFreshInputWindowUs = 500000;
static constexpr std::uint64_t kPipelineGraceUs = 2000000;
static constexpr std::uint64_t kOutputStallUs = 2000000;
static constexpr std::uint64_t kStableOutputResetUs = 10000000;

struct HealthState {
    std::uint64_t lastInputUs = 0;
    std::uint64_t lastPushUs = 0;
    std::uint64_t lastOutputUs = 0;
    std::uint64_t pipelineStartedUs = 0;
    std::uint64_t stableOutputSinceUs = 0;
    unsigned int recoveryAttempts = 0;
    double lastRmsDb = 0.0;
    double lastPeakDb = 0.0;
};

inline std::uint64_t RecoveryDelayUs(unsigned int completedAttempts)
{
    if (completedAttempts == 0) {
        return 0;
    }
    if (completedAttempts == 1) {
        return 250000;
    }
    if (completedAttempts == 2) {
        return 1000000;
    }
    return 5000000;
}

inline bool ShouldRecoverSilentPipeline(
    const HealthState& state,
    std::uint64_t nowUs,
    bool enabled,
    bool rendererRunning,
    bool recoveryPending,
    bool stopPending)
{
    if (!enabled || !rendererRunning || recoveryPending || stopPending ||
        state.pipelineStartedUs == 0 || state.lastPushUs == 0 ||
        nowUs < state.pipelineStartedUs || nowUs < state.lastPushUs) {
        return false;
    }
    if (nowUs - state.lastPushUs > kFreshInputWindowUs ||
        nowUs - state.pipelineStartedUs < kPipelineGraceUs) {
        return false;
    }
    const std::uint64_t outputReferenceUs =
        std::max(state.pipelineStartedUs, state.lastOutputUs);
    return nowUs >= outputReferenceUs &&
           nowUs - outputReferenceUs >= kOutputStallUs;
}

inline bool NoteOutputAndMaybeReset(HealthState& state, std::uint64_t nowUs)
{
    state.lastOutputUs = nowUs;
    if (state.recoveryAttempts == 0) {
        state.stableOutputSinceUs = 0;
        return false;
    }
    if (state.stableOutputSinceUs == 0 || nowUs < state.stableOutputSinceUs) {
        state.stableOutputSinceUs = nowUs;
        return false;
    }
    if (nowUs - state.stableOutputSinceUs < kStableOutputResetUs) {
        return false;
    }
    state.recoveryAttempts = 0;
    state.stableOutputSinceUs = 0;
    return true;
}

inline void ResetSessionHealth(HealthState& state)
{
    state = HealthState{};
}

} // namespace uxplay_audio_recovery

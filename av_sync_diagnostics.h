#pragma once

#include <cstdint>
#include <limits>

#include "av_sync_strategy.h"

namespace uxplay_av_sync {

static constexpr std::uint64_t kSessionAnchorLeadNs = 10000000ULL;
static constexpr std::uint64_t kComparableSampleWindowUs = 500000ULL;

struct SessionClockState {
    bool active = false;
    std::uint64_t sourceAnchorNs = 0;
    std::uint64_t targetAnchorNs = 0;
};

inline std::uint64_t SaturatingAdd(std::uint64_t lhs, std::uint64_t rhs)
{
    if (std::numeric_limits<std::uint64_t>::max() - lhs < rhs) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return lhs + rhs;
}

inline std::uint64_t MapSessionTimestamp(
    SessionClockState& state,
    std::uint64_t convertedLocalNs,
    std::uint64_t localNowNs)
{
    if (!state.active) {
        state.active = true;
        state.sourceAnchorNs = convertedLocalNs;
        state.targetAnchorNs = SaturatingAdd(localNowNs, kSessionAnchorLeadNs);
    }

    if (convertedLocalNs >= state.sourceAnchorNs) {
        return SaturatingAdd(
            state.targetAnchorNs,
            convertedLocalNs - state.sourceAnchorNs);
    }

    const std::uint64_t backwardsNs = state.sourceAnchorNs - convertedLocalNs;
    return backwardsNs <= state.targetAnchorNs
        ? state.targetAnchorNs - backwardsNs
        : 0;
}

inline std::int64_t SignedDelta(std::uint64_t lhs, std::uint64_t rhs)
{
    if (lhs >= rhs) {
        const std::uint64_t delta = lhs - rhs;
        return delta > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(delta);
    }
    const std::uint64_t delta = rhs - lhs;
    return delta > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        ? std::numeric_limits<std::int64_t>::min()
        : -static_cast<std::int64_t>(delta);
}

inline std::int64_t SaturatingSubtract(std::int64_t lhs, std::int64_t rhs)
{
    if (rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() + rhs) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if (rhs < 0 && lhs > std::numeric_limits<std::int64_t>::max() + rhs) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return lhs - rhs;
}

inline bool SamplesComparable(
    std::uint64_t audioSampleUs,
    std::uint64_t videoSampleUs)
{
    const std::uint64_t ageDifferenceUs = audioSampleUs >= videoSampleUs
        ? audioSampleUs - videoSampleUs
        : videoSampleUs - audioSampleUs;
    return audioSampleUs != 0 && videoSampleUs != 0 &&
           ageDifferenceUs <= kComparableSampleWindowUs;
}

} // namespace uxplay_av_sync

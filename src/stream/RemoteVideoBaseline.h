#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace stream {

// =====wjy====
struct RemoteVideoBaselineSnapshot {
    std::uint64_t sampleTimeMs = 0;
    std::uint64_t framesReceived = 0;
    std::uint64_t framesPresented = 0;
    std::uint64_t framesDropped = 0;
    std::uint64_t uiThreadPresentCalls = 0;
    double presentedFps = 0.0;
    double frameAgeP95Ms = 0.0;
};

class RemoteVideoBaseline final {
public:
    explicit RemoteVideoBaseline(std::size_t ageCapacity = 256)
        : ageCapacity_(std::max<std::size_t>(1, ageCapacity))
    {
    }

    void recordReceived() noexcept
    {
        std::lock_guard lock(mutex_);
        ++framesReceived_;
    }

    void recordPresented(std::uint64_t frameAgeMs, bool onUiThread) noexcept
    {
        std::lock_guard lock(mutex_);
        ++framesPresented_;
        if (onUiThread) {
            ++uiThreadPresentCalls_;
        }
        frameAgesMs_.push_back(frameAgeMs);
        if (frameAgesMs_.size() > ageCapacity_) {
            frameAgesMs_.erase(frameAgesMs_.begin());
        }
    }

    void recordDropped() noexcept
    {
        std::lock_guard lock(mutex_);
        ++framesDropped_;
    }

    RemoteVideoBaselineSnapshot snapshot(std::uint64_t sampleTimeMs) const
    {
        std::lock_guard lock(mutex_);
        RemoteVideoBaselineSnapshot result;
        result.sampleTimeMs = sampleTimeMs;
        result.framesReceived = framesReceived_;
        result.framesPresented = framesPresented_;
        result.framesDropped = framesDropped_;
        result.uiThreadPresentCalls = uiThreadPresentCalls_;
        if (lastSampleTimeMs_ != 0 && sampleTimeMs > lastSampleTimeMs_) {
            result.presentedFps = static_cast<double>(framesPresented_ - lastSamplePresented_)
                * 1000.0 / static_cast<double>(sampleTimeMs - lastSampleTimeMs_);
        }
        lastSampleTimeMs_ = sampleTimeMs;
        lastSamplePresented_ = framesPresented_;
        if (!frameAgesMs_.empty()) {
            std::vector<std::uint64_t> sorted = frameAgesMs_;
            std::sort(sorted.begin(), sorted.end());
            const std::size_t index = (sorted.size() * 95 + 99) / 100 - 1;
            result.frameAgeP95Ms = static_cast<double>(sorted[std::min(index, sorted.size() - 1)]);
        }
        return result;
    }

private:
    const std::size_t ageCapacity_;
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> frameAgesMs_;
    std::uint64_t framesReceived_ = 0;
    std::uint64_t framesPresented_ = 0;
    std::uint64_t framesDropped_ = 0;
    std::uint64_t uiThreadPresentCalls_ = 0;
    mutable std::uint64_t lastSampleTimeMs_ = 0;
    mutable std::uint64_t lastSamplePresented_ = 0;
};
// ===end====

} // namespace stream

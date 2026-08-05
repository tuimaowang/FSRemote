#pragma once

#include <optional>
#include <utility>

namespace uu {

// =====wjy====
template <typename Frame>
class LatestEncodeFrameSlot final {
public:
    struct Item {
        Frame frame;
        bool forceKeyframe = false;
    };

    bool push(Frame frame, bool forceKeyframe)
    {
        const bool replaced = pending_.has_value();
        if (pending_) forceKeyframe = forceKeyframe || pending_->forceKeyframe; // wjy: 覆盖旧帧时继承关键帧请求，直到下一次真正编码成功。
        pending_ = Item{std::move(frame), forceKeyframe}; // wjy: 容量恒为一，新帧只替换旧待处理帧，不形成远控延迟队列。
        return replaced;
    }

    std::optional<Item> take()
    {
        std::optional<Item> result = std::move(pending_);
        pending_.reset();
        return result;
    }

    bool empty() const { return !pending_.has_value(); }
    void clear() { pending_.reset(); }

private:
    std::optional<Item> pending_;
};
// ===end====

} // namespace uu

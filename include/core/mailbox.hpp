#pragma once

#include "core/bounded_queue.hpp"
#include <atomic>
#include <functional>

namespace rkmon::core {

// 有明确收件人的控制邮箱；不广播、不执行业务回调、不丢弃旧命令。
// Wakeup 仅负责唤醒接收方事件循环，必须线程安全、快速返回，不抛异常。
// 回调在队列锁外执行，事件循环收到唤醒后应反复 try_receive，直到队列为空。
template <typename T>
class Mailbox {
public:
    using Wakeup = std::function<void()>;
    explicit Mailbox(std::size_t capacity, Wakeup wakeup = {})
        : queue_(capacity), wakeup_(std::move(wakeup)) {}

    PushResult try_post(T event) {
        const auto result = queue_.try_push(std::move(event));
        if (result == PushResult::accepted) notify();
        return result;
    }
    std::optional<T> try_receive() { return queue_.try_pop(); }
    std::optional<T> receive() { return queue_.pop(); }
    void close(CloseMode mode = CloseMode::drain) {
        queue_.close(mode);
        notify();
    }
    [[nodiscard]] std::size_t size() const { return queue_.size(); }
    [[nodiscard]] bool closed() const { return queue_.closed(); }
    [[nodiscard]] std::size_t notification_failures() const noexcept {
        return notification_failures_.load(std::memory_order_relaxed);
    }
private:
    void notify() noexcept {
        // 已提交的事件不能因通知异常被误认为提交失败，避免调用方重复提交。
        // 异常计数供健康检查发现；无可靠唤醒时接收方需要定时检查邮箱。
        try { if (wakeup_) wakeup_(); }
        catch (...) { notification_failures_.fetch_add(1, std::memory_order_relaxed); }
    }
    BoundedQueue<T> queue_;
    const Wakeup wakeup_;
    std::atomic<std::size_t> notification_failures_{0};
};

} // namespace rkmon::core

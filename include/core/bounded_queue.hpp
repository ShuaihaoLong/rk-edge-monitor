#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace rkmon::core {

enum class OverflowPolicy { reject, drop_oldest };
enum class PushResult { accepted, dropped_oldest, full, closed };
enum class CloseMode { drain, discard };

// 多生产者/多消费者队列。close 后不可重新打开；重启服务时创建新队列。
// push/pop 的返回值不区分 nullptr 等业务值，仅用 optional 表示结束。
template <typename T>
class BoundedQueue {
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "Queue payload must have a noexcept move constructor");
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("queue capacity must be positive");
    }
    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    // 阻塞等待空位；close 唤醒等待中的生产者并返回 false。
    bool push(T value) {
        std::unique_lock lock(mutex_);
        not_full_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
        if (closed_) return false;
        queue_.push_back(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    // 不等待空位，适用于视频回调/控制邮箱。该接口可能短暂等待互斥锁。
    // 按值传参：无论成功与否，调用方移入的对象均已交出所有权。
    PushResult try_push(T value, OverflowPolicy policy = OverflowPolicy::reject) {
        std::optional<T> discarded;
        std::unique_lock lock(mutex_);
        if (closed_) return PushResult::closed;
        const bool full = queue_.size() == capacity_;
        if (full && policy == OverflowPolicy::reject) return PushResult::full;
        // 先成功插入再移出旧值，分配失败时不会丢失原队列内容。
        queue_.push_back(std::move(value));
        if (full) {
            discarded.emplace(std::move(queue_.front()));
            queue_.pop_front();
            ++dropped_;
        }
        lock.unlock();
        not_empty_.notify_one();
        // discarded 在锁外析构，避免帧资源释放占用队列锁。
        return full ? PushResult::dropped_oldest : PushResult::accepted;
    }

    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        return take(lock);
    }

    std::optional<T> try_pop() {
        std::unique_lock lock(mutex_);
        return take(lock);
    }

    // drain：停止接收，消费者继续取完已有数据；discard：立即取消待处理数据。
    void close(CloseMode mode = CloseMode::drain) {
        std::deque<T> discarded;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            if (mode == CloseMode::discard) queue_.swap(discarded);
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }
    [[nodiscard]] bool closed() const { std::lock_guard lock(mutex_); return closed_; }
    [[nodiscard]] std::size_t size() const { std::lock_guard lock(mutex_); return queue_.size(); }
    [[nodiscard]] std::size_t dropped() const { std::lock_guard lock(mutex_); return dropped_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    std::optional<T> take(std::unique_lock<std::mutex>& lock) {
        if (queue_.empty()) return std::nullopt;
        std::optional<T> value(std::move(queue_.front()));
        queue_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return value;
    }
    const std::size_t capacity_;
    std::deque<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_, not_full_;
    bool closed_{false};
    std::size_t dropped_{0};
};

} // namespace rkmon::core

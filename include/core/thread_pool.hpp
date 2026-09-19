#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace rkmon::core {

// 固定工作线程，任务显式绑定线程索引，便于复用线程独占资源。
// 每个线程最多一个待执行或执行中任务；忙时拒绝提交，不建立无界队列。
// stop 取消尚未开始的任务，其 future 收到 broken_promise；运行中的任务正常结束。
// 生命周期由外部控制线程串行管理；on_exit 在对应工作线程退出前调用，必须不抛异常。
class ThreadPool {
public:
    explicit ThreadPool(std::size_t count, std::function<void(std::size_t)> on_exit = {})
        : slots_(count), on_exit_(std::move(on_exit)) {
        if (!count) throw std::invalid_argument("thread pool requires workers");
        threads_.reserve(count);
        try {
            for (std::size_t i = 0; i < count; ++i)
                threads_.emplace_back([this, i] { run(i); });
        } catch (...) { request_stop(); join(); throw; }
    }
    ~ThreadPool() { request_stop(); join(); }
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template<class Function>
    auto try_submit(std::size_t index, Function&& function)
        -> std::optional<std::future<std::invoke_result_t<std::decay_t<Function>, std::size_t>>> {
        using Result = std::invoke_result_t<std::decay_t<Function>, std::size_t>;
        std::unique_lock lock(mutex_);
        if (index >= slots_.size()) throw std::out_of_range("thread pool worker index");
        if (stopping_ || slots_[index].busy) return std::nullopt;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            [fn = std::forward<Function>(function), index]() mutable { return std::invoke(fn, index); });
        auto future = task->get_future();
        slots_[index].task = [task] { (*task)(); };
        slots_[index].busy = true;
        lock.unlock();
        ready_.notify_all();
        return std::optional<std::future<Result>>(std::move(future));
    }
    void request_stop() noexcept {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        ready_.notify_all();
    }
    void join() noexcept {
        for (auto& thread : threads_) if (thread.joinable()) thread.join();
    }
    std::size_t size() const noexcept { return slots_.size(); }

private:
    struct Slot { std::function<void()> task; bool busy{false}; };
    void run(std::size_t index) {
        for (;;) {
            std::function<void()> task;
            bool stopping;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || bool(slots_[index].task); });
                stopping = stopping_;
                task.swap(slots_[index].task);
            }
            // 任务及被其持有的资源在锁外释放；packaged_task 将任务异常传给 future。
            if (stopping) break;
            task();
            { std::lock_guard lock(mutex_); slots_[index].busy = false; }
        }
        if (on_exit_) on_exit_(index);
    }
    std::vector<Slot> slots_;
    std::function<void(std::size_t)> on_exit_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool stopping_{false};
    std::vector<std::thread> threads_;
};

} // namespace rkmon::core

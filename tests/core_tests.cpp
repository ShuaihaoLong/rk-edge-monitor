#include "core/bounded_queue.hpp"
#include "core/mailbox.hpp"
#include "core/service_manager.hpp"
#include "core/logger.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace rkmon::core;
using namespace std::chrono_literals;
#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error("failed: " #condition); } while (false)

template<class Exception, class Function> void expect_throw(Function fn) {
    bool caught = false;
    try { fn(); } catch (const Exception&) { caught = true; }
    REQUIRE(caught);
}

void queue_tests() {
    expect_throw<std::invalid_argument>([] { BoundedQueue<int> q(0); });
    BoundedQueue<int> q(2);
    REQUIRE(q.try_push(1) == PushResult::accepted);
    REQUIRE(q.try_push(2) == PushResult::accepted);
    REQUIRE(q.try_push(3) == PushResult::full);
    REQUIRE(q.try_push(3, OverflowPolicy::drop_oldest) == PushResult::dropped_oldest);
    REQUIRE(q.size() == 2 && q.dropped() == 1);
    q.close();
    REQUIRE(q.pop() == 2 && q.pop() == 3);
    REQUIRE(!q.pop());
    REQUIRE(!q.push(4));
    REQUIRE(q.try_push(4) == PushResult::closed);
    q.close();

    BoundedQueue<std::unique_ptr<int>> moved(1);
    REQUIRE(moved.push(std::make_unique<int>(7)));
    REQUIRE(**moved.pop() == 7);
    moved.push(std::make_unique<int>(8));
    moved.close(CloseMode::discard);
    REQUIRE(!moved.pop());

    // 等待中的消费者/生产者必须在 close 后退出。CTest 总超时兜底检测死锁。
    BoundedQueue<int> empty(1);
    auto consumer = std::async(std::launch::async, [&] { return empty.pop(); });
    const auto blocked_consumer = consumer.wait_for(20ms);
    empty.close();
    REQUIRE(blocked_consumer == std::future_status::timeout);
    REQUIRE(consumer.wait_for(1s) == std::future_status::ready);
    REQUIRE(!consumer.get());
    BoundedQueue<int> full(1);
    full.push(1);
    auto producer = std::async(std::launch::async, [&] { return full.push(2); });
    const auto blocked_producer = producer.wait_for(20ms);
    full.close();
    REQUIRE(blocked_producer == std::future_status::timeout);
    REQUIRE(producer.wait_for(1s) == std::future_status::ready);
    REQUIRE(!producer.get());

    // 多生产者/消费者实测：每个消息恰好收到一次，而不只检查总和。
    BoundedQueue<int> concurrent(8);
    constexpr int count = 4000;
    std::vector<std::atomic<int>> seen(count);
    for (auto& item : seen) item.store(0);
    std::vector<std::thread> consumers, producers;
    std::atomic<bool> invalid{false};
    for (int i = 0; i < 3; ++i) consumers.emplace_back([&] {
        while (auto value = concurrent.pop()) {
            if (*value < 0 || *value >= count) invalid = true;
            else seen[*value].fetch_add(1);
        }
    });
    for (int i = 0; i < 4; ++i) producers.emplace_back([&, i] {
        for (int j = i * 1000; j < (i + 1) * 1000; ++j) concurrent.push(j);
    });
    for (auto& thread : producers) thread.join();
    concurrent.close();
    for (auto& thread : consumers) thread.join();
    REQUIRE(!invalid);
    for (auto& item : seen) REQUIRE(item == 1);
}

void mailbox_tests() {
    int wakes = 0;
    Mailbox<int> box(1, [&] { ++wakes; });
    REQUIRE(box.try_post(10) == PushResult::accepted);
    REQUIRE(box.try_post(20) == PushResult::full);
    REQUIRE(wakes == 1);
    REQUIRE(box.try_receive() == 10);
    REQUIRE(!box.try_receive());
    box.close();
    REQUIRE(wakes == 2);
    REQUIRE(!box.receive());
    REQUIRE(box.try_post(30) == PushResult::closed);
    Mailbox<int> broken(1, [] { throw std::runtime_error("wakeup failed"); });
    REQUIRE(broken.try_post(42) == PushResult::accepted);
    REQUIRE(broken.notification_failures() == 1);
    REQUIRE(broken.try_receive() == 42);
    // 唤醒回调可直接读取队列，验证提交方没有持有队列锁。
    Mailbox<int>* pointer = nullptr;
    int received = 0;
    Mailbox<int> reentrant(1, [&] { received = pointer->try_receive().value_or(0); });
    pointer = &reentrant;
    REQUIRE(reentrant.try_post(7) == PushResult::accepted);
    REQUIRE(received == 7);
}

class FakeService : public IService {
public:
    FakeService(std::string id, std::vector<std::string>& trace, int failure = 0)
        : id_(std::move(id)), trace_(trace), failure_(failure) {}
    bool start() override {
        trace_.push_back("start:" + id_);
        active_ = true; // 模拟启动了一部分资源之后失败。
        if (failure_ == 2) throw std::runtime_error("fake failure");
        return failure_ == 0;
    }
    void request_stop() noexcept override { trace_.push_back("stop:" + id_); active_ = false; }
    void join() noexcept override { trace_.push_back("join:" + id_); }
    bool running() const noexcept override { return active_; }
    std::string_view name() const noexcept override { return id_; }
private:
    std::string id_;
    std::vector<std::string>& trace_;
    int failure_;
    bool active_{false};
};

// 真实线程服务：覆盖 manager 析构能唤醒阻塞线程并 join 的契约。
class Worker final : public IService {
public:
    ~Worker() override { stop(); }
    bool start() override {
        queue_ = std::make_unique<BoundedQueue<int>>(1);
        active_ = true;
        thread_ = std::thread([this] { while (queue_->pop()) {} active_ = false; });
        return true;
    }
    void request_stop() noexcept override { if (queue_) queue_->close(); }
    void join() noexcept override { if (thread_.joinable()) thread_.join(); }
    bool running() const noexcept override { return active_.load(); }
    std::string_view name() const noexcept override { return "worker"; }
private:
    std::unique_ptr<BoundedQueue<int>> queue_;
    std::thread thread_;
    std::atomic<bool> active_{false};
};

void service_tests() {
    const std::vector<std::string> normal = {"start:a", "start:b", "stop:b", "stop:a", "join:b", "join:a"};
    std::vector<std::string> trace;
    {
        ServiceManager manager;
        expect_throw<std::invalid_argument>([&] { manager.add(nullptr); });
        manager.add(std::make_unique<FakeService>("a", trace));
        expect_throw<std::invalid_argument>([&] { manager.add(std::make_unique<FakeService>("a", trace)); });
        manager.add(std::make_unique<FakeService>("b", trace));
        REQUIRE(manager.start_all());
        REQUIRE(manager.start_all()); // 不重复启动。
        REQUIRE(manager.health()[0].state == ServiceState::running);
        expect_throw<std::logic_error>([&] { manager.add(std::make_unique<FakeService>("c", trace)); });
        manager.stop_all();
        manager.stop_all();
        REQUIRE(trace == normal);
        trace.clear();
        REQUIRE(manager.start_all()); // 实现支持重启时，管理器可重新按顺序启动。
    }
    REQUIRE(trace == normal); // 析构自动停止。
    for (int failure : {1, 2}) {
        trace.clear();
        ServiceManager manager;
        manager.add(std::make_unique<FakeService>("a", trace));
        manager.add(std::make_unique<FakeService>("b", trace, failure));
        manager.add(std::make_unique<FakeService>("c", trace));
        REQUIRE(!manager.start_all());
        REQUIRE(!manager.last_error().empty());
        REQUIRE(trace == normal); // 未启动 c，失败 b 也得到 stop/join。
        REQUIRE(manager.health()[0].state == ServiceState::stopped);
    }
    {
        ServiceManager manager;
        manager.add(std::make_unique<Worker>());
        REQUIRE(manager.start_all());
    }
}

void logger_tests() {
    using namespace rkmon::log;
    shutdown();
    expect_throw<std::logic_error>([] { get(); });
    Options options;
    options.console = false;
    options.file_path.clear();
    expect_throw<std::invalid_argument>([&] { init(options); });
    const auto path = std::filesystem::temp_directory_path() / ("rkmon-log-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(path);
    options.file_path = (path / "test.log").string();
    options.max_file_size = 512;
    options.rotated_files = 2;
    init(options);
    expect_throw<std::logic_error>([&] { init(options); });
    std::vector<std::thread> writers;
    for (int i = 0; i < 3; ++i) writers.emplace_back([i] {
        auto logger = get();
        for (int j = 0; j < 40; ++j) logger->info("worker {} item {}: rotation test message", i, j);
    });
    for (auto& writer : writers) writer.join();
    RKMON_WARN("final-marker");
    shutdown();
    shutdown();
    std::ifstream stream(options.file_path);
    const std::string content((std::istreambuf_iterator<char>(stream)), {});
    REQUIRE(content.find("final-marker") != std::string::npos);
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        REQUIRE(entry.file_size() <= options.max_file_size);
        ++files;
    }
    REQUIRE(files == 3);
    options.level = spdlog::level::warn;
    init(options);
    RKMON_INFO("hidden-marker");
    RKMON_ERROR("visible-marker");
    shutdown();
    std::ifstream filtered(options.file_path);
    const std::string result((std::istreambuf_iterator<char>(filtered)), {});
    REQUIRE(result.find("hidden-marker") == std::string::npos);
    REQUIRE(result.find("visible-marker") != std::string::npos);
    std::filesystem::remove_all(path);
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("expected queue/mailbox/service/logger");
        const std::string suite = argv[1];
        if (suite == "queue") queue_tests();
        else if (suite == "mailbox") mailbox_tests();
        else if (suite == "service") service_tests();
        else if (suite == "logger") logger_tests();
        else throw std::invalid_argument("unknown suite");
        std::cout << "PASS: " << suite << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        rkmon::log::shutdown();
        return 1;
    }
}

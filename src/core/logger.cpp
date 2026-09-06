#include "core/logger.hpp"
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace rkmon::log {
namespace {
std::mutex logger_mutex;
std::shared_ptr<spdlog::logger> logger;
}

void init(const Options& options) {
    std::lock_guard lock(logger_mutex);
    if (logger) throw std::logic_error("rkmon logger already initialized");
    if (!options.console && options.file_path.empty())
        throw std::invalid_argument("logger requires at least one sink");
    if (!options.file_path.empty() && options.max_file_size == 0)
        throw std::invalid_argument("log file size must be positive");
    std::vector<spdlog::sink_ptr> sinks;
    if (options.console) sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    if (!options.file_path.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            options.file_path, options.max_file_size, options.rotated_files));
    }
    auto candidate = std::make_shared<spdlog::logger>("rkmon", sinks.begin(), sinks.end());
    candidate->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [tid:%t] %v");
    candidate->set_level(options.level);
    candidate->flush_on(spdlog::level::warn);
    logger = std::move(candidate);
}

std::shared_ptr<spdlog::logger> get() {
    std::lock_guard lock(logger_mutex);
    if (!logger) throw std::logic_error("rkmon logger is not initialized");
    return logger;
}

void shutdown() noexcept {
    std::shared_ptr<spdlog::logger> previous;
    {
        std::lock_guard lock(logger_mutex);
        previous = std::move(logger);
    }
    // 不持锁执行输出，不调用 spdlog 全局 shutdown，以免影响第三方自己的 logger。
    try { if (previous) previous->flush(); }
    catch (...) { std::fputs("rkmon: log flush failed during shutdown\n", stderr); }
}
} // namespace rkmon::log

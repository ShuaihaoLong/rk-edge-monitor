#pragma once

#include <spdlog/spdlog.h>
#include <cstddef>
#include <memory>
#include <string>

namespace rkmon::log {
struct Options {
    std::string file_path{"logs/rkmon.log"}; // 空字符串禁用文件输出
    spdlog::level::level_enum level{spdlog::level::info};
    bool console{true};
    std::size_t max_file_size{5 * 1024 * 1024};
    std::size_t rotated_files{3}; // 备份文件数，另有一个当前日志文件
};

// 在业务线程启动前初始化；失败抛异常，不留下部分初始化的全局 logger。
void init(const Options& options = {});
// 未初始化时抛 logic_error；可在模块初始化时缓存共享指针。
std::shared_ptr<spdlog::logger> get();
// 所有业务线程停止之后调用，刷新并释放项目持有的 logger；重复调用安全。
void shutdown() noexcept;
} // namespace rkmon::log

#define RKMON_INFO(...) SPDLOG_LOGGER_INFO(::rkmon::log::get(), __VA_ARGS__)
#define RKMON_WARN(...) SPDLOG_LOGGER_WARN(::rkmon::log::get(), __VA_ARGS__)
#define RKMON_ERROR(...) SPDLOG_LOGGER_ERROR(::rkmon::log::get(), __VA_ARGS__)

#pragma once

#include "core/service.hpp"
#include <memory>
#include <string>
#include <vector>

namespace rkmon::core {

// 控制线程独占使用。按依赖顺序 add：先依赖，再使用依赖的服务。
class ServiceManager {
public:
    ServiceManager() = default;
    ~ServiceManager();
    ServiceManager(const ServiceManager&) = delete;
    ServiceManager& operator=(const ServiceManager&) = delete;

    void add(std::unique_ptr<IService> service);
    bool start_all();
    void stop_all() noexcept;
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }
    [[nodiscard]] std::vector<HealthSnapshot> health() const;

private:
    std::vector<std::unique_ptr<IService>> services_;
    std::size_t attempted_{0};
    bool started_{false};
    std::string last_error_;
};

} // namespace rkmon::core

#pragma once

#include <string>
#include <string_view>

namespace rkmon::core {

enum class ServiceState { stopped, starting, running, degraded, stopping, failed };
struct HealthSnapshot {
    ServiceState state{ServiceState::stopped};
    std::string detail;
};

class IService {
public:
    virtual ~IService() = default;
    // 启停只由控制线程串行调用；服务内部拥有线程和硬件资源。
    virtual bool start() = 0;
    // 必须可重复调用，且适用于启动到一半失败的服务；唤醒阻塞任务，不等待线程。
    virtual void request_stop() noexcept = 0;
    // 等待线程/库任务结束，不能抛异常。只能在控制线程调用，不能 join 自己。
    virtual void join() noexcept = 0;
    virtual void stop() noexcept { request_stop(); join(); }
    [[nodiscard]] virtual bool running() const noexcept = 0;
    // 名字在服务生命周期内必须稳定。
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    // 有 degraded/failed 状态的服务需覆盖此接口并提供线程安全快照。
    [[nodiscard]] virtual HealthSnapshot health() const {
        return {running() ? ServiceState::running : ServiceState::stopped, {}};
    }
};

} // namespace rkmon::core

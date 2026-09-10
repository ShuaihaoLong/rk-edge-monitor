#include "core/service_manager.hpp"
#include <exception>
#include <stdexcept>

namespace rkmon::core {

ServiceManager::~ServiceManager() { stop_all(); }

void ServiceManager::add(std::unique_ptr<IService> service) {
    if (started_ || attempted_ != 0) throw std::logic_error("cannot add a running service");
    if (!service || service->name().empty()) throw std::invalid_argument("service needs a name");
    for (const auto& existing : services_) {
        if (existing->name() == service->name()) throw std::invalid_argument("duplicate service name");
    }
    services_.push_back(std::move(service));
}

bool ServiceManager::start_all() {
    if (started_) return true;
    last_error_.clear();
    for (std::size_t i = 0; i < services_.size(); ++i) {
        // 失败模块也可能已经创建线程，回滚必须包含它。
        attempted_ = i + 1;
        try {
            if (services_[i]->start()) continue;
            const auto snapshot = services_[i]->health();
            last_error_ = std::string(services_[i]->name()) + ": " +
                (snapshot.detail.empty() ? "start returned false" : snapshot.detail);
        } catch (const std::exception& error) {
            // 先停止所有已尝试模块，异常文本构造失败也不留下运行中的线程。
            stop_all();
            last_error_ = std::string(services_[i]->name()) + ": " + error.what();
            return false;
        } catch (...) {
            stop_all();
            last_error_ = std::string(services_[i]->name()) + ": unknown startup exception";
            return false;
        }
        stop_all();
        return false;
    }
    started_ = true;
    return true;
}

void ServiceManager::stop_all() noexcept {
    // 两阶段停止：所有模块先得到通知，再逆序等待，避免等待尚未停止的上游。
    for (std::size_t i = attempted_; i > 0; --i) services_[i - 1]->request_stop();
    for (std::size_t i = attempted_; i > 0; --i) services_[i - 1]->join();
    attempted_ = 0;
    started_ = false;
}

std::vector<HealthSnapshot> ServiceManager::health() const {
    std::vector<HealthSnapshot> result;
    result.reserve(services_.size());
    for (const auto& service : services_) result.push_back(service->health());
    return result;
}

} // namespace rkmon::core

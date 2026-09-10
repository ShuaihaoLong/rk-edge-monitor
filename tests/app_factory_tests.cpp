#include "app/application.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class Service final : public rkmon::core::IService {
public:
    Service(std::string name, std::vector<std::string>& events, bool fail,
            rkmon::app::Application::FaultReporter report = {})
        : name_(std::move(name)), events_(events), fail_(fail), report_(std::move(report)) {}
    bool start() override {
        events_.push_back(name_ + ":start");
        if (report_) { report_(name_, "first fault"); report_(name_, "second fault"); }
        running_ = !fail_;
        return running_;
    }
    void request_stop() noexcept override { events_.push_back(name_ + ":stop"); running_ = false; }
    void join() noexcept override { events_.push_back(name_ + ":join"); }
    bool running() const noexcept override { return running_; }
    std::string_view name() const noexcept override { return name_; }
    rkmon::core::HealthSnapshot health() const override {
        return {fail_ ? rkmon::core::ServiceState::failed : rkmon::core::ServiceState::running,
                fail_ ? "specific startup error" : ""};
    }
private:
    std::string name_;
    std::vector<std::string>& events_;
    bool fail_, running_{false};
    rkmon::app::Application::FaultReporter report_;
};
}
int main() {
    try {
        using App = rkmon::app::Application;
        App::Options options;
        options.log.file_path.clear();
        options.control_mailbox_capacity = 1;
        std::vector<std::string> events;
        {
            App app(options, [&](App::FaultReporter, std::shared_ptr<spdlog::logger>) {
                App::ServiceList services;
                services.push_back(std::make_unique<Service>("first", events, false));
                services.push_back(std::make_unique<Service>("second", events, true));
                return services;
            });
            check(app.run() == 1, "failed module did not fail the application");
            check(events == std::vector<std::string>{"first:start", "second:start", "second:stop", "first:stop",
                                                     "second:join", "first:join"}, "startup rollback order wrong");
        }
        events.clear();
        {
            App app(options, [&](App::FaultReporter report, std::shared_ptr<spdlog::logger>) {
                App::ServiceList services;
                services.push_back(std::make_unique<Service>("faulting", events, false, std::move(report)));
                return services;
            });
            check(app.run() == 1, "full control mailbox suppressed fatal fault");
            check(events == std::vector<std::string>{"faulting:start", "faulting:stop", "faulting:join"},
                  "fault did not clean up service");
        }
        {
            bool called = false;
            App app(options, [&](App::FaultReporter, std::shared_ptr<spdlog::logger>) {
                called = true; return App::ServiceList{};
            });
            app.request_stop();
            check(app.run() == 0 && !called, "stop before run still constructed modules");
        }
        std::cout << "app factory tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

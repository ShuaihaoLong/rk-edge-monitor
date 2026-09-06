#include "core/logger.hpp"
#include "core/service_manager.hpp"
#include <exception>
#include <iostream>

int main() {
    try {
        rkmon::log::Options options;
        options.file_path.clear(); // 当前仅验证中间层启动，真实应用再配置文件位置。
        rkmon::log::init(options);
        {
            rkmon::core::ServiceManager services;
            // VideoService 等实际模块完成后，在这里按依赖顺序注册。
            if (!services.start_all()) {
                RKMON_ERROR("[app] startup failed: {}", services.last_error());
                rkmon::log::shutdown();
                return 1;
            }
            RKMON_INFO("[app] middleware bootstrap complete; no video service registered yet");
            services.stop_all();
        }
        rkmon::log::shutdown();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "rkmon: " << error.what() << '\n';
        rkmon::log::shutdown();
        return 1;
    }
}

#include "display/telemetry.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using rkmon::display::SensorState;
void require(bool condition) { if (!condition) throw std::runtime_error("sensor state check failed"); }
std::string sample(double temperature = 25.3, long offset = 0) {
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "{\"schema\":1,\"temperature_c\":" + std::to_string(temperature) +
           ",\"humidity_percent\":60.2,\"received_at_ms\":" + std::to_string(now + offset) + "}";
}
int main() {
    SensorState s(30);
    s.accept("status", "{\"schema\":1,\"online\":true}", true);
    s.accept("telemetry", sample(), true); require(!s.snapshot().online);
    s.accept("telemetry", sample(99), false); require(!s.snapshot().online);
    s.accept("telemetry", sample(25, -1000), false); require(!s.snapshot().online);
    s.accept("telemetry", "{\"schema\":1,\"temperature_c\":\"25\"}", false); require(!s.snapshot().online);
    s.accept("telemetry", sample(), false); require(s.snapshot().online);
    require(s.snapshot().temperature == 25.3 && s.snapshot().humidity == 60.2);
    std::this_thread::sleep_for(std::chrono::milliseconds(40)); require(!s.snapshot().online);
    s.accept("telemetry", sample(), false); require(s.snapshot().online);
    s.accept("status", "{\"schema\":1,\"online\":false}", false); require(!s.snapshot().online);
    s.accept("status", "{\"schema\":1,\"online\":true}", true); require(!s.snapshot().online);
    s.accept("telemetry", sample(), false); require(s.snapshot().online);
    s.disconnected(); require(!s.snapshot().online);
    std::cout << "display data tests passed\n";
}

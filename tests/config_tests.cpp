#include "app/config.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class Fixture {
public:
    Fixture() {
        char pattern[] = "/tmp/rkmon-ini-tests.XXXXXX";
        const char* result = mkdtemp(pattern);
        if (!result) throw std::runtime_error("mkdtemp failed");
        dir = result;
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(dir, ignored); }
    rkmon::app::RuntimeConfig read(const std::string& contents) {
        std::ofstream file(dir / "test.ini"); file << contents; file.close();
        return rkmon::app::load_config(dir / "test.ini");
    }
    std::filesystem::path dir;
};
}
int main() {
    try {
        Fixture fixture;
        auto config = fixture.read("\xef\xbb\xbf; comment\r\n[app]\r\ncontrol_mailbox_capacity=8\r\n"
            "[logging]\nconsole=false\nfile=logs/a#b.log\nlevel=debug\n[camera]\nenabled=true\n"
            "device=relative-camera\nwidth=640\nheight=480\nformat=YUYV\nfps=30\nqueue_capacity=2\n");
        check(config.application.control_mailbox_capacity == 8, "app settings not loaded");
        check(config.application.log.file_path == (fixture.dir / "logs/a#b.log").string(), "log path resolution wrong");
        check(config.camera && config.camera->capture.device == (fixture.dir / "relative-camera").string(), "device path wrong");
        check(config.camera->queue_capacity == 2 && config.camera->capture.format == rkmon::camera::PixelFormat::YUYV,
              "camera configuration wrong");
        check(!fixture.read("[camera]\nenabled=false\n").camera, "disabled camera instantiated");
        for (const std::string text : {
            "[app]\nunknown=1\n", "[unknown]\n", "[camera\n", "enabled=true\n",
            "[camera]\nenabled=yes\n", "[camera]\nenabled=true\n", "[camera]\nfps=30oops\n",
            "[camera]\nfps=-1\n", "[camera]\nwidth=999999999999999999\n", "[camera]\nqueue_capacity=0\n",
            "[camera]\nbuffer_count=1\n", "[camera]\nformat=NV12\n", "[camera]\nenabled=false\nenabled=true\n",
            "[camera]\n[camera]\n", "[logging]\nlevel=oops\n", "[logging]\nconsole=false\nfile=\n"}) {
            bool rejected = false;
            try { fixture.read(text); }
            catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("test.ini") != std::string::npos;
            }
            check(rejected, "invalid configuration was accepted or lacked file context");
        }
        bool missing = false;
        try { rkmon::app::load_config(fixture.dir / "missing.ini"); }
        catch (const std::runtime_error&) { missing = true; }
        check(missing, "missing configuration was accepted");
        std::cout << "config tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

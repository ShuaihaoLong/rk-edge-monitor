#include "app/application.hpp"

#include <exception>
#include <iostream>

int main() {
    try {
        rkmon::app::Application application;
        return application.run();
    } catch (const std::exception& error) {
        std::cerr << "rkmon: " << error.what() << '\n';
        return 1;
    }
}

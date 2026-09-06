cmake_minimum_required(VERSION 3.16)
project(gst_smoke LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH FALSE)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0)

add_executable(gst-smoke main.cpp)
target_compile_features(gst-smoke PRIVATE cxx_std_17)
target_link_libraries(gst-smoke PRIVATE PkgConfig::GST)

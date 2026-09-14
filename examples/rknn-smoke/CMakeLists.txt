cmake_minimum_required(VERSION 3.16)
project(rkmon_rknn_smoke LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(RKNN_ZOO "${CMAKE_CURRENT_SOURCE_DIR}/../../.local/ai-stage4/zoo" CACHE PATH "Pinned RKNN Model Zoo sources")
find_library(RKNN_RUNTIME rknnrt REQUIRED)
add_executable(rkmon_rknn_smoke main.cpp "${RKNN_ZOO}/examples/yolov8/cpp/postprocess.cc")
target_include_directories(rkmon_rknn_smoke PRIVATE
    "${RKNN_ZOO}/examples/yolov8/cpp" "${RKNN_ZOO}/utils"
    "${RKNN_ZOO}/3rdparty/rknpu2/include" "${RKNN_ZOO}/3rdparty/stb_image")
target_link_libraries(rkmon_rknn_smoke PRIVATE "${RKNN_RUNTIME}")

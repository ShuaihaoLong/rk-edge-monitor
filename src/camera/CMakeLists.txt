add_library(rkmon_camera STATIC
    v4l2_video_source.cpp
    video_capture_service.cpp
)
add_library(rkmon::camera ALIAS rkmon_camera)
target_include_directories(rkmon_camera PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_options(rkmon_camera PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rkmon_camera PUBLIC rkmon::core)

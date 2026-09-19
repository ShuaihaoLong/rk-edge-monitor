set(_gst_default OFF)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    set(_gst_default ON)
endif()
option(RKMON_WITH_GSTREAMER "Build Rockchip GStreamer hardware decoder and publisher" ${_gst_default})

add_library(rkmon_frame STATIC dma_buffer.cpp)
add_library(rkmon::frame ALIAS rkmon_frame)
target_include_directories(rkmon_frame PUBLIC "${PROJECT_SOURCE_DIR}/include")
option(RKMON_WITH_RGA "Enable RGA AI preprocessing" ${_gst_default})
if(RKMON_WITH_RGA)
    find_path(RGA_INCLUDE_DIR im2d.h PATH_SUFFIXES rga REQUIRED)
    find_library(RGA_LIBRARY rga REQUIRED)
    target_sources(rkmon_frame PRIVATE rga_copy.cpp)
    target_include_directories(rkmon_frame SYSTEM PRIVATE "${RGA_INCLUDE_DIR}")
    target_link_libraries(rkmon_frame PRIVATE "${RGA_LIBRARY}")
endif()
add_library(rkmon_video STATIC services/video_process_service.cpp factory.cpp osd.cpp
    services/video_stream_service.cpp)
add_library(rkmon::video ALIAS rkmon_video)
target_include_directories(rkmon_video
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_options(rkmon_video PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rkmon_video PUBLIC rkmon::core rkmon::frame)
if(RKMON_WITH_GSTREAMER)
    find_package(PkgConfig REQUIRED)
    set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH FALSE)
    pkg_check_modules(GST REQUIRED IMPORTED_TARGET GLOBAL
        gstreamer-allocators-1.0>=1.20 gstreamer-1.0>=1.20 gstreamer-app-1.0>=1.20 gstreamer-video-1.0>=1.20)
    target_sources(rkmon_video PRIVATE gstreamer/gst_video_pipeline.cpp
        gstreamer/gst_rtsp_publisher.cpp)
    target_compile_definitions(rkmon_video PRIVATE RKMON_WITH_GSTREAMER)
    target_link_libraries(rkmon_video PRIVATE PkgConfig::GST)
endif()

if(RKMON_WITH_RGA)
    target_compile_definitions(rkmon_video PRIVATE RKMON_WITH_RGA)
endif()

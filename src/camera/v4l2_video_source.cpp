#include "v4l2_video_source.hpp"

#include <linux/videodev2.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace rkmon::camera {

namespace {

int xioctl(int fd, unsigned long request, void* arg) {
    int result;
    do {
        result = ::ioctl(fd, request, arg);
    } while (result < 0 && errno == EINTR);
    return result;
}

void checked_ioctl(int fd, unsigned long request, void* arg, const char* operation) {
    if (xioctl(fd, request, arg) < 0) {
        throw std::system_error(errno, std::generic_category(), operation);
    }
}

std::uint32_t fourcc(PixelFormat format) {
    switch (format) {
    case PixelFormat::MJPG:
        return V4L2_PIX_FMT_MJPEG;
    case PixelFormat::YUYV:
        return V4L2_PIX_FMT_YUYV;
    case PixelFormat::NV12:
        return V4L2_PIX_FMT_NV12;
    case PixelFormat::RGB888:
        return V4L2_PIX_FMT_RGB24;
    case PixelFormat::BGR888:
        return V4L2_PIX_FMT_BGR24;
    }
    throw std::invalid_argument("unsupported pixel format");
}

PixelFormat pixel_format(std::uint32_t value) {
    for (auto f : {PixelFormat::MJPG, PixelFormat::YUYV, PixelFormat::NV12,
                   PixelFormat::RGB888, PixelFormat::BGR888}) {
        if (fourcc(f) == value) {
            return f;
        }
    }
    throw std::runtime_error("driver negotiated an unsupported pixel format");
}

} // namespace

V4L2VideoSource::V4L2VideoSource(CaptureConfig config) : config_(std::move(config)) {
    if (config_.device.empty() || config_.width <= 0 || config_.height <= 0 ||
        config_.fps == 0 || config_.buffer_count < 2 || config_.buffer_count > 64 ||
        config_.poll_timeout_ms <= 0) {
        throw std::invalid_argument("invalid capture configuration");
    }
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }
}

V4L2VideoSource::~V4L2VideoSource() {
    close();
    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
    }
}

void V4L2VideoSource::open() {
    if (fd_ >= 0) {
        throw std::logic_error("camera already open");
    }
    // 清除上次停止留下的唤醒计数，允许关闭后重新打开。
    std::uint64_t pending;
    while (::read(wake_fd_, &pending, sizeof(pending)) > 0) {}
    stop_ = false;
    actual_ = {};
    try {
        fd_ = ::open(config_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            throw std::system_error(errno, std::generic_category(), config_.device);
        }
        query_capabilities();
        configure_format();
        configure_frame_interval();
        allocate_buffers();
        // 所有缓冲区先入队，再启动采集。
        auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        checked_ioctl(fd_, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
        streaming_ = true;
    } catch (...) {
        close();
        throw;
    }
}

void V4L2VideoSource::query_capabilities() {
    v4l2_capability capability{};
    checked_ioctl(fd_, VIDIOC_QUERYCAP, &capability, "VIDIOC_QUERYCAP");
    const auto caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
        ? capability.device_caps : capability.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        throw std::runtime_error("device is not a single-planar streaming capture node");
    }
}

void V4L2VideoSource::configure_format() {
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = static_cast<unsigned>(config_.width);
    format.fmt.pix.height = static_cast<unsigned>(config_.height);
    format.fmt.pix.pixelformat = fourcc(config_.format);
    format.fmt.pix.field = V4L2_FIELD_ANY;
    checked_ioctl(fd_, VIDIOC_S_FMT, &format, "VIDIOC_S_FMT");
    // S_FMT 会回写协商值，不能直接把请求配置当作实际输出。
    actual_.width = static_cast<int>(format.fmt.pix.width);
    actual_.height = static_cast<int>(format.fmt.pix.height);
    actual_.format = pixel_format(format.fmt.pix.pixelformat);
    actual_.stride = format.fmt.pix.bytesperline;
    actual_.size_image = format.fmt.pix.sizeimage;
}

void V4L2VideoSource::configure_frame_interval() {
    v4l2_streamparm parameters{};
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    checked_ioctl(fd_, VIDIOC_G_PARM, &parameters, "VIDIOC_G_PARM");
    if (parameters.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) {
        parameters.parm.capture.timeperframe = {1, config_.fps};
        checked_ioctl(fd_, VIDIOC_S_PARM, &parameters, "VIDIOC_S_PARM");
    }
    actual_.interval_numerator = parameters.parm.capture.timeperframe.numerator;
    actual_.interval_denominator = parameters.parm.capture.timeperframe.denominator;
}

void V4L2VideoSource::allocate_buffers() {
    v4l2_requestbuffers buffers{};
    buffers.count = config_.buffer_count;
    buffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffers.memory = V4L2_MEMORY_MMAP;
    checked_ioctl(fd_, VIDIOC_REQBUFS, &buffers, "VIDIOC_REQBUFS");
    if (buffers.count < 2) {
        throw std::runtime_error("driver allocated fewer than two buffers");
    }
    // REQBUFS 返回实际数量；每个缓冲区只在初始化时映射一次。
    mappings_.reserve(buffers.count);
    for (unsigned i = 0; i < buffers.count; ++i) {
        v4l2_buffer buffer{};
        buffer.type = buffers.type;
        buffer.memory = buffers.memory;
        buffer.index = i;
        checked_ioctl(fd_, VIDIOC_QUERYBUF, &buffer, "VIDIOC_QUERYBUF");
        void* address = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd_, buffer.m.offset);
        if (address == MAP_FAILED) {
            throw std::system_error(errno, std::generic_category(), "mmap");
        }
        mappings_.push_back({address, buffer.length});
        // 映射只建立访问地址，QBUF 才把缓冲区交给驱动采集。
        checked_ioctl(fd_, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF");
    }
}

ReadResult V4L2VideoSource::read() {
    if (stop_) {
        return {ReadStatus::stopped, {}, {}};
    }
    if (!streaming_) {
        return {ReadStatus::error, {}, "camera is not streaming"};
    }
    try {
        // 重试共用一个截止时间，信号中断和坏帧不会延长本次读取超时。
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(config_.poll_timeout_ms);
        for (;;) {
            if (stop_) {
                return {ReadStatus::stopped, {}, {}};
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) {
                return {ReadStatus::timeout, {}, {}};
            }
            // 同时等待相机就绪与停止通知，避免停止时等待整个超时周期。
            pollfd fds[] = {{fd_, POLLIN, 0}, {wake_fd_, POLLIN, 0}};
            const int ready = ::poll(fds, 2, static_cast<int>(remaining));
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready < 0) {
                throw std::system_error(errno, std::generic_category(), "poll");
            }
            if (stop_ || (fds[1].revents & POLLIN)) {
                return {ReadStatus::stopped, {}, {}};
            }
            if (ready == 0) {
                return {ReadStatus::timeout, {}, {}};
            }
            if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                throw std::runtime_error("camera disconnected or poll failed");
            }
            if (!(fds[0].revents & POLLIN)) {
                continue;
            }
            // DQBUF 返回描述信息，图像字节位于 index 对应的映射地址。
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
                if (errno == EAGAIN) {
                    continue;
                }
                throw std::system_error(errno, std::generic_category(), "VIDIOC_DQBUF");
            }
            const auto received_monotonic = std::chrono::steady_clock::now();
            const auto received_at = std::chrono::system_clock::now();
            if (buffer.index >= mappings_.size()) {
                throw std::runtime_error("invalid driver buffer index");
            }
            VideoFrame frame;
            try {
                if (buffer.bytesused > mappings_[buffer.index].length) {
                    throw std::runtime_error("driver bytesused exceeds mapped buffer");
                }
                if (!(buffer.flags & V4L2_BUF_FLAG_ERROR) && buffer.bytesused != 0) {
                    // 复制后即可归还驱动缓冲区，消费者持有的帧不会被后续采集覆盖。
                    auto data = std::shared_ptr<std::uint8_t[]>(new std::uint8_t[buffer.bytesused]);
                    std::memcpy(data.get(), mappings_[buffer.index].address, buffer.bytesused);
                    frame.sequence = buffer.sequence;
                    frame.width = actual_.width;
                    frame.height = actual_.height;
                    frame.format = actual_.format;
                    frame.stride = actual_.stride;
                    frame.timestamp = received_monotonic;
                    frame.received_at = received_at;
                    frame.data = std::move(data);
                    frame.size = buffer.bytesused;
                }
            } catch (...) {
                // 复制或构造帧失败时也尽力归还缓冲区，再向上报告错误。
                xioctl(fd_, VIDIOC_QBUF, &buffer);
                throw;
            }
            // 包括空帧和驱动标记的坏帧，都必须归还缓冲区。
            checked_ioctl(fd_, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF");
            if (frame.size != 0) {
                return {ReadStatus::frame, std::move(frame), {}};
            }
        }
    } catch (const std::exception& error) {
        return {ReadStatus::error, {}, error.what()};
    }
}

void V4L2VideoSource::request_stop() noexcept {
    stop_ = true;
    // 写入 eventfd 唤醒 poll；唤醒句柄保留到对象析构，避免与停止请求竞争。
    const std::uint64_t one = 1;
    ssize_t result;
    do {
        result = ::write(wake_fd_, &one, sizeof(one));
    } while (result < 0 && errno == EINTR);
}

void V4L2VideoSource::close() noexcept {
    // 调用方须先结束读取线程，再按停流、解除映射、关闭设备的顺序释放资源。
    if (streaming_) {
        auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }
    for (const auto& mapping : mappings_) {
        ::munmap(mapping.address, mapping.length);
    }
    mappings_.clear();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace rkmon::camera

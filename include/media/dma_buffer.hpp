#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
namespace rkmon::media {
// fd 由 owner 保活；外部导入时 owner 必须保有原缓冲区，不能只 dup fd 后归还池。
struct DmaBuffer {
    int fd{-1};
    std::size_t size{};
    std::shared_ptr<void> owner;
};
std::shared_ptr<DmaBuffer> allocate_dma_buffer(std::size_t size);
// CPU 访问成对执行 DMA_BUF_IOCTL_SYNC，设备任务必须在映射前同步完成。
class DmaMapping {
public:
    explicit DmaMapping(std::shared_ptr<const DmaBuffer> buffer, bool write=false);
    ~DmaMapping();
    DmaMapping(const DmaMapping&)=delete;
    DmaMapping& operator=(const DmaMapping&)=delete;
    std::uint8_t* data() const noexcept {return data_;}
    void finish();
private:
    std::shared_ptr<const DmaBuffer> buffer_;
    std::uint8_t* data_{};
    bool write_{},active_{};
};
}

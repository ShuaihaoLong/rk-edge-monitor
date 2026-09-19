#include "media/dma_buffer.hpp"
#include <cerrno>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdexcept>
#include <system_error>
namespace rkmon::media {
namespace {
int sync(int fd, unsigned long long flags) {
    dma_buf_sync request{flags};int result;
    do {result=ioctl(fd,DMA_BUF_IOCTL_SYNC,&request);}while(result<0 && errno==EINTR);
    return result;
}
struct OwnedFd {int fd{-1};~OwnedFd(){if(fd>=0)::close(fd);}};
}
std::shared_ptr<DmaBuffer> allocate_dma_buffer(std::size_t size) {
    if(!size)throw std::invalid_argument("empty DMA allocation");
    // DMA32 兼容 RGA2/3 地址限制；不回退到普通内存冒充共享路径。
    const int heap=open("/dev/dma_heap/system-uncached-dma32",O_RDWR|O_CLOEXEC);
    if(heap<0)throw std::system_error(errno,std::generic_category(),"open DMA heap");
    dma_heap_allocation_data request{};request.len=size;request.fd_flags=O_RDWR|O_CLOEXEC;
    const int result=ioctl(heap,DMA_HEAP_IOCTL_ALLOC,&request),error=errno;::close(heap);
    if(result<0)throw std::system_error(error,std::generic_category(),"allocate DMA buffer");
    std::shared_ptr<OwnedFd> owner;
    try {owner=std::make_shared<OwnedFd>();}catch(...){::close(request.fd);throw;}
    owner->fd=static_cast<int>(request.fd);
    return std::make_shared<DmaBuffer>(DmaBuffer{owner->fd,size,owner});
}
DmaMapping::DmaMapping(std::shared_ptr<const DmaBuffer> buffer,bool write):buffer_(std::move(buffer)),write_(write) {
    if(!buffer_ || buffer_->fd<0 || !buffer_->size)throw std::invalid_argument("invalid DMA buffer");
    void* ptr=mmap(nullptr,buffer_->size,PROT_READ|(write?PROT_WRITE:0),MAP_SHARED,buffer_->fd,0);
    if(ptr==MAP_FAILED)throw std::system_error(errno,std::generic_category(),"map DMA buffer");
    data_=static_cast<std::uint8_t*>(ptr);
    if(sync(buffer_->fd,DMA_BUF_SYNC_START|(write_?DMA_BUF_SYNC_RW:DMA_BUF_SYNC_READ))<0) {
        const int error=errno;munmap(data_,buffer_->size);data_=nullptr;
        throw std::system_error(error,std::generic_category(),"begin DMA CPU access");
    }
    active_=true;
}
void DmaMapping::finish() {
    if(active_) {
        if(sync(buffer_->fd,DMA_BUF_SYNC_END|(write_?DMA_BUF_SYNC_RW:DMA_BUF_SYNC_READ))<0)
            throw std::system_error(errno,std::generic_category(),"end DMA CPU access");
        active_=false;
    }
}
DmaMapping::~DmaMapping(){
    if(active_)sync(buffer_->fd,DMA_BUF_SYNC_END|(write_?DMA_BUF_SYNC_RW:DMA_BUF_SYNC_READ));
    if(data_)munmap(data_,buffer_->size);
}
}

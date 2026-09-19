#include "result_writer.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
namespace rkmon::ai {
namespace {
std::string quote(const std::string& text) {
    std::ostringstream out;out << '"';
    for(unsigned char c:text) {
        if(c=='"' || c=='\\')out << '\\' << c;
        else if(c<32)out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << c;
    }
    out << '"';return out.str();
}
}
ResultWriter::ResultWriter(std::string path,std::string event_socket):event_socket_(std::move(event_socket)),path_(std::move(path)) {
    if(event_socket_.size()>=sizeof(sockaddr_un::sun_path))throw std::invalid_argument("event socket path too long");
    if(!event_socket_.empty())event_fd_=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    session_=std::to_string(getpid())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}
ResultWriter::~ResultWriter(){if(event_fd_>=0)::close(event_fd_);}
void ResultWriter::write(const std::string& status,const DetectionResult* r) {
    const auto path=std::filesystem::path(path_);
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path_+".tmp",std::ios::trunc);
    if(!file)throw std::runtime_error("cannot write AI result snapshot");
    std::ostringstream message;
    message << "{\"session\":" << quote(session_) << ",\"revision\":" << ++revision_
         << ",\"status\":" << quote(status) << ",\"sequence\":" << (r?r->sequence:0)
         << ",\"received_at_ms\":" << (r?std::chrono::duration_cast<std::chrono::milliseconds>(r->received_at.time_since_epoch()).count():0)
         << ",\"source_generation\":" << (r?r->source_generation:0)
         << ",\"worker_index\":" << (r?static_cast<int>(r->worker_index):-1)
         << ",\"source_dma\":" << (r && r->source_dma?"true":"false")
         << ",\"input_dma\":" << (r && r->input_dma?"true":"false")
         << ",\"width\":" << (r?r->width:0) << ",\"height\":" << (r?r->height:0)
         << ",\"age_ms\":" << (r?std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-r->source_time).count():0)
         << ",\"inference_ms\":" << (r?r->inference_ms:0)
         << ",\"preprocess_ms\":" << (r?r->preprocess_ms:0)
         << ",\"input_ms\":" << (r?r->input_ms:0)
         << ",\"npu_ms\":" << (r?r->npu_ms:0)
         << ",\"postprocess_ms\":" << (r?r->postprocess_ms:0) << ",\"objects\":[";
    if(r)for(std::size_t i=0;i<r->objects.size();++i) {
        const auto& d=r->objects[i];if(i)message << ',';
        message << "{\"class_id\":" << d.class_id << ",\"label\":" << quote(d.label)
             << ",\"confidence\":" << d.confidence << ",\"box\":[" << d.left << ',' << d.top << ',' << d.right << ',' << d.bottom << "]}";
    }
    message << "]}\n";
    const auto payload=message.str();file << payload;file.close();
    if(!file)throw std::runtime_error("AI snapshot write failed");
    std::filesystem::rename(path_+".tmp",path_);
    // 索引服务停止或队列满时不能阻塞视频；事件是尽力传递，录像文件另有目录恢复。
    if(event_fd_>=0 && r && status=="ok") {
        sockaddr_un address{};address.sun_family=AF_UNIX;
        std::memcpy(address.sun_path,event_socket_.c_str(),event_socket_.size()+1);
        (void)sendto(event_fd_,payload.data(),payload.size(),MSG_DONTWAIT|MSG_NOSIGNAL,
                     reinterpret_cast<const sockaddr*>(&address),sizeof(address));
    }
}
}

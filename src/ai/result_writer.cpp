#include "result_writer.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
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
ResultWriter::ResultWriter(std::string path):path_(std::move(path)) {
    session_=std::to_string(getpid())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}
void ResultWriter::write(const std::string& status,const DetectionResult* r) {
    const auto path=std::filesystem::path(path_);
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path_+".tmp",std::ios::trunc);
    if(!file)throw std::runtime_error("cannot write AI result snapshot");
    file << "{\"session\":" << quote(session_) << ",\"revision\":" << ++revision_
         << ",\"status\":" << quote(status) << ",\"sequence\":" << (r?r->sequence:0)
         << ",\"width\":" << (r?r->width:0) << ",\"height\":" << (r?r->height:0)
         << ",\"age_ms\":" << (r?std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-r->source_time).count():0)
         << ",\"inference_ms\":" << (r?r->inference_ms:0)
         << ",\"preprocess_ms\":" << (r?r->preprocess_ms:0)
         << ",\"input_ms\":" << (r?r->input_ms:0)
         << ",\"npu_ms\":" << (r?r->npu_ms:0)
         << ",\"postprocess_ms\":" << (r?r->postprocess_ms:0) << ",\"objects\":[";
    if(r)for(std::size_t i=0;i<r->objects.size();++i) {
        const auto& d=r->objects[i];if(i)file << ',';
        file << "{\"class_id\":" << d.class_id << ",\"label\":" << quote(d.label)
             << ",\"confidence\":" << d.confidence << ",\"box\":[" << d.left << ',' << d.top << ',' << d.right << ',' << d.bottom << "]}";
    }
    file << "]}\n";file.close();
    if(!file)throw std::runtime_error("AI snapshot write failed");
    std::filesystem::rename(path_+".tmp",path_);
}
}

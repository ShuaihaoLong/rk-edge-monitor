#include "ai/inference_service.hpp"
#include "ai/letterbox/nv12_letterbox.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace rkmon;
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F> void wait(F ready){
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    while(!ready()) {check(std::chrono::steady_clock::now()<end,"wait timeout");std::this_thread::sleep_for(std::chrono::milliseconds(5));}
}
std::string read(const std::filesystem::path& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
class Detector final:public ai::IObjectDetector {
public:
    std::atomic<bool> broken{true};
    std::atomic<unsigned> frames{0},opens{0},last{0};
    void open() override {++opens;if(broken)throw std::runtime_error("model unavailable");}
    ai::DetectionResult detect(const camera::VideoFrame& frame) override {
        ++frames;last=frame.sequence;
        ai::DetectionResult r;r.sequence=frame.sequence;r.source_time=frame.timestamp;r.width=4;r.height=2;
        r.objects.push_back({0,"quote\" newline\n",0.9f,0,0,4,2});return r;
    }
    void close() noexcept override {}
};
}
int main(){
    std::filesystem::path folder;
    try {
        char path[]="/tmp/rkmon-ai-test.XXXXXX";check(mkdtemp(path),"mkdtemp failed");folder=path;
        camera::VideoFrame frame;frame.width=4;frame.height=2;frame.stride=4;frame.size=12;frame.format=camera::PixelFormat::NV12;
        auto data=std::shared_ptr<std::uint8_t[]>(new std::uint8_t[12]);
        std::fill(data.get(),data.get()+8,16);std::fill(data.get()+8,data.get()+12,128);frame.data=data;
        auto image=ai::nv12_letterbox(frame,4);
        check(image.scale==1 && image.x_pad==0 && image.y_pad==1,"letterbox dimensions wrong");
        check(image.rgb[0]==114 && image.rgb[12]==0 && image.rgb[35]==0 && image.rgb[36]==114,"black or padding incorrect");
        std::fill(data.get(),data.get()+8,235);image=ai::nv12_letterbox(frame,4);
        check(image.rgb[12]==255 && image.rgb[13]==255 && image.rgb[14]==255,"white conversion incorrect");
        frame.size=11;bool rejected=false;try{ai::nv12_letterbox(frame);}catch(const std::invalid_argument&){rejected=true;}
        check(rejected,"invalid buffer accepted");frame.size=12;
        auto queue=std::make_shared<ai::InferenceService::Queue>(1);
        auto fake=std::make_unique<Detector>();auto* view=fake.get();
        ai::InferenceConfig config;config.result_path=(folder/"detections.json").string();config.fps=10;
        ai::InferenceService service(std::move(fake),queue,config);
        check(service.start(),"start failed");
        wait([&]{return read(config.result_path).find("unavailable")!=std::string::npos;});
        check(service.running(),"AI fault killed service");view->broken=false;
        wait([&]{return view->opens>=2;});
        for(unsigned seq=0;seq<20;++seq){frame.sequence=seq;frame.timestamp=std::chrono::steady_clock::now();queue->try_push(frame,core::OverflowPolicy::drop_oldest);}
        wait([&]{return view->last==19;});
        wait([&]{return read(config.result_path).find("quote\\\" newline\\u000a")!=std::string::npos;});
        check(queue->dropped()>0,"latest-only queue did not drop");
        wait([&]{return read(config.result_path).find("waiting")!=std::string::npos;});
        auto start=std::chrono::steady_clock::now();service.request_stop();service.join();
        check(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(200),"idle stop blocked");
        check(!queue->closed(),"consumer closed shared queue");
        check(read(config.result_path).find("stopped")!=std::string::npos,"stale result left on stop");
        check(!std::filesystem::exists(config.result_path+".tmp"),"unfinished snapshot");
        std::filesystem::remove_all(folder);std::cout<<"AI service, preprocess and snapshot tests passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';if(!folder.empty())std::filesystem::remove_all(folder);return 1;}
}

#include "ai/rga_letterbox.hpp"
#include "ai/inference_service.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <unistd.h>
using namespace rkmon;
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
camera::VideoFrame frame(int width,int height,unsigned char value) {
    camera::VideoFrame f;f.width=width;f.height=height;f.stride=width;
    f.format=camera::PixelFormat::NV12;f.size=std::size_t(width)*height*3/2;
    auto bytes=std::shared_ptr<std::uint8_t[]>(new std::uint8_t[f.size]);
    std::fill(bytes.get(),bytes.get()+width*height,value);
    std::fill(bytes.get()+width*height,bytes.get()+f.size,128);
    f.data=bytes;f.timestamp=std::chrono::steady_clock::now();return f;
}
}
int main(int argc,char** argv) {
    std::filesystem::path folder;
    try {
        ai::RgaLetterbox rga;
        const std::uint8_t* storage=nullptr;
        for(const auto dims:{std::pair{1920,1080},std::pair{640,480},std::pair{640,640},std::pair{1920,1080}}) {
            for(unsigned char value:{16,235,16}) {
                auto f=frame(dims.first,dims.second,value);
                const auto& out=rga.process(f);
                check(!storage || storage==out.rgb.data(),"RGA output buffer was reallocated");storage=out.rgb.data();
                const auto geometry=ai::nv12_letterbox_geometry(f,640);
                for(int y=0;y<640;++y)for(int x=0;x<640;++x)for(int c=0;c<3;++c) {
                    const bool padding=x<geometry.x_pad || x>=geometry.x_pad+geometry.width ||
                                       y<geometry.y_pad || y>=geometry.y_pad+geometry.height;
                    const int expected=padding?114:(value==16?0:255);
                    check(std::abs(int(out.rgb[(y*640+x)*3+c])-expected)<=2,"incorrect RGB conversion or padding");
                }
            }
        }
        std::cout<<"RGA padding, RGB conversion, geometry changes and buffer reuse passed\n";
        if(argc==3) {
            char path[]="/tmp/rkmon-rga-test.XXXXXX";check(mkdtemp(path),"mkdtemp failed");folder=path;
            ai::InferenceConfig config;config.workers=3;config.core_policy="split";config.fps=30;
            config.model_path=argv[1];config.labels_path=argv[2];config.result_path=(folder/"detections.json").string();
            auto queue=std::make_shared<ai::InferenceService::Queue>(1);
            std::vector<std::unique_ptr<ai::IObjectDetector>> detectors;
            for(unsigned i=0;i<3;++i)detectors.push_back(ai::make_detector(config,i));
            ai::InferenceService service(std::move(detectors),queue,config);service.start();
            auto f=frame(1920,1080,16);std::set<unsigned> seen;
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);
            while(std::chrono::steady_clock::now()<end) {
                f.timestamp=std::chrono::steady_clock::now();++f.sequence;
                queue->try_push(f,core::OverflowPolicy::drop_oldest);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                std::ifstream file(config.result_path);std::string json{std::istreambuf_iterator<char>(file),{}};
                if(json.find("\"status\":\"ok\"")!=std::string::npos)
                    for(unsigned i=0;i<3;++i)if(json.find("\"worker_index\":"+std::to_string(i))!=std::string::npos)seen.insert(i);
            }
            service.stop();check(seen.size()==3,"not all three NPU workers produced valid results");
            std::filesystem::remove_all(folder);folder.clear();
            std::cout<<"Three-core NPU service produced results from workers 0, 1 and 2\n";
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';if(!folder.empty())std::filesystem::remove_all(folder);return 1;}
}

#include "media/gstreamer/gst_video_pipeline.hpp"
#include "media/gstreamer/gst_rtsp_publisher.hpp"
#include "ai/detector.hpp"
#include "media/nv12.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#include <cstring>
#include <cmath>
#include <stdexcept>
using namespace rkmon;
void check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
int main(int argc,char** argv) {
    try {
        if(argc!=5)throw std::runtime_error("Usage: dma_pipeline_tests JPEG_1920x1080 MODEL LABELS RTSP_URL");
        std::ifstream file(argv[1],std::ios::binary);
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
        check(!bytes.empty(),"missing JPEG fixture");
        camera::VideoFrame jpeg;jpeg.width=1920;jpeg.height=1080;jpeg.size=bytes.size();
        auto storage=std::shared_ptr<std::uint8_t[]>(new std::uint8_t[bytes.size()]);
        std::memcpy(storage.get(),bytes.data(),bytes.size());jpeg.data=storage;
        video::GstVideoPipeline decoder({2000,4});decoder.open();
        jpeg.timestamp=std::chrono::steady_clock::now();jpeg.received_at=std::chrono::system_clock::now();
        auto decoded=decoder.decode(jpeg);check(decoded && decoded->dma,"decoder DMA missing");
        ai::InferenceConfig config;config.model_path=argv[2];config.labels_path=argv[3];
        auto dma=ai::make_detector(config);dma->open();
        auto result=dma->detect(*decoded);check(result.source_dma && result.input_dma,"NPU DMA path missing");
        config.input_memory="copy";auto copy=ai::make_detector(config);copy->open();
        const auto reference=copy->detect(*decoded);
        check(reference.objects.size()==result.objects.size(),"DMA detection count differs from copy reference");
        for(std::size_t i=0;i<result.objects.size();++i) {
            const auto& a=result.objects[i];const auto& b=reference.objects[i];
            check(a.class_id==b.class_id && std::abs(a.confidence-b.confidence)<0.01 &&
                  std::abs(a.left-b.left)<=2 && std::abs(a.top-b.top)<=2 &&
                  std::abs(a.right-b.right)<=2 && std::abs(a.bottom-b.bottom)<=2,"DMA detections differ from copy reference");
        }
        std::cout<<"DMA/copy inference comparison passed; objects="<<result.objects.size()<<'\n';
        copy->close();dma->close();
        std::vector<std::uint8_t> retained;
        {media::DmaMapping map(decoded->dma);retained.assign(map.data(),map.data()+decoded->size);}
        for(bool osd:{true,false}) {
            video::StreamConfig stream;stream.url=argv[4];stream.osd_enabled=osd;
            video::GstRtspPublisher publisher(stream);publisher.open();
            for(unsigned i=0;i<120;++i) {
                jpeg.timestamp=std::chrono::steady_clock::now();jpeg.received_at=std::chrono::system_clock::now();jpeg.sequence=i;
                auto frame=decoder.decode(jpeg);check(bool(frame),"decode stopped unexpectedly");
                publisher.write(*frame);std::this_thread::sleep_for(std::chrono::milliseconds(33));publisher.check_health();
            }
            check(publisher.encoded_frames()>90,"DMA encoder stalled or excessively dropped frames");
            std::cout<<"DMA encoder osd="<<osd<<" encoded="<<publisher.encoded_frames()<<'\n';
            publisher.close();std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        decoder.close();
        media::DmaMapping map(decoded->dma);
        check(std::memcmp(map.data(),retained.data(),retained.size())==0,"shared decoded frame modified or recycled prematurely");
        std::cout<<"Retained DMA frame unchanged across decode, inference, OSD, encode and decoder shutdown\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

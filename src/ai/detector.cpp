#include "rknn_detector.hpp"
#include "ai/nv12_letterbox.hpp"
#include "yolov8.h"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
namespace rkmon::ai {
namespace {
void require(bool value,const char* reason) { if(!value) throw std::runtime_error(reason); }
struct OutputGuard {
    rknn_context context;
    std::vector<rknn_output> outputs{9};
    bool acquired{false};
    ~OutputGuard() { if(acquired) rknn_outputs_release(context,outputs.size(),outputs.data()); }
};
}
struct RknnDetector::Impl {
    InferenceConfig config;
    rknn_app_context_t app{};
    rknn_tensor_attr input{};
    std::vector<rknn_tensor_attr> attrs{9};
    std::vector<std::string> labels;
};
RknnDetector::RknnDetector(InferenceConfig c):impl_(std::make_unique<Impl>()) { impl_->config=std::move(c); }
RknnDetector::~RknnDetector() { close(); }
void RknnDetector::close() noexcept {
    if(impl_->app.rknn_ctx) rknn_destroy(impl_->app.rknn_ctx);
    impl_->app={}; impl_->labels.clear();
}
void RknnDetector::open() {
    close(); auto& s=*impl_;
    try {
        std::ifstream file(s.config.model_path,std::ios::binary);
        require(file.good(),"cannot open RKNN model");
        std::vector<char> model((std::istreambuf_iterator<char>(file)),{});
        require(!model.empty() && model.size()<256*1024*1024,"invalid model size");
        require(rknn_init(&s.app.rknn_ctx,model.data(),model.size(),0,nullptr)==0,"rknn_init failed");
        require(rknn_query(s.app.rknn_ctx,RKNN_QUERY_IN_OUT_NUM,&s.app.io_num,sizeof(s.app.io_num))==0,"query IO failed");
        require(s.app.io_num.n_input==1 && s.app.io_num.n_output==9,"YOLOv8 requires 1 input and 9 outputs");
        s.input={};
        require(rknn_query(s.app.rknn_ctx,RKNN_QUERY_INPUT_ATTR,&s.input,sizeof(s.input))==0,"query input failed");
        require(s.input.n_dims==4 && s.input.fmt==RKNN_TENSOR_NHWC && s.input.dims[0]==1 &&
                s.input.dims[1]==640 && s.input.dims[2]==640 && s.input.dims[3]==3,"expected NHWC 640x640 RGB");
        for(unsigned i=0;i<9;++i) {
            auto& a=s.attrs[i];a={};a.index=i;
            require(rknn_query(s.app.rknn_ctx,RKNN_QUERY_OUTPUT_ATTR,&a,sizeof(a))==0,"query output failed");
            const unsigned side=80u>>(i/3), channels=i%3==0?64:i%3==1?80:1;
            require(a.n_dims==4 && a.fmt==RKNN_TENSOR_NCHW && a.type==RKNN_TENSOR_INT8 &&
                a.qnt_type==RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC && a.dims[0]==1 &&
                a.dims[1]==channels && a.dims[2]==side && a.dims[3]==side,"model output incompatible with postprocess");
        }
        s.app.input_attrs=&s.input;s.app.output_attrs=s.attrs.data();s.app.is_quant=true;
        s.app.model_width=640;s.app.model_height=640;s.app.model_channel=3;
        std::ifstream labels(s.config.labels_path);std::string line;
        while(std::getline(labels,line)) { if(!line.empty() && line.back()=='\r')line.pop_back();s.labels.push_back(line); }
        require(s.labels.size()==80,"expected 80 class labels");
    } catch(...) { close();throw; }
}
DetectionResult RknnDetector::detect(const camera::VideoFrame& frame) {
    auto& s=*impl_;require(s.app.rknn_ctx!=0,"detector is closed");
    const auto start=std::chrono::steady_clock::now();
    auto image=nv12_letterbox(frame);
    rknn_input tensor{};tensor.type=RKNN_TENSOR_UINT8;tensor.fmt=RKNN_TENSOR_NHWC;
    tensor.buf=image.rgb.data();tensor.size=image.rgb.size();
    require(rknn_inputs_set(s.app.rknn_ctx,1,&tensor)==0,"rknn_inputs_set failed");
    // Runtime 的阻塞超时限制停止等待；调用返回后才能释放上下文。
    rknn_run_extend run{};run.timeout_ms=1000;
    require(rknn_run(s.app.rknn_ctx,&run)==0,"rknn_run failed or timed out");
    OutputGuard output{s.app.rknn_ctx};
    for(unsigned i=0;i<9;++i)output.outputs[i].index=i;
    require(rknn_outputs_get(s.app.rknn_ctx,9,output.outputs.data(),nullptr)==0,"rknn_outputs_get failed");
    output.acquired=true;
    object_detect_result_list objects{};letterbox_t letterbox{image.x_pad,image.y_pad,image.scale};
    require(post_process(&s.app,output.outputs.data(),&letterbox,0.25f,0.45f,&objects)==0,"postprocess failed");
    DetectionResult result;result.sequence=frame.sequence;result.source_time=frame.timestamp;
    result.width=frame.width;result.height=frame.height;
    for(int i=0;i<objects.count;++i) {
        const auto& d=objects.results[i];
        if(d.cls_id<0 || d.cls_id>=80)continue;
        Detection box{d.cls_id,s.labels[d.cls_id],d.prop,
            std::clamp(d.box.left,0,frame.width),std::clamp(d.box.top,0,frame.height),
            std::clamp(d.box.right,0,frame.width),std::clamp(d.box.bottom,0,frame.height)};
        if(box.right>box.left && box.bottom>box.top)result.objects.push_back(std::move(box));
    }
    result.inference_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    return result;
}
}

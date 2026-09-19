#include "rknn_detector.hpp"
#include "ai/letterbox/nv12_letterbox.hpp"
#ifdef RKMON_WITH_RGA
#include "ai/letterbox/rga_letterbox.hpp"
#endif
#include "yolov8.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>
namespace rkmon::ai {
namespace {
void require(bool value,const char* reason) { if(!value) throw std::runtime_error(reason); }
struct OutputGuard {
    rknn_context context;
    std::array<rknn_output,9> outputs{};
    bool acquired{false};
    ~OutputGuard() { if(acquired) rknn_outputs_release(context,outputs.size(),outputs.data()); }
};
}
struct RknnDetector::Impl {
    InferenceConfig config;
    unsigned worker_index{};
    rknn_app_context_t app{};
    rknn_tensor_attr input{};
    rknn_tensor_mem* input_mem{};
    std::vector<rknn_tensor_attr> attrs{9};
    std::vector<std::string> labels;
    ModelImage cpu_image;
#ifdef RKMON_WITH_RGA
    std::unique_ptr<RgaLetterbox> rga;
#endif
};
RknnDetector::RknnDetector(InferenceConfig c, unsigned worker_index):impl_(std::make_unique<Impl>()) {
    require(c.workers>=1 && c.workers<=3 && worker_index<c.workers,"invalid NPU worker index");
    require(c.core_policy=="auto" || c.core_policy=="split","invalid NPU core policy");
    impl_->config=std::move(c);impl_->worker_index=worker_index;
}
RknnDetector::~RknnDetector() { close(); }
void RknnDetector::close() noexcept {
#ifdef RKMON_WITH_RGA
    impl_->rga.reset();
#endif
    if(impl_->input_mem)rknn_destroy_mem(impl_->app.rknn_ctx,impl_->input_mem);
    impl_->input_mem=nullptr;
    impl_->cpu_image={};
    if(impl_->app.rknn_ctx) rknn_destroy(impl_->app.rknn_ctx);
    impl_->app={}; impl_->labels.clear();
}
void RknnDetector::open() {
    close(); auto& s=*impl_;
    try {
        require(s.config.preprocess=="rga" || s.config.preprocess=="cpu","invalid AI preprocess backend");
#ifndef RKMON_WITH_RGA
        require(s.config.preprocess!="rga","RGA preprocessing requires RKMON_WITH_RGA=ON");
#endif
        std::ifstream file(s.config.model_path,std::ios::binary);
        require(file.good(),"cannot open RKNN model");
        std::vector<char> model((std::istreambuf_iterator<char>(file)),{});
        require(!model.empty() && model.size()<256*1024*1024,"invalid model size");
        require(rknn_init(&s.app.rknn_ctx,model.data(),model.size(),0,nullptr)==0,"rknn_init failed");
        const auto core=s.config.core_policy=="split"
            ? static_cast<rknn_core_mask>(1u<<s.worker_index) : RKNN_NPU_CORE_AUTO;
        require(rknn_set_core_mask(s.app.rknn_ctx,core)==0,"rknn_set_core_mask failed");
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
#ifdef RKMON_WITH_RGA
        if(s.config.preprocess=="rga") {
            require(s.config.input_memory=="dmabuf" || s.config.input_memory=="copy","invalid AI input memory");
            if(s.config.input_memory=="dmabuf") {
                auto attr=s.input;attr.type=RKNN_TENSOR_UINT8;attr.fmt=RKNN_TENSOR_NHWC;attr.pass_through=0;
                const auto stride=attr.w_stride?attr.w_stride:640;
                require(stride>=640 && stride<=32768,"invalid RKNN RGB width stride");
                const auto bytes=std::max(attr.size_with_stride,stride*640*3);
                s.input_mem=rknn_create_mem2(s.app.rknn_ctx,bytes,RKNN_FLAG_MEMORY_NON_CACHEABLE);
                require(s.input_mem && s.input_mem->fd>=0 && s.input_mem->offset==0,"cannot allocate RKNN DMA input");
                require(rknn_set_io_mem(s.app.rknn_ctx,s.input_mem,&attr)==0,"cannot bind RKNN DMA input");
                s.rga=std::make_unique<RgaLetterbox>(640,s.input_mem->fd,s.input_mem->size,stride);
            } else s.rga=std::make_unique<RgaLetterbox>(s.app.model_width);
        }
#endif
        if(s.config.preprocess=="cpu")s.cpu_image.rgb.reserve(static_cast<std::size_t>(s.app.model_width)*s.app.model_height*3);
    } catch(...) { close();throw; }
}
DetectionResult RknnDetector::detect(const camera::VideoFrame& frame) {
    auto& s=*impl_;require(s.app.rknn_ctx!=0,"detector is closed");
    using Clock=std::chrono::steady_clock;
    const auto start=Clock::now();
    const ModelImage* image=nullptr;
#ifdef RKMON_WITH_RGA
    if(s.rga)image=&s.rga->process(frame);
    else
#endif
    {
        nv12_letterbox(frame,s.cpu_image,s.app.model_width);
        image=&s.cpu_image;
    }
    const auto preprocessed=Clock::now();
    rknn_input tensor{};tensor.type=RKNN_TENSOR_UINT8;tensor.fmt=RKNN_TENSOR_NHWC;
    // RKNN 的输入接口使用 void*，同步调用期间预处理结果保持存活且不被覆盖。
    tensor.buf=const_cast<std::uint8_t*>(image->rgb.data());tensor.size=image->rgb.size();
    // DMA 路径已绑定输入，RGA 同步完成后 NPU 直接读取；CPU 未访问该非缓存内存。
    if(!s.input_mem)require(rknn_inputs_set(s.app.rknn_ctx,1,&tensor)==0,"rknn_inputs_set failed");
    const auto input_ready=Clock::now();
    // Runtime 的阻塞超时限制停止等待；调用返回后才能释放上下文。
    rknn_run_extend run{};run.timeout_ms=1000;
    require(rknn_run(s.app.rknn_ctx,&run)==0,"rknn_run failed or timed out");
    const auto inferred=Clock::now();
    OutputGuard output{s.app.rknn_ctx};
    for(unsigned i=0;i<9;++i)output.outputs[i].index=i;
    require(rknn_outputs_get(s.app.rknn_ctx,9,output.outputs.data(),nullptr)==0,"rknn_outputs_get failed");
    output.acquired=true;
    object_detect_result_list objects{};letterbox_t letterbox{image->x_pad,image->y_pad,image->scale};
    require(post_process(&s.app,output.outputs.data(),&letterbox,0.25f,0.45f,&objects)==0,"postprocess failed");
    DetectionResult result;result.sequence=frame.sequence;result.source_time=frame.timestamp;
    result.source_dma=bool(frame.dma);result.input_dma=s.input_mem!=nullptr;
    result.source_generation=frame.source_generation;result.worker_index=s.worker_index;
    result.width=frame.width;result.height=frame.height;
    for(int i=0;i<objects.count;++i) {
        const auto& d=objects.results[i];
        if(d.cls_id<0 || d.cls_id>=80)continue;
        Detection box{d.cls_id,s.labels[d.cls_id],d.prop,
            std::clamp(d.box.left,0,frame.width),std::clamp(d.box.top,0,frame.height),
            std::clamp(d.box.right,0,frame.width),std::clamp(d.box.bottom,0,frame.height)};
        if(box.right>box.left && box.bottom>box.top)result.objects.push_back(std::move(box));
    }
    const auto finished=Clock::now();
    result.preprocess_ms=std::chrono::duration<double,std::milli>(preprocessed-start).count();
    result.input_ms=std::chrono::duration<double,std::milli>(input_ready-preprocessed).count();
    result.npu_ms=std::chrono::duration<double,std::milli>(inferred-input_ready).count();
    result.postprocess_ms=std::chrono::duration<double,std::milli>(finished-inferred).count();
    result.inference_ms=std::chrono::duration<double,std::milli>(finished-start).count();
    return result;
}
}

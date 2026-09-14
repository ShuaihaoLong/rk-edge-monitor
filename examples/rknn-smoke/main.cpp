// 独立能力探针：固定 Model Zoo YOLOv8n 的 640x640 RGB 样图及九输出 INT8 模型。
// 不采集相机、不改变监控服务；后处理沿用已固定版本的官方实现。
#include "yolov8.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
void check(bool ok, const char* operation) {
    if (!ok) throw std::runtime_error(operation);
}
struct Runtime {
    rknn_app_context_t app{};
    ~Runtime() { if (app.rknn_ctx) rknn_destroy(app.rknn_ctx); }
};
struct Outputs {
    rknn_context context;
    std::vector<rknn_output> data;
    bool acquired{false};
    ~Outputs() { if (acquired) rknn_outputs_release(context, data.size(), data.data()); }
};
double ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b-a).count();
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "用法：rkmon_rknn_smoke 模型.rknn bus.jpg\n"
                     "工作目录需包含 model/coco_80_labels_list.txt；输出 result.png 和性能统计。\n";
        return 0;
    }
    if (argc != 3) return 2;
    try {
        std::ifstream file(argv[1], std::ios::binary);
        check(file.good(), "cannot open model");
        std::vector<char> model((std::istreambuf_iterator<char>(file)), {});
        check(!model.empty(), "empty model");
        Runtime runtime;
        auto& app = runtime.app;
        check(rknn_init(&app.rknn_ctx, model.data(), model.size(), 0, nullptr) == 0, "rknn_init failed");
        rknn_sdk_version version{};
        check(rknn_query(app.rknn_ctx, RKNN_QUERY_SDK_VERSION, &version, sizeof(version)) == 0, "query version failed");
        std::cout << "Runtime=" << version.api_version << " Driver=" << version.drv_version << '\n';
        check(rknn_query(app.rknn_ctx, RKNN_QUERY_IN_OUT_NUM, &app.io_num, sizeof(app.io_num)) == 0, "query counts failed");
        check(app.io_num.n_input == 1 && app.io_num.n_output == 9, "expected one input and nine outputs");
        rknn_tensor_attr input{};
        check(rknn_query(app.rknn_ctx, RKNN_QUERY_INPUT_ATTR, &input, sizeof(input)) == 0, "query input failed");
        check(input.n_dims == 4 && input.fmt == RKNN_TENSOR_NHWC && input.dims[0] == 1 &&
              input.dims[1] == 640 && input.dims[2] == 640 && input.dims[3] == 3, "expected NHWC 1x640x640x3");
        std::vector<rknn_tensor_attr> attrs(9);
        for (unsigned i=0; i<attrs.size(); ++i) {
            auto& attr=attrs[i]; attr.index=i;
            check(rknn_query(app.rknn_ctx, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr)) == 0, "query output failed");
            const unsigned side=80u >> (i/3), channels=(i%3 == 0 ? 64 : i%3 == 1 ? 80 : 1);
            check(attr.n_dims == 4 && attr.fmt == RKNN_TENSOR_NCHW && attr.type == RKNN_TENSOR_INT8 &&
                  attr.qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC && attr.dims[0] == 1 &&
                  attr.dims[1] == channels && attr.dims[2] == side && attr.dims[3] == side,
                  "output layout does not match paired YOLOv8 postprocess");
        }
        app.input_attrs=&input; app.output_attrs=attrs.data();
        app.model_width=640; app.model_height=640; app.model_channel=3; app.is_quant=true;
        int width=0,height=0,channels=0;
        std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels(stbi_load(argv[2], &width, &height, &channels, 3), &stbi_image_free);
        check(pixels && width == 640 && height == 640, "probe requires 640x640 sample image");
        check(init_post_process() == 0, "cannot load labels");
        struct Labels { ~Labels() { deinit_post_process(); } } labels;
        rknn_input tensor{};
        tensor.index=0; tensor.type=RKNN_TENSOR_UINT8; tensor.fmt=RKNN_TENSOR_NHWC;
        tensor.size=640*640*3; tensor.buf=pixels.get();
        letterbox_t letterbox{0,0,1.0f};
        object_detect_result_list results{};
        std::vector<double> run_times, total_times;
        // 五次预热后统计三十次；总时间包含输入提交、输出获取和 CPU 后处理。
        for (int iteration=0; iteration<35; ++iteration) {
            auto start=std::chrono::steady_clock::now();
            check(rknn_inputs_set(app.rknn_ctx, 1, &tensor) == 0, "input submission failed");
            auto before_run=std::chrono::steady_clock::now();
            check(rknn_run(app.rknn_ctx, nullptr) == 0, "rknn_run failed");
            auto after_run=std::chrono::steady_clock::now();
            Outputs outputs{app.rknn_ctx, std::vector<rknn_output>(9)};
            for (unsigned i=0;i<9;++i) outputs.data[i].index=i;
            check(rknn_outputs_get(app.rknn_ctx, 9, outputs.data.data(), nullptr) == 0, "get outputs failed");
            outputs.acquired=true;
            check(post_process(&app, outputs.data.data(), &letterbox, 0.25f, 0.45f, &results) == 0, "postprocess failed");
            bool person=false,bus=false;
            for (int i=0;i<results.count;++i) {
                const auto& d=results.results[i];
                person |= d.cls_id==0; bus |= d.cls_id==5;
                check(d.box.left>=0 && d.box.top>=0 && d.box.right<=640 && d.box.bottom<=640 &&
                      d.box.right>d.box.left && d.box.bottom>d.box.top, "invalid bounding box");
            }
            check(person && bus, "sample should detect person and bus");
            if (iteration>=5) {
                run_times.push_back(ms(before_run,after_run));
                total_times.push_back(ms(start,std::chrono::steady_clock::now()));
            }
        }
        auto report=[](const char* name, std::vector<double> values) {
            std::sort(values.begin(),values.end()); double sum=0;for(auto v:values)sum+=v;
            std::cout << name << " mean_ms=" << sum/values.size() << " p50_ms=" << values[values.size()/2]
                      << " p95_ms=" << values[(values.size()*95-1)/100] << '\n';
        };
        report("rknn_run",run_times); report("input_run_outputs_postprocess",total_times);
        for (int i=0;i<results.count;++i) {
            const auto& d=results.results[i];
            std::cout << coco_cls_to_name(d.cls_id) << " score=" << d.prop << " box=" << d.box.left << ',' << d.box.top << ',' << d.box.right << ',' << d.box.bottom << '\n';
            for (int y=d.box.top;y<std::min(d.box.bottom,640);++y)
                for (int x=d.box.left;x<std::min(d.box.right,640);++x)
                    if (y<d.box.top+2 || y>=d.box.bottom-2 || x<d.box.left+2 || x>=d.box.right-2) {
                        auto* p=pixels.get()+(y*640+x)*3;p[0]=0;p[1]=255;p[2]=0;
                    }
        }
        check(stbi_write_png("result.png",640,640,3,pixels.get(),640*3) != 0, "write image failed");
        std::cout << "PASS: 35 inferences; person/bus and box bounds checked each time\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}

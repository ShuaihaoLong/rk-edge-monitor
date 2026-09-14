#include "inference_service.hpp"
#include <stdexcept>
namespace rkmon::ai {
InferenceService::InferenceService(std::unique_ptr<IObjectDetector> detector,std::shared_ptr<Queue> input,
    InferenceConfig config,std::shared_ptr<spdlog::logger> logger)
    :detector_(std::move(detector)),input_(std::move(input)),config_(std::move(config)),writer_(config_.result_path),logger_(std::move(logger)) {
    if(!detector_ || !input_ || !config_.fps || config_.fps>60 || config_.result_path.empty())
        throw std::invalid_argument("invalid AI service configuration");
}
InferenceService::~InferenceService(){request_stop();join();}
bool InferenceService::start(){
    if(running_)return true;
    if(worker_.joinable())throw std::logic_error("join AI before restarting");
    stop_=false;running_=true;
    try { worker_=std::thread(&InferenceService::run,this); }
    catch(...) {running_=false;throw;}
    return true;
}
void InferenceService::request_stop() noexcept {stop_=true;}
void InferenceService::join() noexcept {if(worker_.joinable())worker_.join();running_=false;}
bool InferenceService::running() const noexcept {return running_;}
core::HealthSnapshot InferenceService::health() const {
    std::lock_guard lock(mutex_);
    return {running_?core::ServiceState::running:core::ServiceState::stopped,detail_};
}
void InferenceService::publish(const std::string& status,const DetectionResult* result) noexcept {
    try {
        writer_.write(status,result);
        std::lock_guard lock(mutex_);detail_=status=="ok"?"":status;
    } catch(const std::exception& e) {
        std::lock_guard lock(mutex_);detail_=e.what();
    }
}
void InferenceService::run() noexcept {
    using Clock=std::chrono::steady_clock;
    bool opened=false;
    auto retry=Clock::now(),next=retry,heartbeat=retry,last_input=retry;
    publish("starting");
    while(!stop_) {
        try {
            auto now=Clock::now();
            if(!opened && now>=retry) {
                detector_->open();opened=true;last_input=now;
                publish("waiting");
            }
            if(!opened || now<next) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));continue;
            }
            auto frame=input_->pop_for(std::chrono::milliseconds(20));
            if(stop_)break;
            if(!frame) {
                if(Clock::now()-last_input>std::chrono::seconds(1) && Clock::now()>=heartbeat) {
                    publish("waiting");heartbeat=Clock::now()+std::chrono::milliseconds(500);
                }
                continue;
            }
            last_input=Clock::now();
            // 丢弃已经过期的输入，避免将旧图像识别结果当作实时结果发布。
            if(last_input-frame->timestamp>std::chrono::milliseconds(700))continue;
            auto result=detector_->detect(*frame);
            if(stop_)break;
            publish("ok",&result);
            next=last_input+std::chrono::milliseconds(1000/config_.fps);
        } catch(const std::exception& error) {
            detector_->close();opened=false;
            publish("unavailable");
            try { if(logger_)logger_->warn("[ai] {}; retry in 2s",error.what()); } catch(...) {}
            retry=Clock::now()+std::chrono::seconds(2);
        } catch(...) {
            detector_->close();opened=false;publish("unavailable");retry=Clock::now()+std::chrono::seconds(2);
        }
    }
    detector_->close();publish("stopped");running_=false;
}
}

#include "inference_service.hpp"
#include "core/thread_pool.hpp"
#include <algorithm>
#include <stdexcept>
namespace rkmon::ai {
namespace {
std::vector<std::unique_ptr<IObjectDetector>> single(std::unique_ptr<IObjectDetector> detector) {
    std::vector<std::unique_ptr<IObjectDetector>> detectors;
    detectors.push_back(std::move(detector));return detectors;
}
}
InferenceService::InferenceService(std::unique_ptr<IObjectDetector> detector,std::shared_ptr<Queue> input,
    InferenceConfig config,std::shared_ptr<spdlog::logger> logger)
    :InferenceService(single(std::move(detector)),std::move(input),std::move(config),std::move(logger)) {}
InferenceService::InferenceService(std::vector<std::unique_ptr<IObjectDetector>> detectors,std::shared_ptr<Queue> input,
    InferenceConfig config,std::shared_ptr<spdlog::logger> logger)
    :detectors_(std::move(detectors)),input_(std::move(input)),config_(std::move(config)),writer_(config_.result_path,config_.event_socket),logger_(std::move(logger)) {
    if(!input_ || !config_.fps || config_.fps>60 || config_.result_path.empty() ||
       config_.workers<1 || config_.workers>3 || detectors_.size()!=config_.workers ||
       (config_.core_policy!="auto" && config_.core_policy!="split") ||
       std::any_of(detectors_.begin(),detectors_.end(),[](const auto& d){return !d;}))
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
    const auto ready=ready_workers_.load();
    auto detail=detail_;
    if(running_ && ready!=config_.workers) {
        if(!detail.empty())detail+="; ";
        detail+="ready workers "+std::to_string(ready)+"/"+std::to_string(config_.workers);
    }
    return {running_?(ready==config_.workers && detail.empty()?core::ServiceState::running:
                     core::ServiceState::degraded):core::ServiceState::stopped,detail};
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
    using Result=std::optional<DetectionResult>;
    struct Worker {
        std::optional<std::future<Result>> job;
        bool ready{false};
        Clock::time_point retry{};
    };
    publish("starting");
    try {
        // 每个检测器仅由对应线程访问；线程退出前释放 RKNN/RGA 资源。
        core::ThreadPool pool(detectors_.size(),[this](std::size_t i){detectors_[i]->close();});
        std::vector<Worker> workers(detectors_.size());
        std::optional<camera::VideoFrame> pending;
        std::optional<std::uint64_t> generation,last_sequence;
        auto next=Clock::now(),heartbeat=next,last_result=next;
        const auto interval=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0/config_.fps));
        constexpr auto max_age=std::chrono::milliseconds(700);
        std::size_t cursor=0;
        if(logger_)logger_->info("[ai] workers={}, core_policy={}, total fps={}",config_.workers,config_.core_policy,config_.fps);
        while(!stop_) {
            auto now=Clock::now();
            // 先观察输入代次，再收结果，防止重连前的在途任务覆盖新代次。
            if(auto frame=input_->pop_for(std::chrono::milliseconds(10))) {
                if(!generation || frame->source_generation>*generation) {
                    generation=frame->source_generation;last_sequence.reset();pending.reset();publish("waiting");
                }
                if(frame->source_generation==*generation && now-frame->timestamp<=max_age &&
                   (!pending || frame->sequence>pending->sequence)) pending=std::move(*frame);
            }
            if(stop_)break;
            now=Clock::now();
            for(std::size_t i=0;i<workers.size();++i) {
                auto& worker=workers[i];
                if(worker.job && worker.job->wait_for(std::chrono::milliseconds(0))==std::future_status::ready) {
                    try {
                        auto result=worker.job->get();worker.job.reset();
                        if(!result) worker.ready=true;
                        else if(generation && result->source_generation==*generation &&
                                (!last_sequence || result->sequence>*last_sequence) && now-result->source_time<=max_age) {
                            last_sequence=result->sequence;last_result=now;publish("ok",&*result);
                        }
                    } catch(...) {
                        worker.job.reset();worker.ready=false;worker.retry=now+std::chrono::seconds(2);
                        try { throw; }
                        catch(const std::exception& e) {try {if(logger_)logger_->warn("[ai] worker {}: {}; retry in 2s",i,e.what());}catch(...) {}}
                        catch(...) {try {if(logger_)logger_->warn("[ai] worker {} failed; retry in 2s",i);}catch(...) {}}
                    }
                }
                if(!worker.ready && !worker.job && now>=worker.retry) {
                    worker.job=pool.try_submit(i,[this](std::size_t index)->Result {
                        try {detectors_[index]->open();return std::nullopt;}
                        catch(...) {detectors_[index]->close();throw;}
                    });
                }
            }
            ready_workers_=static_cast<unsigned>(std::count_if(workers.begin(),workers.end(),[](const auto& w){return w.ready;}));
            if(pending && now-pending->timestamp>max_age)pending.reset();
            if(pending && now>=next) {
                for(std::size_t offset=0;offset<workers.size();++offset) {
                    const auto i=(cursor+offset)%workers.size();
                    auto& worker=workers[i];
                    if(!worker.ready || worker.job)continue;
                    auto job=pool.try_submit(i,[this,frame=*pending](std::size_t index)->Result {
                        try {
                            auto result=detectors_[index]->detect(frame);
                            result.sequence=frame.sequence;result.source_generation=frame.source_generation;
                            result.received_at=frame.received_at;
                            result.source_time=frame.timestamp;result.worker_index=static_cast<unsigned>(index);
                            return result;
                        } catch(...) {detectors_[index]->close();throw;}
                    });
                    if(job) {
                        worker.job=std::move(job);pending.reset();cursor=(i+1)%workers.size();next=now+interval;break;
                    }
                }
            }
            if(now>=heartbeat) {
                if(!ready_workers_)publish("unavailable");
                else if(now-last_result>std::chrono::seconds(1))publish("waiting");
                heartbeat=now+std::chrono::milliseconds(500);
            }
        }
        pool.request_stop();pool.join();
    } catch(const std::exception& e) {
        try {if(logger_)logger_->error("[ai] coordinator stopped: {}",e.what());}catch(...) {}
    } catch(...) {}
    ready_workers_=0;publish("stopped");running_=false;
}
}

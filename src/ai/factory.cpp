#include "ai/detector.hpp"
#ifdef RKMON_WITH_RKNN
#include "ai/rknn_detector.hpp"
#endif
#include <stdexcept>
namespace rkmon::ai {
std::unique_ptr<IObjectDetector> make_detector(const InferenceConfig& config) {
#ifdef RKMON_WITH_RKNN
    return std::make_unique<RknnDetector>(config);
#else
    (void)config;
    throw std::runtime_error("AI requires RKMON_WITH_RKNN=ON");
#endif
}
}

#pragma once

#include <string>

namespace rkmon::ai {
struct InferenceConfig {
    std::string model_path, labels_path, result_path;
    unsigned fps{10};
    std::string preprocess{"rga"};
    unsigned workers{1};
    std::string core_policy{"auto"};
};
}

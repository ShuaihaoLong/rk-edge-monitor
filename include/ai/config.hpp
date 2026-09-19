#pragma once

#include <string>

namespace rkmon::ai {
struct InferenceConfig {
    std::string model_path, labels_path, result_path;
    unsigned fps{10};
    std::string event_socket;
    std::string preprocess{"rga"};
    std::string input_memory{"dmabuf"};
    unsigned workers{1};
    std::string core_policy{"auto"};
};
}

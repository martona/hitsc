#pragma once

#include <string>

namespace hitsc {

struct KvmCaptureDecodeOptions {
    std::string input_path;
    std::string output_directory;
    int frame_limit = 20;
};

void decode_kvm_capture(const KvmCaptureDecodeOptions& options);

} // namespace hitsc

#pragma once

#include <string>

namespace hitsc {

struct MegaracCaptureDecodeOptions {
    std::string input_path;
    std::string output_directory;
    int frame_limit = 20;
};

void decode_megarac_capture(const MegaracCaptureDecodeOptions& options);

} // namespace hitsc

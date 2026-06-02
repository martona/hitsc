#pragma once

#include "options.hpp"

namespace hitsc {

struct ViewWindow;

void run_pikvm_view(const PikvmViewOptions& options, const ViewWindow* handoff = nullptr);

} // namespace hitsc

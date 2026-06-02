#pragma once

#include "options.hpp"

namespace hitsc {

struct ViewWindow;

void run_aten_view(const AtenViewOptions& options, const ViewWindow* handoff = nullptr);

} // namespace hitsc

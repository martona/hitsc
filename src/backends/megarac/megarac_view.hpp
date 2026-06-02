#pragma once

#include "options.hpp"

namespace hitsc {

struct ViewWindow;

void run_megarac_view(const MegaracViewOptions& options, const ViewWindow* handoff = nullptr);

} // namespace hitsc

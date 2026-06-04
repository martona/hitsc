#pragma once

#include "options.hpp"

#include <memory>

namespace hitsc {

class KvmViewBase;

// Construct a MegaRAC view (not yet attached to a window). Used by
// run_megarac_view and by auto-detect once it resolves to MegaRAC.
std::unique_ptr<KvmViewBase> make_megarac_view(const MegaracViewOptions& options);

void run_megarac_view(const MegaracViewOptions& options);

} // namespace hitsc

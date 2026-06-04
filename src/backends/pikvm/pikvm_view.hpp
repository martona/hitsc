#pragma once

#include "options.hpp"

#include <memory>

namespace hitsc {

class KvmViewBase;

// Construct a PiKVM view (not yet attached to a window). Used by run_pikvm_view
// and by auto-detect once it resolves to PiKVM.
std::unique_ptr<KvmViewBase> make_pikvm_view(const PikvmViewOptions& options);

void run_pikvm_view(const PikvmViewOptions& options);

} // namespace hitsc

#pragma once

#include "options.hpp"

#include <memory>

namespace hitsc {

class KvmViewBase;

// Construct an ATEN view (not yet attached to a window). Used by run_aten_view
// and by auto-detect once it resolves to ATEN.
std::unique_ptr<KvmViewBase> make_aten_view(const AtenViewOptions& options);

void run_aten_view(const AtenViewOptions& options);

} // namespace hitsc

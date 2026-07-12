#pragma once

#include "options.hpp"

#include <string>

namespace hitsc {

void run_auto_view(const AutoViewOptions& options);

// Blocking fingerprint probe of GET / (the same scoring run_auto_view uses).
// Returns "megarac"/"aten"/"pikvm", or an empty string when undetermined. Used by
// flows that gate a backend-specific action on the detected type (BMC cold reset).
std::string detect_kvm_backend_name(LoginOptions& login);

} // namespace hitsc

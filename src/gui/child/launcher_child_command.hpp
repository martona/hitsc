#pragma once

#include "options.hpp"

namespace hitsc {

int run_launcher_child(VerbosityOptions verbosity);

// The "coldreset" verb: same stdin launch-request handshake as "child", but after
// authenticating it asks the BMC to cold-reset itself instead of opening a KVM
// session. MegaRAC only (auto-detected hosts probe first).
int run_launcher_child_cold_reset(VerbosityOptions verbosity);

} // namespace hitsc

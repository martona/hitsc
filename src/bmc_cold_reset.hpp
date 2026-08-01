#pragma once

#include "options.hpp"

namespace hitsc {

enum class ColdResetBackend {
    Detect,   // fingerprint GET / first ("auto" hosts); MegaRAC or ATEN may proceed
    Megarac,
    Aten,
};

struct BmcColdResetOptions {
    LoginOptions login;
    ColdResetBackend backend = ColdResetBackend::Detect;
};

// Opens the standard viewer window in console-only mode (no KVM view is ever
// attached), authenticates against the BMC, and POSTs the backend's reset
// (MegaRAC: /api/maintenance/reset; ATEN: Redfish Manager.Reset). The window
// stays open on the success/failure status screen until the user closes it
// (Esc). Returns EXIT_SUCCESS/EXIT_FAILURE by reset outcome.
int run_bmc_cold_reset(const BmcColdResetOptions& options);

} // namespace hitsc

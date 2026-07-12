#pragma once

#include "options.hpp"

namespace hitsc {

struct BmcColdResetOptions {
    LoginOptions login;
    // Fingerprint GET / first ("auto" hosts); only a MegaRAC result may proceed.
    // False for hosts already saved as MegaRAC.
    bool detect_backend = false;
};

// Opens the standard viewer window in console-only mode (no KVM view is ever
// attached), authenticates against the BMC, and POSTs the MegaRAC maintenance
// reset. The window stays open on the success/failure status screen until the
// user closes it (Esc). Returns EXIT_SUCCESS/EXIT_FAILURE by reset outcome.
int run_bmc_cold_reset(const BmcColdResetOptions& options);

} // namespace hitsc

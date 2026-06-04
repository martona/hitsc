#pragma once

#include <openssl/types.h>

#include <string>

namespace hitsc {

// Attach the window and host identity the certificate prompt uses. Called once,
// as soon as the viewer window exists. native_window_handle is the viewer window
// (a QWidget*; may be null when there is no window — headless probes, etc., in
// which case the broker can never prompt and falls back to strict verification).
// host_id keys the persistent pin in the host store; an empty id disables
// persistence (the cert can still be accepted for the lifetime of the process).
void cert_trust_attach_window(void* native_window_handle, std::string host_id);

// Decide whether to proceed with a TLS connection presenting `leaf`. Called from
// the verify callback at the leaf certificate. os_chain_trusted reports whether
// the system chain verification (every depth) passed; verify_error is the first
// X509_V_* failure code seen while walking the chain. The hostname check is done
// here against `leaf`. May block showing a modal prompt. Returns true to proceed
// with the handshake, false to abort it.
bool cert_trust_evaluate(
    const std::string& host,
    X509* leaf,
    bool os_chain_trusted,
    long verify_error);

} // namespace hitsc

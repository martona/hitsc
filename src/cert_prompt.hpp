#pragma once

#include <optional>
#include <string>

namespace hitsc {

// Fields shown in the certificate-trust prompt.
struct CertPromptInfo {
    std::string subject;     // leaf common name (empty if absent)
    std::string issuer;      // issuer common name (empty if absent)
    std::string valid_from;  // human-readable notBefore
    std::string valid_to;    // human-readable notAfter
    std::string fingerprint; // SHA-256, colon-delimited hex (display form)
    std::string reason;      // why the OS won't vouch for it
};

// The user's choice from the prompt.
struct CertPromptResult {
    bool proceed = false; // connect with this certificate
    bool pin = false;     // remember it for this host (checkbox)
};

// Show a modal certificate-trust prompt parented to native_parent (a QWidget* --
// the viewer window; may be null). `changed` selects the red "certificate
// changed" variant over the first-contact "untrusted certificate" one;
// previous_fingerprint is shown in that variant. allow_pin toggles the remember
// checkbox (off when there is no host to persist against, e.g. a direct CLI
// launch). Safe to call from any thread -- it marshals onto the GUI thread and
// blocks until the user chooses. With no QApplication (headless), returns
// {proceed:false}.
CertPromptResult show_cert_prompt(
    void* native_parent,
    const std::string& host,
    const CertPromptInfo& info,
    bool changed,
    const std::optional<std::string>& previous_fingerprint,
    bool allow_pin);

// Release a certificate prompt currently blocked waiting on the GUI thread,
// resolving it as "do not proceed". Call on the GUI thread before joining the
// network/detection thread on shutdown, so a window-close while a prompt is
// pending can't deadlock (the worker is blocked waiting for the GUI to show the
// dialog; this unblocks it). No-op when no prompt is pending.
void cancel_pending_cert_prompt();

} // namespace hitsc

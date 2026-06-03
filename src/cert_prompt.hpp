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

// Show a modal certificate-trust prompt parented to native_parent (an HWND on
// Windows; may be null). `changed` selects the red "certificate changed" variant
// over the first-contact "untrusted certificate" one; previous_fingerprint is
// shown in that variant. allow_pin toggles the remember checkbox (off when there
// is no host to persist against, e.g. a direct CLI launch). Blocks until the
// user chooses. On non-Windows, or if the rich dialog cannot be shown, returns
// {proceed:false}.
CertPromptResult show_cert_prompt(
    void* native_parent,
    const std::string& host,
    const CertPromptInfo& info,
    bool changed,
    const std::optional<std::string>& previous_fingerprint,
    bool allow_pin);

} // namespace hitsc

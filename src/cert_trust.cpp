#include "cert_trust.hpp"

#include "cert_prompt.hpp"
#include "gui/launcher_host_store.hpp"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <QString>

#include <cctype>
#include <cstddef>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace hitsc {
namespace {

// Process-wide trust state. The viewer is one process per host, so a single
// broker fits: it remembers the window to parent prompts on, the host id to key
// the persistent pin, and the certificates the user has approved this run (so a
// login's REST and WebSocket handshakes only ever prompt once).
struct TrustBroker {
    std::mutex mutex;
    void* native_window = nullptr;
    std::string host_id;
    bool pin_loaded = false;
    std::optional<std::string> pinned;  // canonical fingerprint (upper hex, no separators)
    std::set<std::string> approved;     // canonical fingerprints approved this process
};

TrustBroker& broker()
{
    static TrustBroker instance;
    return instance;
}

std::string to_hex_upper(const unsigned char* data, unsigned int length, bool with_colons)
{
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(static_cast<std::size_t>(length) * (with_colons ? 3 : 2));
    for (unsigned int i = 0; i < length; ++i) {
        if (with_colons && i != 0) {
            out.push_back(':');
        }
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0F]);
    }
    return out;
}

std::optional<std::string> sha256_fingerprint(X509* cert)
{
    if (cert == nullptr) {
        return std::nullopt;
    }
    unsigned char buffer[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (X509_digest(cert, EVP_sha256(), buffer, &length) != 1) {
        return std::nullopt;
    }
    return to_hex_upper(buffer, length, false);
}

std::string fingerprint_for_display(const std::string& canonical)
{
    std::string out;
    out.reserve(canonical.size() + canonical.size() / 2);
    for (std::size_t i = 0; i < canonical.size(); i += 2) {
        if (i != 0) {
            out.push_back(':');
        }
        out.push_back(canonical[i]);
        if (i + 1 < canonical.size()) {
            out.push_back(canonical[i + 1]);
        }
    }
    return out;
}

std::string common_name(X509_NAME* name)
{
    if (name == nullptr) {
        return {};
    }
    const int index = X509_NAME_get_index_by_NID(name, NID_commonName, -1);
    if (index < 0) {
        return {};
    }
    X509_NAME_ENTRY* entry = X509_NAME_get_entry(name, index);
    if (entry == nullptr) {
        return {};
    }
    ASN1_STRING* data = X509_NAME_ENTRY_get_data(entry);
    if (data == nullptr) {
        return {};
    }
    unsigned char* utf8 = nullptr;
    const int length = ASN1_STRING_to_UTF8(&utf8, data);
    if (length < 0 || utf8 == nullptr) {
        return {};
    }
    std::string out(reinterpret_cast<char*>(utf8), static_cast<std::size_t>(length));
    OPENSSL_free(utf8);
    return out;
}

std::string asn1_time_to_string(const ASN1_TIME* time)
{
    if (time == nullptr) {
        return {};
    }
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == nullptr) {
        return {};
    }
    std::string out;
    if (ASN1_TIME_print(bio, time) != 0) {
        char* data = nullptr;
        const long length = BIO_get_mem_data(bio, &data);
        if (data != nullptr && length > 0) {
            out.assign(data, static_cast<std::size_t>(length));
        }
    }
    BIO_free(bio);
    return out;
}

bool hostname_matches(X509* cert, const std::string& host)
{
    if (cert == nullptr || host.empty()) {
        return false;
    }
    if (X509_check_host(cert, host.c_str(), host.size(), 0, nullptr) == 1) {
        return true;
    }
    return X509_check_ip_asc(cert, host.c_str(), 0) == 1;
}

std::string untrusted_reason(long verify_error, bool hostname_ok)
{
    if (!hostname_ok && verify_error == X509_V_OK) {
        return "The certificate was issued for a different host name.";
    }
    if (verify_error == X509_V_OK) {
        return "The certificate is not issued by a trusted authority.";
    }
    const char* text = X509_verify_cert_error_string(verify_error);
    std::string reason = text != nullptr ? text : "certificate verification failed";
    if (!reason.empty()) {
        reason[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(reason[0])));
        reason.push_back('.');
    }
    return reason;
}

std::optional<std::string> load_pin(const std::string& host_id)
{
    if (host_id.empty()) {
        return std::nullopt;
    }
    try {
        const HostStore store;
        return store.load_pinned_cert(QString::fromStdString(host_id));
    } catch (...) {
        return std::nullopt;
    }
}

void save_pin(const std::string& host_id, const std::string& fingerprint)
{
    if (host_id.empty()) {
        return;
    }
    try {
        const HostStore store;
        store.save_pinned_cert(QString::fromStdString(host_id), fingerprint);
    } catch (...) {
    }
}

} // namespace

void cert_trust_attach_window(void* native_window_handle, std::string host_id)
{
    TrustBroker& state = broker();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.native_window = native_window_handle;
    state.host_id = std::move(host_id);
    state.pin_loaded = false;
    state.pinned.reset();
    state.approved.clear();
}

bool cert_trust_evaluate(
    const std::string& host,
    X509* leaf,
    bool os_chain_trusted,
    long verify_error)
{
    const std::optional<std::string> fingerprint = sha256_fingerprint(leaf);
    const bool hostname_ok = hostname_matches(leaf, host);
    const bool os_trusted = os_chain_trusted && hostname_ok;

    if (!fingerprint) {
        // Can't fingerprint it — only proceed if the OS already vouches for it.
        return os_trusted;
    }

    TrustBroker& state = broker();
    std::unique_lock<std::mutex> lock(state.mutex);

    if (state.approved.count(*fingerprint) != 0) {
        return true;
    }
    if (!state.pin_loaded) {
        state.pinned = load_pin(state.host_id);
        state.pin_loaded = true;
    }
    if (state.pinned && *state.pinned == *fingerprint) {
        state.approved.insert(*fingerprint);
        return true;
    }
    if (os_trusted) {
        // Trusted by the OS — connect, but never touch the pin. A trusted cert
        // is an independent acceptable path; the pin stays the anchor.
        return true;
    }

    const bool changed = state.pinned.has_value();
    void* native_window = state.native_window;
    const std::string host_id = state.host_id;
    std::optional<std::string> previous_display;
    if (changed) {
        previous_display = fingerprint_for_display(*state.pinned);
    }
    lock.unlock();

    if (native_window == nullptr) {
        // No UI to prompt on (headless probe / direct launch with no window):
        // fall back to strict verification — untrusted means abort.
        return false;
    }

    CertPromptInfo info;
    info.subject = common_name(X509_get_subject_name(leaf));
    info.issuer = common_name(X509_get_issuer_name(leaf));
    info.valid_from = asn1_time_to_string(X509_get0_notBefore(leaf));
    info.valid_to = asn1_time_to_string(X509_get0_notAfter(leaf));
    info.fingerprint = fingerprint_for_display(*fingerprint);
    info.reason = untrusted_reason(verify_error, hostname_ok);

    const bool allow_pin = !host_id.empty();
    const CertPromptResult result =
        show_cert_prompt(native_window, host, info, changed, previous_display, allow_pin);
    if (!result.proceed) {
        return false;
    }

    lock.lock();
    state.approved.insert(*fingerprint);
    if (result.pin && !host_id.empty()) {
        save_pin(host_id, *fingerprint);
        state.pinned = *fingerprint;
    }
    return true;
}

} // namespace hitsc

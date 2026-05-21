#include "auto_view.hpp"

#include "backends/aten/aten_view.hpp"
#include "backends/megarac/megarac_view.hpp"
#include "backends/pikvm/pikvm_view.hpp"
#include "cookie_jar.hpp"
#include "errors.hpp"
#include "http_client.hpp"
#include "log.hpp"
#include "text.hpp"
#include "tls_session_cache.hpp"

#include <boost/beast/http.hpp>

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hitsc {
namespace {

enum class DetectedKvmBackend {
    Unknown,
    Megarac,
    Aten,
    Pikvm,
};

struct FingerprintCandidate {
    DetectedKvmBackend backend = DetectedKvmBackend::Unknown;
    int score = 0;
    std::vector<std::string> reasons;
};

struct KvmBackendFingerprint {
    DetectedKvmBackend backend = DetectedKvmBackend::Unknown;
    int score = 0;
    std::vector<std::string> reasons;
};

std::string backend_name(DetectedKvmBackend backend)
{
    switch (backend) {
    case DetectedKvmBackend::Megarac:
        return "megarac";
    case DetectedKvmBackend::Aten:
        return "aten";
    case DetectedKvmBackend::Pikvm:
        return "pikvm";
    case DetectedKvmBackend::Unknown:
        break;
    }
    return "unknown";
}

std::string join_reasons(const std::vector<std::string>& reasons)
{
    std::string result;
    for (const std::string& reason : reasons) {
        if (!result.empty()) {
            result += ", ";
        }
        result += reason;
    }
    return result;
}

std::string header_value(const StringResponse& response, std::string_view name)
{
    const std::string normalized_name = lower_copy(std::string(name));
    for (const auto& field : response) {
        if (lower_copy(std::string(field.name_string())) == normalized_name) {
            return std::string(field.value());
        }
    }
    return {};
}

bool header_contains(const StringResponse& response, std::string_view name, std::string_view needle)
{
    const std::string value = lower_copy(header_value(response, name));
    return value.find(lower_copy(std::string(needle))) != std::string::npos;
}

bool header_equals(const StringResponse& response, std::string_view name, std::string_view expected)
{
    const std::string value = lower_copy(trim_copy(header_value(response, name)));
    return value == lower_copy(std::string(expected));
}

bool is_redirect(const StringResponse& response)
{
    const unsigned int status = response.result_int();
    return status >= 300 && status < 400;
}

void score_if(
    FingerprintCandidate& candidate,
    bool condition,
    int points,
    std::string reason)
{
    if (!condition) {
        return;
    }
    candidate.score += points;
    candidate.reasons.push_back(std::move(reason));
}

KvmBackendFingerprint classify_root_response(const StringResponse& response)
{
    FingerprintCandidate megarac{DetectedKvmBackend::Megarac};
    FingerprintCandidate aten{DetectedKvmBackend::Aten};
    FingerprintCandidate pikvm{DetectedKvmBackend::Pikvm};

    const std::string csp = lower_copy(header_value(response, "content-security-policy"));

    score_if(pikvm, is_redirect(response), 2, "HTTP redirect from /");
    score_if(pikvm, header_equals(response, "location", "/login"), 4, "Location: /login");
    score_if(pikvm, header_contains(response, "server", "nginx"), 2, "Server: nginx");

    score_if(megarac, response.result_int() == 200, 1, "HTTP 200 from /");
    score_if(megarac, header_contains(response, "server", "lighttpd"), 4, "Server: lighttpd");
    score_if(megarac, header_contains(response, "content-encoding", "gzip"), 1, "Content-Encoding: gzip");
    score_if(megarac, header_contains(response, "referrer-policy", "no-referrer"), 2, "Referrer-Policy: no-referrer");
    score_if(megarac, csp.find("object-src 'none'") != std::string::npos, 1, "CSP object-src 'none'");
    score_if(megarac, csp.find("frame-ancestors 'self'") != std::string::npos, 1, "CSP frame-ancestors 'self'");

    score_if(aten, response.result_int() == 200, 1, "HTTP 200 from /");
    score_if(aten, header_contains(response, "content-type", "charset=utf-8"), 1, "Content-Type charset=UTF-8");
    score_if(aten, header_equals(response, "cache-control", "private"), 1, "Cache-Control: private");
    score_if(aten, csp.find("'unsafe-eval'") != std::string::npos, 3, "CSP allows unsafe-eval");
    score_if(aten, csp.find("worker-src 'self' blob:") != std::string::npos, 2, "CSP worker-src blob");
    score_if(aten, csp.find("img-src 'self' data:") != std::string::npos, 1, "CSP img-src data");

    std::vector<FingerprintCandidate> candidates{megarac, aten, pikvm};
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return left.score > right.score;
    });

    const FingerprintCandidate& best = candidates[0];
    const FingerprintCandidate& second = candidates[1];
    if (best.score < 3 || best.score == second.score) {
        return KvmBackendFingerprint{};
    }
    return KvmBackendFingerprint{best.backend, best.score, best.reasons};
}

KvmBackendFingerprint detect_kvm_backend(LoginOptions& login)
{
    if (!login.tls_session_cache) {
        login.tls_session_cache = std::make_shared<TlsSessionCache>(16);
    }

    HttpsClient client(
        login.base_url,
        login.insecure,
        login.verbose,
        10,
        login.tls_session_cache.get(),
        false);
    CookieJar cookies;
    StringResponse response = client.request(http::verb::get, "/", {}, {}, &cookies);
    return classify_root_response(response);
}

std::string detection_failure_message()
{
    return "Could not auto-detect KVM type from GET /. Choose ATEN, MegaRAC, or PiKVM manually.";
}

} // namespace

void run_auto_view(const AutoViewOptions& options)
{
    AutoViewOptions detected_options = options;
    KvmBackendFingerprint fingerprint = detect_kvm_backend(detected_options.login);

    if (fingerprint.backend == DetectedKvmBackend::Unknown) {
        throw UserError(detection_failure_message());
    }

    log_info() << "auto KVM detection selected"
               << " backend=" << backend_name(fingerprint.backend)
               << " score=" << fingerprint.score
               << " reason=" << join_reasons(fingerprint.reasons);

    switch (fingerprint.backend) {
    case DetectedKvmBackend::Megarac: {
        MegaracViewOptions view_options;
        view_options.login = std::move(detected_options.login);
        view_options.idle_timeout_seconds = detected_options.idle_timeout_seconds;
        run_megarac_view(view_options);
        return;
    }
    case DetectedKvmBackend::Aten: {
        AtenViewOptions view_options;
        view_options.login = std::move(detected_options.login);
        view_options.idle_timeout_seconds = detected_options.idle_timeout_seconds;
        view_options.shared = detected_options.aten_shared;
        run_aten_view(view_options);
        return;
    }
    case DetectedKvmBackend::Pikvm: {
        PikvmViewOptions view_options;
        view_options.login = std::move(detected_options.login);
        view_options.idle_timeout_seconds = detected_options.idle_timeout_seconds;
        view_options.video_decode = detected_options.pikvm_video_decode;
        run_pikvm_view(view_options);
        return;
    }
    case DetectedKvmBackend::Unknown:
        break;
    }

    throw UserError(detection_failure_message());
}

} // namespace hitsc

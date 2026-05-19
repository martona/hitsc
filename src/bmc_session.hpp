#pragma once

#include "http_client.hpp"
#include "options.hpp"
#include "tls_session_cache.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hitsc {

enum class BmcCredentialSource {
    Username,
    Password,
    Literal,
};

enum class BmcCredentialTransform {
    None,
    Base64,
};

struct BmcLoginField {
    std::string name;
    BmcCredentialSource source = BmcCredentialSource::Literal;
    std::string literal;
    BmcCredentialTransform transform = BmcCredentialTransform::None;
    bool encode_name = true;
};

using BmcSessionTokenParser = std::string (*)(std::string_view body);

struct BmcLoginProfile {
    std::string vendor_name;
    std::string login_target;
    std::vector<BmcLoginField> fields;
    std::string content_type = "application/x-www-form-urlencoded";
    BmcSessionTokenParser token_parser = nullptr;
    std::string token_cookie_name;
    int max_success_status = 299;
};

class BmcWebSession {
public:
    explicit BmcWebSession(const LoginOptions& options);
    ~BmcWebSession() = default;

    BmcWebSession(BmcWebSession&&) noexcept = default;
    BmcWebSession& operator=(BmcWebSession&&) noexcept = default;
    BmcWebSession(const BmcWebSession&) = delete;
    BmcWebSession& operator=(const BmcWebSession&) = delete;

    StringResponse request(
        http::verb method,
        std::string_view target,
        std::string body,
        std::string_view content_type,
        const std::vector<Header>& extra_headers = {});

    CookieJar& cookies();
    const CookieJar& cookies() const;
    std::string_view session_token() const;
    void set_cookie(std::string name, std::string value);

private:
    friend BmcWebSession login_bmc_web_session(const LoginOptions& options, const BmcLoginProfile& profile);

    void set_session_token(std::string token, std::string_view token_cookie_name);

    Url base_url_;
    CookieJar cookies_;
    std::string session_token_;
    std::unique_ptr<TlsSessionCache> tls_session_cache_;
    HttpsClient client_;
};

BmcWebSession login_bmc_web_session(const LoginOptions& options, const BmcLoginProfile& profile);

} // namespace hitsc

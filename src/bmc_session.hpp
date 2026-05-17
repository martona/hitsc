#pragma once

#include "cookie_jar.hpp"
#include "options.hpp"

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

struct BmcWebSession {
    CookieJar cookies;
    std::string session_token;
};

BmcWebSession login_bmc_web_session(const LoginOptions& options, const BmcLoginProfile& profile);

} // namespace hitsc

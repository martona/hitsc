#include "trace.hpp"

#include "text.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace hitsc {
namespace json = boost::json;
namespace boost_system = boost::system;

namespace {

constexpr std::string_view kRedacted = "[redacted]";
constexpr std::size_t kMaxBodyLogBytes = 4096;

bool contains_any(std::string_view value, const std::vector<std::string_view>& needles)
{
    const std::string lowered = lower_copy(value);
    return std::any_of(needles.begin(), needles.end(), [&](std::string_view needle) {
        return lowered.find(needle) != std::string::npos;
    });
}

bool is_sensitive_name(std::string_view name)
{
    static const std::vector<std::string_view> sensitive_words{
        "authorization",
        "cookie",
        "csrf",
        "password",
        "passwd",
        "pwd",
        "qsession",
        "session",
        "token",
    };

    return contains_any(name, sensitive_words);
}

std::string header_name(const http::field field, std::string_view name)
{
    if (field != http::field::unknown) {
        return std::string(http::to_string(field));
    }

    return std::string(name);
}

std::string sanitized_header_value(const http::field field, std::string_view name, std::string_view value)
{
    if (field == http::field::cookie ||
        field == http::field::set_cookie ||
        field == http::field::authorization ||
        field == http::field::proxy_authorization ||
        is_sensitive_name(name)) {
        return std::string(kRedacted);
    }

    return std::string(value);
}

std::vector<std::string_view> split(std::string_view value, char delimiter)
{
    std::vector<std::string_view> parts;
    while (true) {
        const auto delimiter_index = value.find(delimiter);
        parts.push_back(value.substr(0, delimiter_index));
        if (delimiter_index == std::string_view::npos) {
            break;
        }
        value.remove_prefix(delimiter_index + 1);
    }
    return parts;
}

std::string sanitize_form_body(std::string_view body)
{
    std::ostringstream out;
    bool first = true;
    for (const std::string_view part : split(body, '&')) {
        if (!first) {
            out << '&';
        }
        first = false;

        const auto equals = part.find('=');
        const std::string_view key = equals == std::string_view::npos ? part : part.substr(0, equals);
        if (is_sensitive_name(key)) {
            out << key << '=' << kRedacted;
        } else {
            out << part;
        }
    }

    return out.str();
}

json::value sanitize_json_value(const json::value& value)
{
    if (const auto* object = value.if_object()) {
        json::object sanitized;
        for (const auto& [key, child] : *object) {
            if (is_sensitive_name(key)) {
                sanitized[key] = kRedacted;
            } else {
                sanitized[key] = sanitize_json_value(child);
            }
        }
        return sanitized;
    }

    if (const auto* array = value.if_array()) {
        json::array sanitized;
        for (const auto& child : *array) {
            sanitized.push_back(sanitize_json_value(child));
        }
        return sanitized;
    }

    return value;
}

std::string sanitize_json_body(std::string_view body)
{
    boost_system::error_code error;
    json::value parsed = json::parse(body, error);
    if (error) {
        return std::string(body);
    }

    return json::serialize(sanitize_json_value(parsed));
}

std::string truncate_body(std::string body)
{
    if (body.size() <= kMaxBodyLogBytes) {
        return body;
    }

    body.resize(kMaxBodyLogBytes);
    body += "...";
    return body;
}

std::string content_type_of(const http::fields& fields)
{
    const auto content_type = fields.find(http::field::content_type);
    if (content_type == fields.end()) {
        return {};
    }
    return lower_copy(content_type->value());
}

std::string sanitize_body(std::string_view body, std::string_view content_type)
{
    if (body.empty()) {
        return {};
    }

    const std::string lowered_content_type = lower_copy(content_type);
    if (lowered_content_type.find("application/x-www-form-urlencoded") != std::string::npos) {
        return truncate_body(sanitize_form_body(body));
    }
    if (lowered_content_type.find("json") != std::string::npos) {
        return truncate_body(sanitize_json_body(body));
    }

    return truncate_body(std::string(body));
}

void log_prefixed(std::string_view prefix, std::string_view text)
{
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        const auto length = end == std::string_view::npos ? text.size() - start : end - start;
        std::cerr << prefix << text.substr(start, length) << '\n';
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
}

void log_headers(const http::fields& fields, std::string_view prefix)
{
    for (const auto& field : fields) {
        std::cerr << prefix
                  << header_name(field.name(), field.name_string()) << ": "
                  << sanitized_header_value(field.name(), field.name_string(), field.value())
                  << '\n';
    }
}

} // namespace

void log_http_request(const http::request<http::string_body>& request, bool enabled)
{
    if (!enabled) {
        return;
    }

    std::cerr << "hitsc: >>> "
              << request.method_string() << ' '
              << request.target() << " HTTP/1."
              << request.version() % 10 << '\n';
    log_headers(request.base(), "hitsc: >>> ");

    const std::string sanitized_body = sanitize_body(request.body(), content_type_of(request.base()));
    if (!sanitized_body.empty()) {
        std::cerr << "hitsc: >>>\n";
        log_prefixed("hitsc: >>> ", sanitized_body);
    }
}

void log_http_response(const http::response<http::string_body>& response, std::string_view decoded_body, bool enabled)
{
    if (!enabled) {
        return;
    }

    std::cerr << "hitsc: <<< HTTP/1."
              << response.version() % 10 << ' '
              << response.result_int() << ' '
              << response.reason() << '\n';
    log_headers(response.base(), "hitsc: <<< ");

    const std::string sanitized_body = sanitize_body(decoded_body, content_type_of(response.base()));
    if (!sanitized_body.empty()) {
        std::cerr << "hitsc: <<<\n";
        log_prefixed("hitsc: <<< ", sanitized_body);
    }
}

} // namespace hitsc

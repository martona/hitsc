#include "text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace hitsc {

std::string trim_copy(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }

    const auto last = value.find_last_not_of(" \t");
    return std::string(value.substr(first, last - first + 1));
}

std::string lower_copy(std::string_view value)
{
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

std::string read_env(std::string_view name)
{
    std::string variable(name);
    const char* value = std::getenv(variable.c_str());
    if (value == nullptr) {
        return {};
    }
    return value;
}

std::string form_url_encode(std::string_view value)
{
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;

    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded << static_cast<char>(ch);
        } else if (ch == ' ') {
            encoded << '+';
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }

    return encoded.str();
}

std::string body_snippet(std::string_view body)
{
    constexpr std::size_t max_length = 500;
    std::string snippet(body.substr(0, std::min(body.size(), max_length)));
    if (body.size() > max_length) {
        snippet += "...";
    }
    return snippet;
}

} // namespace hitsc

#include "cookie_jar.hpp"

#include "text.hpp"

#include <utility>

namespace hitsc {

void CookieJar::add_set_cookie(std::string_view header)
{
    const auto first_part = header.substr(0, header.find(';'));
    const auto equals = first_part.find('=');
    if (equals == std::string_view::npos || equals == 0) {
        return;
    }

    set(trim_copy(first_part.substr(0, equals)), trim_copy(first_part.substr(equals + 1)));
}

void CookieJar::set(std::string name, std::string value)
{
    if (!name.empty()) {
        cookies_[std::move(name)] = std::move(value);
    }
}

std::string CookieJar::header() const
{
    std::string value;
    for (const auto& [name, cookie_value] : cookies_) {
        if (!value.empty()) {
            value += "; ";
        }
        value += name;
        value += '=';
        value += cookie_value;
    }
    return value;
}

std::size_t CookieJar::size() const
{
    return cookies_.size();
}

} // namespace hitsc

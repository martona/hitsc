#pragma once

#include <map>
#include <string>
#include <string_view>

namespace hitsc {

class CookieJar {
public:
    void add_set_cookie(std::string_view header);
    void set(std::string name, std::string value);

    [[nodiscard]] std::string header() const;
    [[nodiscard]] std::size_t size() const;

private:
    std::map<std::string, std::string> cookies_;
};

} // namespace hitsc

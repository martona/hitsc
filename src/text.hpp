#pragma once

#include <string>
#include <string_view>

namespace hitsc {

std::string trim_copy(std::string_view value);
std::string lower_copy(std::string_view value);
std::string read_env(std::string_view name);
std::string form_url_encode(std::string_view value);
std::string body_snippet(std::string_view body);

} // namespace hitsc

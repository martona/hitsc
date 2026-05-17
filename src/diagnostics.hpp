#pragma once

#include <exception>
#include <iosfwd>
#include <string_view>

namespace hitsc {

void install_exception_handlers();
void print_stack_trace(std::ostream& output, std::string_view context = {}, unsigned frames_to_skip = 0);
void print_exception_with_stack(std::ostream& output, const std::exception& ex, std::string_view context);
void print_current_exception_with_stack(std::ostream& output, std::string_view context);

} // namespace hitsc

#pragma once

#include <boost/log/trivial.hpp>

#include <sstream>
#include <string_view>
#include <utility>

namespace hitsc {

class LogLine {
public:
    explicit LogLine(boost::log::trivial::severity_level severity);
    LogLine(LogLine&& other) noexcept;
    LogLine& operator=(LogLine&& other) noexcept = delete;
    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;
    ~LogLine();

    template <typename Value>
    LogLine& operator<<(Value&& value)
    {
        stream_ << std::forward<Value>(value);
        return *this;
    }

private:
    boost::log::trivial::severity_level severity_;
    std::ostringstream stream_;
    bool active_ = true;
};

void initialize_logging();
void write_log(boost::log::trivial::severity_level severity, std::string_view message);

LogLine log_trace();
LogLine log_debug();
LogLine log_info();
LogLine log_warning();
LogLine log_error();

} // namespace hitsc

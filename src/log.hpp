#pragma once

#include <boost/log/trivial.hpp>

#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// In-memory tail of recent log lines, captured at write_log(). Used by the
// viewer's on-window console to surface connection progress and errors.
struct LogEntry {
    boost::log::trivial::severity_level severity = boost::log::trivial::info;
    std::string text;
};

std::vector<LogEntry> recent_log_lines(std::size_t max_lines);

LogLine log_trace();
LogLine log_debug();
LogLine log_info();
LogLine log_warning();
LogLine log_error();

} // namespace hitsc

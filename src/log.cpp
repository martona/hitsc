#include "log.hpp"

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>

#include <iomanip>
#include <mutex>

namespace hitsc {
namespace logging = boost::log;
namespace expr = boost::log::expressions;
namespace trivial = boost::log::trivial;
namespace posix_time = boost::posix_time;

namespace {

using Logger = logging::sources::severity_logger_mt<trivial::severity_level>;

Logger& logger()
{
    static Logger instance;
    return instance;
}

std::string format_timestamp(const posix_time::ptime& time)
{
    const posix_time::time_duration tod = time.time_of_day();
    std::ostringstream output;
    output << std::setw(2) << std::setfill('0') << tod.hours()
           << ':' << std::setw(2) << std::setfill('0') << tod.minutes()
           << ':' << std::setw(2) << std::setfill('0') << tod.seconds()
           << '.' << std::setw(3) << std::setfill('0')
           << (tod.total_milliseconds() % 1000);
    return output.str();
}

void format_record(const logging::record_view& record, logging::formatting_ostream& output)
{
    const auto timestamp = logging::extract<posix_time::ptime>("TimeStamp", record);
    if (timestamp) {
        output << format_timestamp(timestamp.get()) << ' ';
    }

    output << "hitsc: " << record[expr::smessage];
}

} // namespace

void initialize_logging()
{
    static std::once_flag once;
    std::call_once(once, [] {
        logging::add_common_attributes();
        auto sink = logging::add_console_log(std::clog);
        sink->set_formatter(&format_record);
    });
}

void write_log(trivial::severity_level severity, std::string_view message)
{
    initialize_logging();

    if (message.empty()) {
        BOOST_LOG_SEV(logger(), severity);
        return;
    }

    std::size_t start = 0;
    while (start < message.size()) {
        const std::size_t end = message.find('\n', start);
        const std::string_view line = end == std::string_view::npos
            ? message.substr(start)
            : message.substr(start, end - start);
        if (!line.empty()) {
            BOOST_LOG_SEV(logger(), severity) << line;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
}

LogLine::LogLine(trivial::severity_level severity)
    : severity_(severity)
{
}

LogLine::LogLine(LogLine&& other) noexcept
    : severity_(other.severity_)
    , stream_(std::move(other.stream_))
    , active_(other.active_)
{
    other.active_ = false;
}

LogLine::~LogLine()
{
    if (active_) {
        write_log(severity_, stream_.str());
    }
}

LogLine log_trace()
{
    return LogLine(trivial::trace);
}

LogLine log_debug()
{
    return LogLine(trivial::debug);
}

LogLine log_info()
{
    return LogLine(trivial::info);
}

LogLine log_warning()
{
    return LogLine(trivial::warning);
}

LogLine log_error()
{
    return LogLine(trivial::error);
}

} // namespace hitsc

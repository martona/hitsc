#include "launcher_child_command.hpp"

#include "backends/auto/auto_view.hpp"
#include "backends/aten/aten_view.hpp"
#include "bmc_cold_reset.hpp"
#include "errors.hpp"
#include "launcher_child_protocol.hpp"
#include "backends/megarac/megarac_view.hpp"
#include "parent_liveness_monitor.hpp"
#include "backends/pikvm/pikvm_view.hpp"
#include "url.hpp"

#include <QByteArray>

#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>

namespace hitsc {
namespace {

std::string to_utf8_string(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

ChildSessionLaunchRequest read_launch_request()
{
    std::istreambuf_iterator<char> begin(std::cin);
    std::istreambuf_iterator<char> end;
    const std::string input(begin, end);
    if (input.empty()) {
        throw UserError("empty child session launch request");
    }

    return parse_child_session_launch_request(
        QByteArray(input.data(), static_cast<qsizetype>(input.size())));
}

LoginOptions make_login_options(const ChildSessionLaunchRequest& request)
{
    if (request.credentials.username.trimmed().isEmpty()
        || request.credentials.password.isEmpty()) {
        throw UserError("saved credentials are required to connect from the launcher");
    }

    LoginOptions login;
    login.base_url = parse_https_url(to_utf8_string(request.url));
    login.base_url.target = "/";
    login.username = to_utf8_string(request.credentials.username);
    login.password = to_utf8_string(request.credentials.password);
    login.host_id = to_utf8_string(request.session_id);
    return login;
}

enum class ChildAction {
    Connect,
    ColdReset,
};

int run_child_session(VerbosityOptions verbosity, ChildAction action)
{
    const ChildSessionLaunchRequest request = read_launch_request();
    LoginOptions login = make_login_options(request);
    login.verbose = verbosity.verbose;
    login.vverbose = verbosity.vverbose;

    ParentLivenessMonitor parent_liveness_monitor;
    parent_liveness_monitor.start(request.parent_process_id);

    const QByteArray ready = serialize_child_session_status(request.session_id, QStringLiteral("ready"));
    std::cout.write(ready.constData(), ready.size());
    std::cout.flush();

    if (action == ChildAction::ColdReset) {
        BmcColdResetOptions options;
        options.login = std::move(login);
        switch (request.type) {
        case LauncherHostType::Auto:
            options.backend = ColdResetBackend::Detect;
            break;
        case LauncherHostType::Megarac:
            options.backend = ColdResetBackend::Megarac;
            break;
        case LauncherHostType::Aten:
            options.backend = ColdResetBackend::Aten;
            break;
        case LauncherHostType::Pikvm:
            throw UserError(
                "BMC cold reset is only supported for MegaRAC, ATEN, and auto-detected hosts");
        }
        return run_bmc_cold_reset(options);
    }

    switch (request.type) {
    case LauncherHostType::Auto: {
        AutoViewOptions options;
        options.login = std::move(login);
        run_auto_view(options);
        return EXIT_SUCCESS;
    }
    case LauncherHostType::Megarac: {
        MegaracViewOptions options;
        options.login = std::move(login);
        run_megarac_view(options);
        return EXIT_SUCCESS;
    }
    case LauncherHostType::Aten: {
        AtenViewOptions options;
        options.login = std::move(login);
        run_aten_view(options);
        return EXIT_SUCCESS;
    }
    case LauncherHostType::Pikvm: {
        PikvmViewOptions options;
        options.login = std::move(login);
        run_pikvm_view(options);
        return EXIT_SUCCESS;
    }
    }

    throw UserError("unsupported child session host type");
}

} // namespace

int run_launcher_child(VerbosityOptions verbosity)
{
    return run_child_session(verbosity, ChildAction::Connect);
}

int run_launcher_child_cold_reset(VerbosityOptions verbosity)
{
    return run_child_session(verbosity, ChildAction::ColdReset);
}

} // namespace hitsc

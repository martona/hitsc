#include "cli.hpp"

#include "app_info.hpp"
#include "backends/auto/auto_view.hpp"
#include "backends/aten/aten_view.hpp"
#include "console.hpp"
#include "errors.hpp"
#include "gui/child/launcher_child_command.hpp"
#include "gui/launcher_gui.hpp"
#include "backends/megarac/megarac_view.hpp"
#include "options.hpp"
#include "backends/pikvm/pikvm_view.hpp"
#include "text.hpp"
#include "url.hpp"

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif


namespace hitsc {
namespace {

void fill_default_credentials(LoginOptions& options, const std::string& password_env_name)
{
    if (options.username.empty()) {
        options.username = read_env("HITSC_USERNAME");
    }

    if (options.password.empty() && !password_env_name.empty()) {
        options.password = read_env(password_env_name);
        if (options.password.empty()) {
            throw UserError(
                "password environment variable is empty or not set: " + password_env_name);
        }
    }

    if (options.password.empty()) {
        options.password = read_env("HITSC_PASSWORD");
    }

    if (options.username.empty()) {
        throw UserError("missing username; pass --username or set HITSC_USERNAME");
    }
    if (options.password.empty()) {
        options.password = read_password_from_console("Password for " + options.username + ": ");
    }
    if (options.password.empty()) {
        throw UserError("empty password entered");
    }
}

void normalize_verbosity(bool& verbose, bool vverbose)
{
    if (vverbose) {
        verbose = true;
    }
}

void normalize_verbosity(LoginOptions& options)
{
    normalize_verbosity(options.verbose, options.vverbose);
}

void normalize_verbosity(VerbosityOptions& options)
{
    normalize_verbosity(options.verbose, options.vverbose);
}

void configure_verbosity_options(CLI::App& command, bool& verbose, bool& vverbose)
{
    command.add_flag("-v,--verbose", verbose, "Log HTTP request/response and protocol details to stderr.");
    command.add_flag(
        "--vverbose",
        vverbose,
        "Also log noisy input/output events, packets, websocket messages, and frames; implies --verbose.");
}

void configure_login_options(CLI::App& command, LoginOptions& options, std::string& password_env_name)
{
    command.add_flag("-k,--insecure", options.insecure, "Disable certificate and hostname verification.");
    configure_verbosity_options(command, options.verbose, options.vverbose);
    command.add_flag(
        "--debug-disable-http-keepalive",
        options.debug_disable_http_keepalive,
        "Close BMC HTTP TLS connections after each response to exercise TLS session resumption.");
    command.add_option("-u,--username", options.username, "Username for the BMC web login.");
    command.add_option("-p,--password", options.password, "Password for the BMC web login.");
    command.add_option("--password-env", password_env_name, "Read the password from an environment variable.");
}

void configure_view_options(
    CLI::App& command,
    LoginOptions& login,
    int& idle_timeout_seconds,
    std::string& password_env_name)
{
    configure_login_options(command, login, password_env_name);
    command
        .add_option(
            "--idle-timeout",
            idle_timeout_seconds,
            "Stop if no WebSocket message arrives for this many seconds; 0 disables it.")
        ->check(CLI::NonNegativeNumber);
}

} // namespace

int run_cli(int argc, char* argv[])
{
    CLI::App app{std::string(kExpansion), std::string(kName)};
    app.set_version_flag("--version", std::string(kName) + " " + std::string(kVersion));
    app.require_subcommand(1);

    CLI::App* gui = app.add_subcommand("gui", "Open the saved-host launcher.");
    CLI::App* child = app.add_subcommand("child", "Run a launcher child session.");
    VerbosityOptions process_verbosity;
    configure_verbosity_options(*gui, process_verbosity.verbose, process_verbosity.vverbose);
    configure_verbosity_options(*child, process_verbosity.verbose, process_verbosity.vverbose);

    AutoViewOptions auto_options;
    std::string auto_url;
    std::string auto_password_env_name;
    bool auto_pikvm_software_decode = false;
    bool auto_aten_exclusive = false;
    CLI::App* auto_command =
        app.add_subcommand("auto", "Auto-detect the KVM type and open an iKVM window.");
    configure_view_options(
        *auto_command,
        auto_options.login,
        auto_options.idle_timeout_seconds,
        auto_password_env_name);
    auto_command->add_flag(
        "--exclusive",
        auto_aten_exclusive,
        "Request an exclusive ATEN RFB session if Auto detects ATEN.");
    auto_command->add_flag(
        "--software",
        auto_pikvm_software_decode,
        "Use software video decoding if Auto detects PiKVM.");
    auto_command->add_option("url", auto_url, "https://host[:port]")->required();

    MegaracViewOptions megarac_options;
    std::string megarac_url;
    std::string megarac_password_env_name;
    CLI::App* megarac =
        app.add_subcommand("megarac", "Open a MegaRAC iKVM window.");
    configure_view_options(
        *megarac,
        megarac_options.login,
        megarac_options.idle_timeout_seconds,
        megarac_password_env_name);
    megarac->add_option("url", megarac_url, "https://host[:port]")->required();

    AtenViewOptions aten_options;
    std::string aten_url;
    std::string aten_password_env_name;
    bool aten_exclusive = false;
    CLI::App* aten =
        app.add_subcommand("aten", "Open an ATEN/Supermicro iKVM window.");
    configure_view_options(
        *aten,
        aten_options.login,
        aten_options.idle_timeout_seconds,
        aten_password_env_name);
    aten->add_flag("--exclusive", aten_exclusive, "Request an exclusive RFB session.");
    aten->add_option("url", aten_url, "https://host[:port]")->required();

    PikvmViewOptions pikvm_options;
    std::string pikvm_url;
    std::string pikvm_password_env_name;
    bool pikvm_software_decode = false;
    CLI::App* pikvm =
        app.add_subcommand("pikvm", "Open a PiKVM H.264 video and input window.");
    configure_view_options(
        *pikvm,
        pikvm_options.login,
        pikvm_options.idle_timeout_seconds,
        pikvm_password_env_name);
    pikvm->add_flag(
        "--software",
        pikvm_software_decode,
        "Use software video decoding.");
    pikvm->add_option("url", pikvm_url, "https://host[:port]")->required();

    if (argc == 1) {
        std::cout << app.help();
        return EXIT_SUCCESS;
    }

    if (argc > 1) {
        const std::string command = argv[1];
        if (!command.empty()
            && command.front() != '-'
            && command != "megarac"
            && command != "aten"
            && command != "pikvm"
            && command != "auto"
            && command != "gui"
            && command != "child") {
            std::cerr << "Unknown subcommand: " << command << "\n";
            std::cerr << "Run with --help for more information.\n";
            return EXIT_FAILURE;
        }
    }

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }

    if (*auto_command) {
        normalize_verbosity(auto_options.login);
        auto_options.login.base_url = parse_https_url(auto_url);
        auto_options.login.base_url.target = "/";
        auto_options.aten_shared = !auto_aten_exclusive;
        auto_options.pikvm_video_decode = auto_pikvm_software_decode
            ? PikvmVideoDecodeMode::software
            : PikvmVideoDecodeMode::auto_select;
        fill_default_credentials(auto_options.login, auto_password_env_name);

        run_auto_view(auto_options);
        return EXIT_SUCCESS;
    }

    if (*megarac) {
        normalize_verbosity(megarac_options.login);
        megarac_options.login.base_url = parse_https_url(megarac_url);
        megarac_options.login.base_url.target = "/";
        fill_default_credentials(megarac_options.login, megarac_password_env_name);

        run_megarac_view(megarac_options);
        return EXIT_SUCCESS;
    }

    if (*aten) {
        normalize_verbosity(aten_options.login);
        aten_options.login.base_url = parse_https_url(aten_url);
        aten_options.login.base_url.target = "/";
        aten_options.shared = !aten_exclusive;
        fill_default_credentials(aten_options.login, aten_password_env_name);

        run_aten_view(aten_options);
        return EXIT_SUCCESS;
    }

    if (*pikvm) {
        normalize_verbosity(pikvm_options.login);
        pikvm_options.login.base_url = parse_https_url(pikvm_url);
        pikvm_options.login.base_url.target = "/";
        pikvm_options.video_decode = pikvm_software_decode
            ? PikvmVideoDecodeMode::software
            : PikvmVideoDecodeMode::auto_select;
        fill_default_credentials(pikvm_options.login, pikvm_password_env_name);

        run_pikvm_view(pikvm_options);
        return EXIT_SUCCESS;
    }

    if (*child) {
        normalize_verbosity(process_verbosity);
        return run_launcher_child(process_verbosity);
    }

    if (*gui) {
        normalize_verbosity(process_verbosity);
        int result = run_launcher_gui(argc, argv, process_verbosity);
    #ifdef _WIN32
        // Fuck. QT. Sideways.
        // QObject::~QObject: Timers cannot be stopped from another thread
        // The above warning is thrown when exiting; it's coming from a QQuickPixmapCache
        // destructor, so Qt Quick Controls/Fusion internals. Fucking piece of shit.
        // I'm not leaving that on the console, engine.clearComponentCache() doesn't
        // help, so a blunt instrument it is:
        ExitProcess(result);
    #else
        return result;
    #endif
    }

    return EXIT_SUCCESS;
}

} // namespace hitsc

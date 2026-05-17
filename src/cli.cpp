#include "cli.hpp"

#include "app_info.hpp"
#include "aten_probe.hpp"
#include "aten_view.hpp"
#include "console.hpp"
#include "megarac_capture_decode.hpp"
#include "megarac_probe.hpp"
#include "megarac_view.hpp"
#include "megarac_session.hpp"
#include "options.hpp"
#include "text.hpp"
#include "url.hpp"

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

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
            throw std::invalid_argument(
                "password environment variable is empty or not set: " + password_env_name);
        }
    }

    if (options.password.empty()) {
        options.password = read_env("HITSC_PASSWORD");
    }

    if (options.username.empty()) {
        throw std::invalid_argument("missing username; pass --username or set HITSC_USERNAME");
    }
    if (options.password.empty()) {
        options.password = read_password_from_console("Password for " + options.username + ": ");
    }
    if (options.password.empty()) {
        throw std::invalid_argument("empty password entered");
    }
}

void configure_login_subcommand(CLI::App& command, LoginOptions& options, std::string& password_env_name)
{
    command.add_flag("-k,--insecure", options.insecure, "Disable certificate and hostname verification.");
    command.add_flag("-v,--verbose", options.verbose, "Log HTTP request/response details to stderr.");
    command.add_option("-u,--username", options.username, "Username for the BMC web login.");
    command.add_option("-p,--password", options.password, "Password for the BMC web login.");
    command.add_option("--password-env", password_env_name, "Read the password from an environment variable.");
}

void configure_megarac_probe_subcommand(
    CLI::App& command,
    MegaracProbeOptions& options,
    std::string& password_env_name)
{
    configure_login_subcommand(command, options.login, password_env_name);
    command.add_option("--capture,--dump", options.capture_path, "Write a binary KVM capture file for offline replay.");
    command
        .add_option("--packets", options.packet_limit, "Stop after this many KVM protocol packets; 0 keeps reading.")
        ->check(CLI::NonNegativeNumber);
    command
        .add_option("--idle-timeout", options.idle_timeout_seconds, "Stop if no WebSocket message arrives for this many seconds.")
        ->check(CLI::PositiveNumber);
}

} // namespace

int run_cli(int argc, char* argv[])
{
    CLI::App app{std::string(kExpansion), std::string(kName)};
    app.set_version_flag("--version", std::string(kName) + " " + std::string(kVersion));
    app.require_subcommand(1);

    LoginOptions megarac_login_options;
    std::string megarac_login_url;
    std::string megarac_login_password_env_name;
    CLI::App* megarac_login =
        app.add_subcommand("megarac-login", "Log in to a MegaRAC BMC web session.");
    configure_login_subcommand(*megarac_login, megarac_login_options, megarac_login_password_env_name);
    megarac_login->add_option("url", megarac_login_url, "https://host[:port]")->required();

    MegaracProbeOptions megarac_probe_options;
    std::string megarac_probe_url;
    std::string megarac_probe_password_env_name;
    CLI::App* megarac_probe = app.add_subcommand(
        "megarac-probe",
        "Connect to MegaRAC H5Viewer /kvm and log protocol packets.");
    configure_megarac_probe_subcommand(*megarac_probe, megarac_probe_options, megarac_probe_password_env_name);
    megarac_probe->add_option("url", megarac_probe_url, "https://host[:port]")->required();

    MegaracViewOptions megarac_view_options;
    std::string megarac_view_url;
    std::string megarac_view_password_env_name;
    CLI::App* megarac_view =
        app.add_subcommand("megarac-view", "Open a live SDL viewer for MegaRAC H5Viewer /kvm.");
    configure_login_subcommand(*megarac_view, megarac_view_options.login, megarac_view_password_env_name);
    megarac_view
        ->add_option("--idle-timeout", megarac_view_options.idle_timeout_seconds, "Stop if no WebSocket message arrives for this many seconds; 0 disables it.")
        ->check(CLI::NonNegativeNumber);
    megarac_view->add_option("url", megarac_view_url, "https://host[:port]")->required();

    MegaracCaptureDecodeOptions megarac_decode_options;
    CLI::App* megarac_decode_capture =
        app.add_subcommand("megarac-decode-capture", "Decode a hitsc MegaRAC KVM capture into BMP frames.");
    megarac_decode_capture
        ->add_option("input", megarac_decode_options.input_path, "Input .hkv capture file.")
        ->required();
    megarac_decode_capture
        ->add_option("output-dir", megarac_decode_options.output_directory, "Directory for decoded BMP frames.")
        ->required();
    megarac_decode_capture
        ->add_option("--frames", megarac_decode_options.frame_limit, "Stop after this many complete frames; 0 decodes all frames.")
        ->check(CLI::NonNegativeNumber);

    AtenProbeOptions aten_options;
    std::string aten_url;
    std::string aten_password_env_name;
    bool aten_skip_bootstrap = false;
    CLI::App* aten_probe =
        app.add_subcommand("aten-probe", "Log in to ATEN/Supermicro HTML5 iKVM and read the RFB WebSocket greeting.");
    configure_login_subcommand(*aten_probe, aten_options.login, aten_password_env_name);
    aten_probe
        ->add_option("--idle-timeout", aten_options.idle_timeout_seconds, "Stop if no WebSocket message arrives for this many seconds.")
        ->check(CLI::PositiveNumber);
    aten_probe
        ->add_option("--websocket-path", aten_options.websocket_path, "ATEN iKVM WebSocket path.")
        ->default_str("/");
    aten_probe->add_flag("--skip-bootstrap", aten_skip_bootstrap, "Skip the iKVM bootstrap GET before opening the WebSocket.");
    aten_probe->add_option("url", aten_url, "https://host[:port]")->required();

    AtenViewOptions aten_view_options;
    std::string aten_view_url;
    std::string aten_view_password_env_name;
    bool aten_view_skip_bootstrap = false;
    bool aten_view_exclusive = false;
    CLI::App* aten_view = app.add_subcommand(
        "aten-view",
        "Open an ATEN/Supermicro RFB-over-WebSocket KVM window.");
    configure_login_subcommand(*aten_view, aten_view_options.login, aten_view_password_env_name);
    aten_view
        ->add_option("--idle-timeout", aten_view_options.idle_timeout_seconds, "Stop if no WebSocket message arrives for this many seconds.")
        ->check(CLI::PositiveNumber);
    aten_view
        ->add_option("--websocket-path", aten_view_options.websocket_path, "ATEN iKVM WebSocket path.")
        ->default_str("/");
    aten_view
        ->add_option("--updates", aten_view_options.framebuffer_update_limit, "Stop after this many framebuffer updates; 0 keeps reading.")
        ->check(CLI::NonNegativeNumber);
    aten_view->add_flag("--exclusive", aten_view_exclusive, "Request an exclusive RFB session.");
    aten_view->add_flag("--skip-bootstrap", aten_view_skip_bootstrap, "Skip the iKVM bootstrap GET before opening the WebSocket.");
    aten_view->add_option("url", aten_view_url, "https://host[:port]")->required();

    if (argc == 1) {
        std::cout << app.help();
        return EXIT_SUCCESS;
    }

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }

    if (*megarac_login) {
        megarac_login_options.base_url = parse_https_url(megarac_login_url);
        megarac_login_options.base_url.target = "/";
        fill_default_credentials(megarac_login_options, megarac_login_password_env_name);

        MegaRacSession session = login_megarac(megarac_login_options);
        MegaRacLogoutGuard logout_guard(megarac_login_options);
        logout_guard.arm(session);
        std::cout << "hitsc: megarac login succeeded\n";
        std::cout << "hitsc: cookies stored: " << session.cookies.size() << '\n';
        std::cout << "hitsc: csrf token: " << (session.csrf_token.empty() ? "not present" : "present") << '\n';
        return EXIT_SUCCESS;
    }

    if (*megarac_probe) {
        megarac_probe_options.login.base_url = parse_https_url(megarac_probe_url);
        megarac_probe_options.login.base_url.target = "/";
        fill_default_credentials(megarac_probe_options.login, megarac_probe_password_env_name);

        run_megarac_probe(megarac_probe_options);
        return EXIT_SUCCESS;
    }

    if (*megarac_view) {
        megarac_view_options.login.base_url = parse_https_url(megarac_view_url);
        megarac_view_options.login.base_url.target = "/";
        fill_default_credentials(megarac_view_options.login, megarac_view_password_env_name);

        run_megarac_view(megarac_view_options);
        return EXIT_SUCCESS;
    }

    if (*megarac_decode_capture) {
        decode_megarac_capture(megarac_decode_options);
        return EXIT_SUCCESS;
    }

    if (*aten_probe) {
        aten_options.login.base_url = parse_https_url(aten_url);
        aten_options.login.base_url.target = "/";
        aten_options.fetch_bootstrap = !aten_skip_bootstrap;
        fill_default_credentials(aten_options.login, aten_password_env_name);

        run_aten_probe(aten_options);
        return EXIT_SUCCESS;
    }

    if (*aten_view) {
        aten_view_options.login.base_url = parse_https_url(aten_view_url);
        aten_view_options.login.base_url.target = "/";
        aten_view_options.fetch_bootstrap = !aten_view_skip_bootstrap;
        aten_view_options.shared = !aten_view_exclusive;
        fill_default_credentials(aten_view_options.login, aten_view_password_env_name);

        run_aten_view(aten_view_options);
        return EXIT_SUCCESS;
    }

    return EXIT_SUCCESS;
}

} // namespace hitsc

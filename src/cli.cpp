#include "cli.hpp"

#include "app_info.hpp"
#include "console.hpp"
#include "kvm_capture_decode.hpp"
#include "kvm_probe.hpp"
#include "kvm_view.hpp"
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
    command.add_option("-u,--username", options.username, "Username for the MegaRAC /api/session login.");
    command.add_option("-p,--password", options.password, "Password for the MegaRAC /api/session login.");
    command.add_option("--password-env", password_env_name, "Read the password from an environment variable.");
}

void configure_kvm_probe_subcommand(
    CLI::App& command,
    KvmProbeOptions& options,
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

    LoginOptions login_options;
    std::string login_url;
    std::string password_env_name;
    CLI::App* login = app.add_subcommand("login", "Log in to a MegaRAC BMC web session.");
    configure_login_subcommand(*login, login_options, password_env_name);
    login->add_option("url", login_url, "https://host[:port]")->required();

    KvmProbeOptions kvm_probe_options;
    std::string kvm_probe_url;
    std::string kvm_probe_password_env_name;
    CLI::App* kvm_probe = app.add_subcommand("kvm-probe", "Connect to MegaRAC H5Viewer /kvm and log protocol packets.");
    configure_kvm_probe_subcommand(*kvm_probe, kvm_probe_options, kvm_probe_password_env_name);
    kvm_probe->add_option("url", kvm_probe_url, "https://host[:port]")->required();

    KvmViewOptions view_options;
    std::string view_url;
    std::string view_password_env_name;
    CLI::App* view = app.add_subcommand("view", "Open a live SDL viewer for MegaRAC H5Viewer /kvm.");
    configure_login_subcommand(*view, view_options.login, view_password_env_name);
    view
        ->add_option("--idle-timeout", view_options.idle_timeout_seconds, "Stop if no WebSocket message arrives for this many seconds.")
        ->check(CLI::PositiveNumber);
    view->add_option("url", view_url, "https://host[:port]")->required();

    KvmCaptureDecodeOptions decode_options;
    CLI::App* decode_capture =
        app.add_subcommand("decode-capture", "Decode a hitsc KVM capture into BMP frames.");
    decode_capture->add_option("input", decode_options.input_path, "Input .hkv capture file.")->required();
    decode_capture->add_option("output-dir", decode_options.output_directory, "Directory for decoded BMP frames.")->required();
    decode_capture
        ->add_option("--frames", decode_options.frame_limit, "Stop after this many complete frames; 0 decodes all frames.")
        ->check(CLI::NonNegativeNumber);

    if (argc == 1) {
        std::cout << app.help();
        return EXIT_SUCCESS;
    }

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return app.exit(error);
    }

    if (*login) {
        login_options.base_url = parse_https_url(login_url);
        login_options.base_url.target = "/";
        fill_default_credentials(login_options, password_env_name);

        const MegaRacSession session = login_megarac(login_options);
        std::cout << "hitsc: megarac login succeeded\n";
        std::cout << "hitsc: cookies stored: " << session.cookies.size() << '\n';
        std::cout << "hitsc: csrf token: " << (session.csrf_token.empty() ? "not present" : "present") << '\n';
        return EXIT_SUCCESS;
    }

    if (*kvm_probe) {
        kvm_probe_options.login.base_url = parse_https_url(kvm_probe_url);
        kvm_probe_options.login.base_url.target = "/";
        fill_default_credentials(kvm_probe_options.login, kvm_probe_password_env_name);

        run_kvm_probe(kvm_probe_options);
        return EXIT_SUCCESS;
    }

    if (*view) {
        view_options.login.base_url = parse_https_url(view_url);
        view_options.login.base_url.target = "/";
        fill_default_credentials(view_options.login, view_password_env_name);

        run_kvm_view(view_options);
        return EXIT_SUCCESS;
    }

    if (*decode_capture) {
        decode_kvm_capture(decode_options);
        return EXIT_SUCCESS;
    }

    return EXIT_SUCCESS;
}

} // namespace hitsc

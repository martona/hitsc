#include "cli.hpp"
#include "diagnostics.hpp"
#include "errors.hpp"
#include "log.hpp"

#include <cstdlib>
#include <iostream>

#ifdef _WIN32
namespace {

constexpr char kConsoleShimEnvironmentName[] = "HITSC_CONSOLE_SHIM";

bool consume_console_shim_marker()
{
    const bool started_through_shim = std::getenv(kConsoleShimEnvironmentName) != nullptr;
    _putenv_s(kConsoleShimEnvironmentName, "");
    return started_through_shim;
}

} // namespace
#endif

int main(int argc, char* argv[])
{
    hitsc::initialize_logging();
    hitsc::install_exception_handlers();

    try {
#ifdef _WIN32
        const bool started_through_shim = consume_console_shim_marker();
        if (argc == 1 && !started_through_shim) {
            static char fallback_program_name[] = "hitsc";
            static char gui_command[] = "gui";
            char* gui_argv[] = {argc > 0 ? argv[0] : fallback_program_name, gui_command};
            return hitsc::run_cli(2, gui_argv);
        }
#endif
        return hitsc::run_cli(argc, argv);
    } catch (const hitsc::UserError& ex) {
        std::cerr << "hitsc: " << ex.what() << "\n";
        return EXIT_FAILURE;
    } catch (const std::exception& ex) {
        hitsc::print_exception_with_stack(std::cerr, ex, "main");
        std::cerr << "Try 'hitsc --help'.\n";
        return EXIT_FAILURE;
    } catch (...) {
        hitsc::print_current_exception_with_stack(std::cerr, "main");
        std::cerr << "Try 'hitsc --help'.\n";
        return EXIT_FAILURE;
    }
}

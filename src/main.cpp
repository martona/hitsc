#include "cli.hpp"
#include "diagnostics.hpp"
#include "log.hpp"

#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[])
{
    hitsc::initialize_logging();
    hitsc::install_exception_handlers();

    try {
        return hitsc::run_cli(argc, argv);
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

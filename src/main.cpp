#include "cli.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

int main(int argc, char* argv[])
{
    try {
        return hitsc::run_cli(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "hitsc: " << ex.what() << '\n';
        std::cerr << "Try 'hitsc --help'.\n";
        return EXIT_FAILURE;
    }
}

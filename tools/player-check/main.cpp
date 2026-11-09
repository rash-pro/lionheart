#include "disc.h"
#include <iostream>
int main(int argc, char **argv)
{
    try {
        std::vector<std::string> args(argv + 1, argv + argc);
        if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
            std::cout << "usage: check_player_binary BINARY EXE [--disc CUE] [--compile-commands PATH]\n"
                         "       [--generated-dir PATH] [--extract-dir PATH]\n";
            return 0;
        }
        auto root = player_check::fs::path(__FILE__).parent_path().parent_path().parent_path();
        std::cout << player_check::scan_cli(args, player_check::disc_extraction(root), root).dump() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "check_player_binary: " << error.what() << '\n';
        return 1;
    }
}

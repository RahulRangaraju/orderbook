// gen: write N synthetic commands in our CSV format.
//
//   ./build/gen 1000000 > data/gen_1m.csv
//   ./build/gen 1000000 7 > data/gen_1m_seed7.csv     # different seed
//
// ~70% adds, 25% cancels, 5% modifies around a random-walking mid price.
// Same seed -> same file, every time.
#include "ob/generator.hpp"
#include "ob/parser.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gen <count> [seed]\n");
        return 2;
    }
    const std::size_t n = std::strtoull(argv[1], nullptr, 10);
    ob::GenConfig cfg;
    if (argc > 2) cfg.seed = std::strtoull(argv[2], nullptr, 10);

    ob::Generator gen(cfg);
    std::string buf;
    buf.reserve(1 << 20);
    buf += "# type,id,side,price,qty\n";
    for (std::size_t i = 0; i < n; ++i) {
        buf += ob::formatCommand(gen.next());
        buf += '\n';
        if (buf.size() > (1 << 20) - 64) {
            std::fwrite(buf.data(), 1, buf.size(), stdout);
            buf.clear();
        }
    }
    std::fwrite(buf.data(), 1, buf.size(), stdout);
    return 0;
}

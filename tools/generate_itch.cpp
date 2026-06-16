#include <iostream>
#include <fstream>
#include <random>
#include <cstring>

#pragma pack(push, 1)
struct AddMsg {
    uint64_t timestamp;
    uint64_t order_id;
    char symbol[8];
    char side;
    uint64_t price;
    uint64_t quantity;
};
struct MarketMsg {
    uint64_t timestamp;
    uint64_t order_id;
    char symbol[8];
    char side;
    uint64_t quantity;
};
#pragma pack(pop)

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: generate_itch <output_file> <num_events>\n";
        return 1;
    }
    std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
    size_t num = std::stoull(argv[2]);
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint64_t> price_dist(90000, 110000);
    std::uniform_int_distribution<uint64_t> qty_dist(1, 100);
    std::uniform_int_distribution<int> side_dist(0,1);
    uint64_t ts = 1'000'000'000;
    uint64_t next_id = 1;
    char sym[8] = "TEST   ";

    for (size_t i = 0; i < num; ++i) {
        char type = (i > 10 && rng() % 5 == 0) ? 'F' : 'A';
        ts += rng() % 1000 + 1;
        if (type == 'A') {
            AddMsg m;
            m.timestamp = ts;
            m.order_id = next_id++;
            memcpy(m.symbol, sym, 8);
            m.side = side_dist(rng) ? 'B' : 'S';
            m.price = price_dist(rng);
            m.quantity = qty_dist(rng);
            out.write("A", 1);
            out.write(reinterpret_cast<const char*>(&m), sizeof(m));
        } else {
            MarketMsg m;
            m.timestamp = ts;
            m.order_id = next_id++;
            memcpy(m.symbol, sym, 8);
            m.side = side_dist(rng) ? 'B' : 'S';
            m.quantity = qty_dist(rng);
            out.write("F", 1);
            out.write(reinterpret_cast<const char*>(&m), sizeof(m));
        }
    }
    out.close();
    std::cout << "Generated " << num << " events.\n";
    return 0;
}

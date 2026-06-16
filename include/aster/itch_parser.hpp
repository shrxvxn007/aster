#pragma once

#include "aster/types.hpp"
#include <vector>
#include <string>

namespace aster::itch {

struct RawMessage {
    char type;
    std::vector<char> data;
};

std::vector<RawMessage> parse_raw(const std::string& path);
std::vector<OrderEvent> build_event_sequence(const std::string& path,
                                              Timestamp& out_start_ts);

} // namespace aster::itch

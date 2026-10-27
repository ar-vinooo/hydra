// Parses Nintendo-style TZif blobs (64-bit single block + POSIX footer) with
// the production parser and dumps the resulting TimeZoneRule for
// tests/timezone_reference.py.
//   hydra-timezone-tests <blobs.txt> <out.txt>     one hex blob per input line
// Output per blob: header line, then ats, type indices, type infos, chars (hex).
#include "core/horizon/services/timesrv/internal/tzif.hpp"
#include <fstream>
#include <iostream>
#include <sstream>

using namespace hydra;
using namespace hydra::horizon::services::timesrv;

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: blobs.txt out.txt\n";
        return 2;
    }
    std::ifstream input(argv[1]);
    std::ofstream output(argv[2]);
    std::string line;
    while (std::getline(input, line)) {
        std::vector<u8> bytes;
        for (usize i = 0; i + 1 < line.size(); i += 2)
            bytes.push_back(static_cast<u8>(std::stoul(line.substr(i, 2), nullptr, 16)));
        ztd::io::MemoryStream stream(bytes);
        auto rule = std::make_unique<TimeZoneRule>();
        std::memset(rule.get(), 0, sizeof(TimeZoneRule));
        internal::parseTimeZoneBinary(&stream, *rule);
        output << rule->time_count << ' ' << rule->type_count << ' ' << rule->char_count
               << ' ' << rule->default_type << ' ' << rule->go_back << ' ' << rule->go_ahead << '\n';
        for (u32 i = 0; i < rule->time_count; ++i) output << rule->ats[i] << ' ';
        output << '\n';
        for (u32 i = 0; i < rule->time_count; ++i) output << u32(rule->type_indices[i]) << ' ';
        output << '\n';
        for (u32 i = 0; i < rule->type_count; ++i)
            output << rule->type_infos[i].gmt_offset << ',' << rule->type_infos[i].is_day_saving_time
                   << ',' << rule->type_infos[i].abbreviation_list_index << ' ';
        output << '\n';
        for (u32 i = 0; i < rule->char_count; ++i) {
            char buffer[4];
            snprintf(buffer, sizeof(buffer), "%02x", static_cast<u8>(rule->chars[i]));
            output << buffer;
        }
        output << '\n';
    }
}

#pragma once
#include <cstdint>
// HW 8, part 2 — fast uint64 -> decimal text. Write the digits into out; return the length.
inline int u64toa(std::uint64_t v, char* out) {
    (void)v; (void)out;
    return 0;   // TODO(student): beat snprintf/std::to_string; handle 0 and UINT64_MAX
}

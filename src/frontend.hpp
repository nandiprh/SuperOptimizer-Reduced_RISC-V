#pragma once

#include "isa.hpp"
#include "testgen.hpp"
#include <string>

namespace sopt {

// Toplevel frontend entry: parse a small C-like expression program
//   "t3 = (a0 + a1) - a1 << 1;"  or multiple statements "t1 = a0+a1; t2 = t1<<1;"
// into a ProgramSpec (code + derived live_in/live_out).
// Returns false with a human-readable error in `err` on failure.
bool parse_expression_program(const std::string& text, ProgramSpec& out, std::string& err);

} // namespace sopt
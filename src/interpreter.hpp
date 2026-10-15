#pragma once

#include "isa.hpp"
#include <array>
#include <cstdint>

namespace sopt {

using State = std::array<uint32_t, 32>;

uint32_t run_instruction(const Insn& insn, State& state);
void run_program(const Insn* prog, int n, State& state);

} // namespace sopt

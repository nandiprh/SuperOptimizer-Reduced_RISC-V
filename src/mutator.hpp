#pragma once

#include "isa.hpp"
#include <vector>

namespace sopt {

struct ProgramSpec;

void mutator_init_random(unsigned int s);
Insn random_insn(const ProgramSpec& spec);
Insn random_insn_same_fmt(const ProgramSpec& spec, Fmt fmt);
void mutate_program(const ProgramSpec& spec, Insn* buf, int len, double* move_probs = nullptr);

} // namespace sopt

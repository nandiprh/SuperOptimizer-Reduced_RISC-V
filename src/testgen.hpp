#pragma once

#include "isa.hpp"
#include <vector>
#include <array>
#include <cstdint>

namespace sopt {

struct TestCase {
  std::array<uint32_t, 32> in;
  std::array<uint32_t, 32> out_target;
};

struct ProgramSpec {
  std::vector<Insn> code;
  std::vector<int> live_in;
  std::vector<int> live_out;
  std::vector<int> temps;
  int max_len;
};

void init_random_seed(unsigned int s);
uint32_t rand32();
TestCase gen_test_case(const std::vector<Insn>& target_prog, const std::vector<int>& live_in, const std::vector<int>& live_out, int idx, bool fixed_boundaries = true);
std::vector<TestCase> gen_test_set(const std::vector<Insn>& target_prog, const std::vector<int>& live_in, const std::vector<int>& live_out, int count, bool fixed_boundaries = true);

// Verification-oriented: biases live_in toward small signed ints [-2048,2047].
std::vector<TestCase> gen_tight_test_set(const std::vector<Insn>& target_prog, const std::vector<int>& live_in, const std::vector<int>& live_out, int count);

} // namespace sopt

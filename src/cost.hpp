#pragma once

#include "isa.hpp"
#include <vector>
#include <array>
#include <cstdint>

namespace sopt {

struct TestCase;

long long popcount_xor(uint32_t a, uint32_t b);
long long eq_dist(const Insn* prog, int n, const std::vector<TestCase>& tests, const std::vector<int>& live_out);
long long cost_eq_only(const Insn* prog, int n, const std::vector<TestCase>& tests, const std::vector<int>& live_out);
long long perf_insn_count(const Insn* prog, int n);

} // namespace sopt

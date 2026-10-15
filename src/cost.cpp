#include "cost.hpp"
#include "interpreter.hpp"
#include "testgen.hpp"
#include <bit>

namespace sopt {

long long popcount_xor(uint32_t a, uint32_t b) {
  uint32_t x = a ^ b;
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<long long>(__builtin_popcount(x));
#else
  long long c = 0;
  while (x) { c += x & 1; x >>= 1; }
  return c;
#endif
}

long long eq_dist(const Insn* prog, int n, const std::vector<TestCase>& tests, const std::vector<int>& live_out) {
  long long dist = 0;
  for (const auto& tc : tests) {
    State st = tc.in;
    run_program(prog, n, st);
    for (int r : live_out) {
      if (r < 0 || r >= 32) continue;
      dist += popcount_xor(st[r], tc.out_target[r]);
    }
  }
  return dist;
}

long long cost_eq_only(const Insn* prog, int n, const std::vector<TestCase>& tests, const std::vector<int>& live_out) {
  return eq_dist(prog, n, tests, live_out);
}

long long perf_insn_count(const Insn* prog, int n) {
  long long cnt = 0;
  for (int i = 0; i < n; i++) {
    if (!is_nop(prog[i])) cnt++;
  }
  return cnt;
}

} // namespace sopt

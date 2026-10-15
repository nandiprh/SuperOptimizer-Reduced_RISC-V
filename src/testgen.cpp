#include "testgen.hpp"
#include <random>
#include <algorithm>
#include <ctime>
#include <climits>
#include "interpreter.hpp"

namespace sopt {

static std::mt19937* rng = nullptr;

void init_random_seed(unsigned int s) {
  if (rng) delete rng;
  rng = new std::mt19937(s == 0 ? static_cast<unsigned int>(std::time(nullptr)) : s);
}

uint32_t rand32() {
  if (!rng) init_random_seed(0);
  std::uniform_int_distribution<uint32_t> d(0, UINT32_MAX);
  return d(*rng);
}

// 64-entry dense boundary table: 0..31, -1..-16, common immediates, extremes.
// Covers ranges that pure uniform 32-bit random sampling effectively never hits,
// so that small-constant rewrites (e.g. a<1 vs a<16) can still be distinguished.
static uint32_t boundary_val(int k) {
  static const uint32_t tab[64] = {
    // 0..31 small nonneg
    0, 1, 2, 3, 4, 5, 6, 7,
    8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31,
    // small negatives
    0xFFFFFFFFu, 0xFFFFFFF0u, 0xFFFFFFE0u, 0xFFFFFFC0u,
    0xFFFFFF80u, 0xFFFFFF00u, 0xFFFFFE00u, 0xFFFFFC00u,
    // near-extreme immediates
    0xFFFFF800u, 0xFFFF0000u, 0xFF000000u, 0x7FFFF800u,
    // classic bit patterns
    0x80000000u, 0x7FFFFFFFu, 0x55555555u, 0xAAAAAAAAu,
    32, 63, 64, 127,
    255, 511, 1023, 2047,
    4095, 0x00001000u, 0x00000FFFu, 0x3FFFFFFFu,
    0x40000000u, 0x5FFFFFFFu, 0x60000000u, 0x7FFFFFFEu
  };
  return tab[static_cast<unsigned>(k) & 63u];
}

TestCase gen_test_case(const std::vector<Insn>& target_prog, const std::vector<int>& live_in, const std::vector<int>& live_out, int idx, bool fixed_boundaries) {
  TestCase tc;
  // Registers not in live_in are initialized to a fixed random constant per test
  // case so that reading an uninitialized register is detectable (not silently 0).
  for (int i = 0; i < 32; i++) tc.in[i] = rand32();
  tc.out_target.fill(0);
  tc.in[0] = 0; // x0 is always zero
  for (size_t j = 0; j < live_in.size(); j++) {
    int r = live_in[j];
    if (r < 0 || r >= 32) continue;
    if (fixed_boundaries && idx < 128) {
      tc.in[r] = boundary_val((int)j * 3 + idx);
    } else {
      tc.in[r] = rand32();
    }
  }
  {
    State st = tc.in;
    run_program(target_prog.empty() ? nullptr : target_prog.data(), (int)target_prog.size(), st);
    for (int r : live_out) {
      if (r >= 0 && r < 32) tc.out_target[r] = st[r];
    }
  }
  return tc;
}

std::vector<TestCase> gen_test_set(const std::vector<Insn>& target_prog, const std::vector<int>& live_in, const std::vector<int>& live_out, int count, bool fixed_boundaries) {
  std::vector<TestCase> set;
  set.resize(count);
  for (int i = 0; i < count; i++) {
    set[i] = gen_test_case(target_prog, live_in, live_out, i, fixed_boundaries);
  }
  return set;
}

// Verification-oriented test set: biases live_in inputs toward small signed
// integers ([-2048,2047]) so that rewrites differing only on small-constant
// ranges are exposed, while a fraction of cases still uses full 32-bit randoms.
std::vector<TestCase> gen_tight_test_set(const std::vector<Insn>& target_prog,
                                          const std::vector<int>& live_in,
                                          const std::vector<int>& live_out,
                                          int count) {
  std::vector<TestCase> set;
  set.reserve(count);
  std::uniform_int_distribution<int32_t> small(-2048, 2047);
  for (int i = 0; i < count; i++) {
    TestCase tc;
    for (int r = 0; r < 32; r++) tc.in[r] = rand32();
    tc.in[0] = 0;
    for (int r : live_in) {
      if (r < 0 || r >= 32) continue;
      tc.in[r] = (i % 6 == 5) ? rand32() : static_cast<uint32_t>(small(*rng));
    }
    {
      State st = tc.in;
      run_program(target_prog.empty() ? nullptr : target_prog.data(), (int)target_prog.size(), st);
      for (int r : live_out) {
        if (r >= 0 && r < 32) tc.out_target[r] = st[r];
      }
    }
    set.push_back(tc);
  }
  return set;
}

} // namespace sopt

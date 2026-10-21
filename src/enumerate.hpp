#pragma once

#include "isa.hpp"
#include <vector>

namespace sopt {

struct ProgramSpec;
struct TestCase;

struct EnumerateConfig {
  int max_len;              // max program length to try (1..max_len)
  int test_count;           // test cases for filtering
  bool verify_survivors;    // run Z3/probabilistic verification on survivors
};

struct EnumerateResult {
  std::vector<Insn> best_prog;
  long long best_cost;      // 1000*eq + perf of best
  long long best_eq;
  long long best_perf;
  int programs_tested;      // total sequences evaluated
  double time_sec;
};

// Exhaustively try every sequence of length 1..max_len over the register and
// immediate pools. Keep those that pass the test set, then verify survivors.
// Returns the shortest correct program (optimal for the given length cap).
EnumerateResult enumerate_search(const ProgramSpec& spec,
                                 const std::vector<TestCase>& tests,
                                 const EnumerateConfig& cfg);

} // namespace sopt

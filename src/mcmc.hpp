#pragma once

#include "isa.hpp"
#include <vector>
#include <array>
#include <cmath>
#include <cstdint>

namespace sopt {

struct ProgramSpec;
struct TestCase;

struct MCMCConfig {
  int max_iters;
  int chains;
  unsigned int seed;
  double beta;
  int test_count;
  bool use_regression; // legacy flag; cost_mode is authoritative
  // Cost mode for the performance term:
  // 0 = instruction count (default), 1 = regression-predicted cycles,
  // 2 = bundling-simulator cycles (static), 3 = Tomasulo cycles (dynamic).
  int cost_mode;
  // Optional post-search steps
  bool do_enum;            // run enumerative baseline for ground-truth optimum
  int enum_max_len;        // max length for enumerative search
  bool do_z3;              // run Z3 exact verification + counterexample loop
  // Optional starting point for each chain. Empty => start from all NOPs.
  std::vector<Insn> init_prog;
};

struct MCMCResult {
  std::vector<Insn> best_prog;
  long long best_cost;
  long long best_eq;
  long long best_perf;
  double time_sec;
};

MCMCResult run_mcmc(const ProgramSpec& spec, const std::vector<TestCase>& tests, const MCMCConfig& cfg);

} // namespace sopt

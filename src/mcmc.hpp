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
  bool use_regression;
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

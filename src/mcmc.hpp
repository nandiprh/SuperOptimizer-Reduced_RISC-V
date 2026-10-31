#pragma once

#include "isa.hpp"
#include <vector>
#include <array>
#include <cmath>
#include <cstdint>

namespace sopt {

struct ProgramSpec;
struct TestCase;

// Search algorithm used by the MCMC driver.  They all walk a fixed-length
// NOP-filled buffer with the same mutator, but differ in how a proposal is
// accepted:
//   MCMC          - Metropolis-Hastings with a fixed inverse temperature beta
//   ADAPTIVE_MCMC - Metropolis-Hastings whose beta is adapted online so the
//                   observed acceptance rate tracks target_accept
//   HC_MCMC       - greedy hill climbing (only non-worsening moves) with
//                   stagnation-triggered random kicks to escape local minima
enum class SearchAlgo { MCMC, ADAPTIVE_MCMC, HC_MCMC };

const char* search_algo_name(SearchAlgo algo);

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
  bool enum_only;          // use enumerative search as the search algorithm
  int enum_max_len;        // max length for enumerative search
  bool do_z3;              // run Z3 exact verification + counterexample loop
  // Optional starting point for each chain. Empty => start from all NOPs.
  std::vector<Insn> init_prog;

  // --- Search algorithm selection -----------------------------------------
  SearchAlgo algo = SearchAlgo::MCMC;

  // Adaptive MCMC: target acceptance rate and log-space adaptation gain.
  // Every adapt_window proposals, beta <- clamp(beta * exp(gain*(accept-target))).
  double target_accept = 0.234;
  double adapt_gain = 1.0;
  int adapt_window = 200;
  double beta_min = 1e-3;
  double beta_max = 1e3;

  // HC-MCMC: after this many iterations without a new best, kick the chain by
  // re-starting from the incumbent best plus a few random mutations.
  int hc_stall_limit = 1000;
  int hc_kick_mutations = 4;
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

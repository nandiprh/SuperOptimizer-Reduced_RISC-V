#include "mcmc.hpp"
#include "testgen.hpp"
#include "mutator.hpp"
#include "cost.hpp"
#include "interpreter.hpp"
#include <random>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <iostream>
#include <chrono>
#include <cstring>

namespace sopt {

static long long total_cost(const Insn* prog, int n, const std::vector<TestCase>& tests, const std::vector<int>& live_out, bool use_reg) {
  (void)use_reg;
  long long eqv = cost_eq_only(prog, n, tests, live_out);
  long long perv = perf_insn_count(prog, n);
  // simple weighted: eq dominates
  return eqv * 1000LL + perv;
}

MCMCResult run_mcmc(const ProgramSpec& spec, const std::vector<TestCase>& tests, const MCMCConfig& cfg) {
  MCMCResult res;
  res.best_cost = std::numeric_limits<long long>::max();
  res.best_eq = std::numeric_limits<long long>::max();
  res.best_perf = std::numeric_limits<long long>::max();
  res.time_sec = 0.0;

  auto start = std::chrono::high_resolution_clock::now();
  std::mt19937 rng(cfg.seed);
  std::uniform_real_distribution<double> urand(0.0, 1.0);

  int L = spec.max_len > 0 ? spec.max_len : (int)spec.code.size();
  if (L <= 0) L = 1;

  // initialize current program with nops
  std::vector<Insn> cur(L);
  for (int i = 0; i < L; i++) cur[i] = make_nop();

  long long cur_cost = total_cost(cur.data(), L, tests, spec.live_out, cfg.use_regression);
  long long cur_eq = cost_eq_only(cur.data(), L, tests, spec.live_out);
  long long cur_perf = perf_insn_count(cur.data(), L);

  res.best_prog = cur;
  res.best_cost = cur_cost;
  res.best_eq = cur_eq;
  res.best_perf = cur_perf;

  for (int iter = 0; iter < cfg.max_iters; iter++) {
    std::vector<Insn> cand = cur;
    mutate_program(spec, cand.data(), L);
    long long cand_eq = cost_eq_only(cand.data(), L, tests, spec.live_out);
    // early exit not fully implemented in loop, but compute full cost
    long long cand_perf = perf_insn_count(cand.data(), L);
    long long cand_cost = cand_eq * 1000LL + cand_perf;
    double delta = (double)cand_cost - (double)cur_cost;
    bool accept = false;
    if (delta <= 0.0) {
      accept = true;
    } else {
      double prob = std::exp(-cfg.beta * delta);
      if (urand(rng) < prob) accept = true;
    }
    if (accept) {
      cur = cand;
      cur_cost = cand_cost;
      cur_eq = cand_eq;
      cur_perf = cand_perf;
      if (cur_cost < res.best_cost) {
        res.best_prog = cur;
        res.best_cost = cur_cost;
        res.best_eq = cur_eq;
        res.best_perf = cur_perf;
      }
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  res.time_sec = std::chrono::duration<double>(end - start).count();
  return res;
}

} // namespace sopt

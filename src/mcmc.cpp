#include "mcmc.hpp"
#include "testgen.hpp"
#include "mutator.hpp"
#include "cost.hpp"
#include "interpreter.hpp"
#include "sim.hpp"
#include "features.hpp"
#include <random>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <chrono>
#include <cstring>

namespace sopt {

namespace {

long long perf_cost(const Insn* prog, int n, const std::vector<int>& live_out,
                    const MCMCConfig& cfg) {
  switch (cfg.cost_mode) {
    case 1: { // gradient-boosted regression model
      BlockFeatures f = extract_features(prog, n, live_out);
      return static_cast<long long>(std::llround(predict_cycles(f)));
    }
    case 2: { // static bundling: DAG + height-ranked list scheduling
      SimResult r = sim_cycles(prog, n, SimMode::BUNDLED);
      return static_cast<long long>(r.cycles);
    }
    case 3: { // dynamic: Tomasulo + ROB
      SimResult r = sim_cycles(prog, n, SimMode::DYNAMIC);
      return static_cast<long long>(r.cycles);
    }
    case 0:
    default:
      return perf_insn_count(prog, n);
  }
}

long long total_cost(const Insn* prog, int n, const std::vector<TestCase>& tests,
                     const std::vector<int>& live_out, const MCMCConfig& cfg) {
  long long eqv = cost_eq_only(prog, n, tests, live_out);
  long long perv = perf_cost(prog, n, live_out, cfg);
  // eq dominates so correctness > performance lexicographically
  return eqv * 1000LL + perv;
}

struct ChainState {
  std::vector<Insn> cur;
  long long cost = 0;
  long long eq = 0;
  long long perf = 0;
  std::vector<Insn> best;
  long long best_cost = std::numeric_limits<long long>::max();
  long long best_eq = std::numeric_limits<long long>::max();
  long long best_perf = std::numeric_limits<long long>::max();
  long long best_insns = std::numeric_limits<long long>::max();
};

void run_chain(const ProgramSpec& spec, const std::vector<TestCase>& tests,
               const MCMCConfig& cfg, int L, int chain_idx, ChainState& st) {
  // Give every chain its own proposal stream so --chains actually explores
  // independent trajectories.
  mutator_init_random(cfg.seed + static_cast<unsigned>(chain_idx) * 7919u);
  std::mt19937 rng(cfg.seed + static_cast<unsigned>(chain_idx) * 104729u + 17u);
  std::uniform_real_distribution<double> urand(0.0, 1.0);

  st.cur.assign(L, make_nop());
  if (static_cast<int>(cfg.init_prog.size()) == L) st.cur = cfg.init_prog;

  st.cost = total_cost(st.cur.data(), L, tests, spec.live_out, cfg);
  st.eq = cost_eq_only(st.cur.data(), L, tests, spec.live_out);
  st.perf = perf_cost(st.cur.data(), L, spec.live_out, cfg);
  st.best = st.cur;
  st.best_cost = st.cost;
  st.best_eq = st.eq;
  st.best_perf = st.perf;
  st.best_insns = perf_insn_count(st.cur.data(), L);

  for (int iter = 0; iter < cfg.max_iters; iter++) {
    std::vector<Insn> cand = st.cur;
    mutate_program(spec, cand.data(), L);

    long long cand_eq = cost_eq_only(cand.data(), L, tests, spec.live_out);
    long long cand_perf = perf_cost(cand.data(), L, spec.live_out, cfg);
    long long cand_cost = cand_eq * 1000LL + cand_perf;
    long long cand_insns = perf_insn_count(cand.data(), L);

    double delta = static_cast<double>(cand_cost) - static_cast<double>(st.cost);
    bool accept = false;
    if (delta <= 0.0) {
      accept = true;
    } else {
      double prob = std::exp(-cfg.beta * delta);
      if (urand(rng) < prob) accept = true;
    }
    if (!accept) continue;

    st.cur = std::move(cand);
    st.cost = cand_cost;
    st.eq = cand_eq;
    st.perf = cand_perf;

    // Prefer a lower cost; break ties by instruction count so a cycle-cost run
    // still reports the simplest program among equally fast bundles.
    if (st.cost < st.best_cost ||
        (st.cost == st.best_cost && cand_insns < st.best_insns)) {
      st.best = st.cur;
      st.best_cost = st.cost;
      st.best_eq = st.eq;
      st.best_perf = st.perf;
      st.best_insns = cand_insns;
    }
  }
}

} // namespace

MCMCResult run_mcmc(const ProgramSpec& spec, const std::vector<TestCase>& tests,
                    const MCMCConfig& cfg) {
  MCMCResult res;
  res.best_cost = std::numeric_limits<long long>::max();
  res.best_eq = std::numeric_limits<long long>::max();
  res.best_perf = std::numeric_limits<long long>::max();
  res.time_sec = 0.0;

  auto start = std::chrono::high_resolution_clock::now();

  int L = spec.max_len > 0 ? spec.max_len : static_cast<int>(spec.code.size());
  if (L <= 0) L = 1;

  int chains = std::max(1, cfg.chains);
  for (int c = 0; c < chains; c++) {
    ChainState st;
    run_chain(spec, tests, cfg, L, c, st);
    if (st.best_cost < res.best_cost) {
      res.best_prog = st.best;
      res.best_cost = st.best_cost;
      res.best_eq = st.best_eq;
      res.best_perf = st.best_perf;
    }
  }

  auto end = std::chrono::high_resolution_clock::now();
  res.time_sec = std::chrono::duration<double>(end - start).count();
  return res;
}

} // namespace sopt

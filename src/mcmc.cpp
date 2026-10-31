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

  auto score = [&](const std::vector<Insn>& p, long long& c, long long& e, long long& pf) {
    e = cost_eq_only(p.data(), L, tests, spec.live_out);
    pf = perf_cost(p.data(), L, spec.live_out, cfg);
    c = e * 1000LL + pf;
  };
  // Prefer a lower cost; break ties by instruction count so a cycle-cost run
  // still reports the simplest program among equally fast bundles.
  auto new_best = [&](long long cost, long long eq, long long perf, long long insns) {
    if (cost < st.best_cost || (cost == st.best_cost && insns < st.best_insns)) {
      st.best = st.cur;
      st.best_cost = cost;
      st.best_eq = eq;
      st.best_perf = perf;
      st.best_insns = insns;
      return true;
    }
    return false;
  };

  st.cur.assign(L, make_nop());
  if (static_cast<int>(cfg.init_prog.size()) == L) st.cur = cfg.init_prog;

  score(st.cur, st.cost, st.eq, st.perf);
  st.best = st.cur;
  st.best_cost = st.cost;
  st.best_eq = st.eq;
  st.best_perf = st.perf;
  st.best_insns = perf_insn_count(st.cur.data(), L);

  // Adaptive temperature state.
  double beta = cfg.beta;
  int window_proposals = 0;
  int window_accepts = 0;
  int since_improve = 0;

  for (int iter = 0; iter < cfg.max_iters; iter++) {
    // HC-MCMC: when greedy descent has stalled, kick the incumbent best with a
    // few random mutations and keep climbing from there. This is the "MCMC"
    // half of the hybrid: the kick is an unconditional jump, while descent
    // stays strictly greedy.
    if (cfg.algo == SearchAlgo::HC_MCMC &&
        since_improve >= std::max(1, cfg.hc_stall_limit)) {
      st.cur = st.best;
      for (int k = 0; k < std::max(1, cfg.hc_kick_mutations); k++) {
        mutate_program(spec, st.cur.data(), L);
      }
      score(st.cur, st.cost, st.eq, st.perf);
      new_best(st.cost, st.eq, st.perf, perf_insn_count(st.cur.data(), L));
      since_improve = 0;
    }

    std::vector<Insn> cand = st.cur;
    mutate_program(spec, cand.data(), L);

    long long cand_eq, cand_perf, cand_cost;
    score(cand, cand_cost, cand_eq, cand_perf);
    long long cand_insns = perf_insn_count(cand.data(), L);

    double delta = static_cast<double>(cand_cost) - static_cast<double>(st.cost);
    bool accept = false;
    if (cfg.algo == SearchAlgo::HC_MCMC) {
      // Greedy hill climbing: no uphill moves, but equal-cost plateau moves
      // are allowed so the walk can traverse flat regions.
      accept = cand_cost <= st.cost;
    } else {
      double cur_beta = (cfg.algo == SearchAlgo::ADAPTIVE_MCMC) ? beta : cfg.beta;
      if (delta <= 0.0) {
        accept = true;
      } else {
        double prob = std::exp(-cur_beta * delta);
        if (urand(rng) < prob) accept = true;
      }
      if (cfg.algo == SearchAlgo::ADAPTIVE_MCMC) {
        window_proposals++;
        if (accept) window_accepts++;
        if (window_proposals >= std::max(1, cfg.adapt_window)) {
          double rate = static_cast<double>(window_accepts) / window_proposals;
          beta *= std::exp(cfg.adapt_gain * (rate - cfg.target_accept));
          beta = std::clamp(beta, cfg.beta_min, cfg.beta_max);
          window_proposals = 0;
          window_accepts = 0;
        }
      }
    }

    if (!accept) {
      since_improve++;
      continue;
    }

    st.cur = std::move(cand);
    st.cost = cand_cost;
    st.eq = cand_eq;
    st.perf = cand_perf;

    if (new_best(st.cost, st.eq, st.perf, cand_insns)) {
      since_improve = 0;
    } else {
      since_improve++;
    }
  }
}

} // namespace

const char* search_algo_name(SearchAlgo algo) {
  switch (algo) {
    case SearchAlgo::ADAPTIVE_MCMC: return "adaptive";
    case SearchAlgo::HC_MCMC:       return "hc";
    case SearchAlgo::MCMC:
    default:                        return "mcmc";
  }
}

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

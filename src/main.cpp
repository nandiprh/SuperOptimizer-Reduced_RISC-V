#include "isa.hpp"
#include "interpreter.hpp"
#include "testgen.hpp"
#include "cost.hpp"
#include "mutator.hpp"
#include "mcmc.hpp"
#include "enumerate.hpp"
#include "sim.hpp"
#include "features.hpp"
#include "verify.hpp"
#include "frontend.hpp"
#include <iostream>
#include <vector>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>

using namespace sopt;

static void trim(std::string& s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
}

// ---------------------------------------------------------------------------
// .sopt header parsing (registers may be xN, ABI names, or bare indices)
// ---------------------------------------------------------------------------

static bool parse_reg_token(const std::string& raw, int& out) {
  if (raw.empty()) return false;
  std::string s;
  for (char c : raw) {
    if (!std::isspace(static_cast<unsigned char>(c)) &&
        c != '{' && c != '}' && c != '[' && c != ']') s.push_back(c);
  }
  if (s.empty()) return false;
  bool numeric = true;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) { numeric = false; break; }
  }
  if (numeric) {
    out = std::stoi(s);
    return out >= 0 && out < 32;
  }
  out = reg_name_to_num(s);
  return out >= 0;
}

static void parse_reg_list(const std::string& rest, std::vector<int>& dst) {
  std::stringstream ss(rest);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    int r;
    if (parse_reg_token(tok, r)) dst.push_back(r);
  }
}

// Restrict the mutator pool to live_in ∪ live_out ∪ the scratch registers the
// target actually touches (architecture 3: pool of ~6-10, not all 32).
static void derive_temps(ProgramSpec& spec) {
  for (const Insn& insn : spec.code) {
    if (insn.op == Op::NOP) continue;
    for (int r : {static_cast<int>(insn.rd), static_cast<int>(insn.rs1), static_cast<int>(insn.rs2)}) {
      if (r == 0) continue; // x0 is always zero; no need in pool
      bool in_pool = false;
      for (int lr : spec.live_in) if (lr == r) { in_pool = true; break; }
      if (in_pool) continue;
      for (int lr : spec.live_out) if (lr == r) { in_pool = true; break; }
      if (!in_pool) spec.temps.push_back(r);
    }
  }
}

static int run_and_report(const ProgramSpec& spec, const MCMCConfig& cfg) {
  init_random_seed(cfg.seed);
  mutator_init_random(cfg.seed);

  int test_count = cfg.test_count > 0 ? cfg.test_count : 96;
  auto tests = gen_test_set(spec.code, spec.live_in, spec.live_out, test_count, true);
  auto res = run_mcmc(spec, tests, cfg);

  // --- Algorithm 2: Enumerative baseline (ground-truth optimum for short targets) ---
  EnumerateResult enum_res;
  if (cfg.do_enum) {
    EnumerateConfig ecfg;
    ecfg.max_len = cfg.enum_max_len > 0 ? cfg.enum_max_len : 3;
    ecfg.test_count = 96;
    ecfg.verify_survivors = false;
    enum_res = enumerate_search(spec, tests, ecfg);
  }

  long long target_perf = perf_insn_count(spec.code.data(), (int)spec.code.size());
  long long best_insns = perf_insn_count(res.best_prog.data(), (int)res.best_prog.size());
  auto insns_word = [](long long n) { return (n == 1 || n == -1) ? "insn" : "insns"; };
  std::cout << "best_cost=" << res.best_cost << " best_eq=" << res.best_eq
            << " best_perf=" << res.best_perf << " best_insns=" << best_insns
            << " time=" << res.time_sec << "\n";
  std::cout << "target program (" << target_perf << " " << insns_word(target_perf) << "):\n";
  for (const auto& insn : spec.code) {
    if (!is_nop(insn)) std::cout << "  " << format_insn(insn) << "\n";
  }
  std::cout << "optimized program (" << best_insns << " " << insns_word(best_insns) << "):\n";
  for (const auto& insn : res.best_prog) {
    if (!is_nop(insn)) std::cout << "  " << format_insn(insn) << "\n";
  }
  if (cfg.do_enum) {
    std::cout << "enumerative tested=" << enum_res.programs_tested
              << " enum_best_perf=" << enum_res.best_perf
              << " enum_eq=" << enum_res.best_eq
              << " enum_time=" << enum_res.time_sec << "\n";
    if (enum_res.best_eq == 0 && enum_res.best_perf < best_insns) {
      std::cout << "NOTE: enumerative found shorter correct program (" << enum_res.best_perf
                << " insns) than MCMC (" << best_insns << ")\n";
    }
  }

  if (res.best_eq == 0) {
    // --- Algorithm 5: Z3 exact verification + counterexample loop ---
    if (cfg.do_z3 && z3_available()) {
      std::vector<TestCase> ce_tests = tests; // mutable test set grows with counterexamples
      int max_ce_rounds = 5;
      for (int round = 0; round < max_ce_rounds; round++) {
        auto vr = z3_verify_equivalent(spec, res.best_prog, spec.live_out);
        if (vr.equivalent) {
          std::cout << "verify: OK (Z3-proven, QF_BV)\n";
          break;
        }
        if (vr.status == "sat") {
          auto ce = z3_counterexample(spec, res.best_prog, spec.live_out);
          if (!ce.has_value()) {
            std::cout << "verify: OK (Z3-proven, QF_BV)\n";
            break;
          }
          ce_tests.push_back(*ce);
          std::cout << "z3 counterexample found; adding to test set (round " << (round + 1) << ")\n";
          // Resume the search from the current best program, against the enlarged
          // test set, rather than restarting from an all-NOP buffer.
          MCMCConfig resume = cfg;
          resume.init_prog = res.best_prog;
          resume.seed = cfg.seed + static_cast<unsigned>(round + 1) * 1013u;
          auto res2 = run_mcmc(spec, ce_tests, resume);
          if (res2.best_eq == 0) {
            res = res2;
          } else {
            std::cout << "verify: search could not satisfy counterexample; keeping previous candidate\n";
            break;
          }
        } else {
          std::cout << "verify: Z3 " << vr.status << " (could not prove)\n";
          break;
        }
      }
    } else {
      // Probabilistic verification (fresh random tests).
      auto vtests = gen_test_set(spec.code, spec.live_in, spec.live_out, 4096, false);
      auto vtests2 = gen_tight_test_set(spec.code, spec.live_in, spec.live_out, 4096);
      long long veq = eq_dist(res.best_prog.data(), (int)res.best_prog.size(), vtests, spec.live_out) +
                      eq_dist(res.best_prog.data(), (int)res.best_prog.size(), vtests2, spec.live_out);
      std::cout << "verify: " << (veq == 0 ? "OK (probabilistic)" : "FAILED") << " fresh_rand_eq=" << veq << "\n";
    }
  }
  return 0;
}

static bool load_sopt(const std::string& fname, ProgramSpec& spec) {
  std::ifstream f(fname);
  if (!f) {
    std::cerr << "cannot open " << fname << "\n";
    return false;
  }
  std::string line;
  bool in_header = true;
  while (std::getline(f, line)) {
    trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (in_header) {
      if (line.find("code:") != std::string::npos) { in_header = false; continue; }
      if (line.find("live_in:") != std::string::npos) {
        size_t p = line.find(':');
        if (p != std::string::npos) parse_reg_list(line.substr(p + 1), spec.live_in);
        continue;
      }
      if (line.find("live_out:") != std::string::npos) {
        size_t p = line.find(':');
        if (p != std::string::npos) parse_reg_list(line.substr(p + 1), spec.live_out);
        continue;
      }
      continue;
    }
    Insn insn;
    if (parse_insn(line, insn)) {
      spec.code.push_back(insn);
    } else if (!line.empty()) {
      std::cerr << "warning: skipping unrecognized line: " << line << "\n";
    }
  }
  if (spec.live_in.empty()) spec.live_in.push_back(10);
  if (spec.live_out.empty()) spec.live_out.push_back(10);
  if (spec.code.empty()) {
    std::cerr << "warning: target has no parsed instructions; search is meaningless\n";
    spec.max_len = 4;
  }
  derive_temps(spec);
  return true;
}

static void repl(const MCMCConfig& base_cfg) {
  MCMCConfig cfg = base_cfg;
  std::cout << "sopt frontend (type an expression, e.g.  t3 = (a0 + a1) - a1 << 1;)\n"
               "commands: :help  :iters N  :beta B  :seed S  :show  :quit\n";
  std::string pending;
  while (true) {
    std::cout << "sopt> " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) { std::cout << "\n"; break; }
    trim(line);
    if (line.empty()) continue;

    if (line[0] == ':') {
      if (line == ":quit" || line == ":q" || line == ":exit") break;
      if (line == ":help") {
        std::cout << "  <dst> = <expr>   superoptimize an expression block\n"
                     "  :iters N         set MCMC iterations (default " << base_cfg.max_iters << ")\n"
                     "  :beta B          set inverse temperature (default " << base_cfg.beta << ")\n"
                     "  :seed S          set RNG seed (default " << base_cfg.seed << ")\n"
                     "  :show            print current settings\n"
                     "  :quit            exit\n";
        continue;
      }
      if (line.rfind(":iters", 0) == 0) {
        std::stringstream ss(line.substr(7));
        int v; if (ss >> v && v > 0) cfg.max_iters = v;
        else std::cout << "usage: :iters <positive int>\n";
        continue;
      }
      if (line.rfind(":beta", 0) == 0) {
        std::stringstream ss(line.substr(6));
        double v; if (ss >> v && v > 0.0) cfg.beta = v;
        else std::cout << "usage: :beta <positive float>\n";
        continue;
      }
      if (line.rfind(":seed", 0) == 0) {
        std::stringstream ss(line.substr(6));
        unsigned int v; if (ss >> v) cfg.seed = v;
        else std::cout << "usage: :seed <u32>\n";
        continue;
      }
      if (line == ":show") {
        std::cout << "iters=" << cfg.max_iters << " beta=" << cfg.beta
                  << " seed=" << cfg.seed << " chains=" << cfg.chains << "\n";
        continue;
      }
      std::cout << "unknown command (see :help)\n";
      continue;
    }

    pending += (pending.empty() ? "" : " ") + line;
    if (!pending.empty() && pending.back() != ';') continue;  // multi-line block

    ProgramSpec spec;
    std::string err;
    if (!parse_expression_program(pending, spec, err)) {
      std::cout << "parse error: " << err << "\n";
    } else if (spec.code.empty()) {
      std::cout << "no instructions generated\n";
    } else {
      derive_temps(spec);
      spec.max_len = (int)spec.code.size();
      run_and_report(spec, cfg);
    }
    pending.clear();
  }
}

static void print_usage(const char* prog) {
  std::cerr << "Usage:\n"
            << "  " << prog << " <file.sopt> [--beta B --iters N --chains C --seed S]\n"
            << "  " << prog << " --expr \"<block>\" [options]     compile+optimize an expression on the go\n"
            << "  " << prog << " [options]                       interactive REPL\n"
            << "\nOptions:\n"
            << "  --beta B         inverse temperature (default 1.0)\n"
            << "  --iters N        MCMC iterations (default 10000)\n"
            << "  --chains C       accepted; only 1 chain runs today\n"
            << "  --seed S         RNG seed (default 12345)\n"
            << "  --cost-mode M    perf term: 0=insn count, 1=regression cycles, 2=bundled sim, 3=Tomasulo sim (default 0)\n"
            << "  --enum           run enumerative baseline for ground-truth optimum\n"
            << "  --enum-max-len N max length for enumerative search (default 3)\n"
            << "  --z3             Z3 QF_BV exact verification + counterexample loop\n"
            << "\nExamples:\n"
            << "  " << prog << " bench/toy/smoke.sopt --iters 300000 --beta 2 --seed 1\n"
            << "  " << prog << " --expr \"t3 = (a0 + a1) - a1 << 1;\" --iters 300000\n"
            << "  " << prog << " bench/toy/smoke.sopt --iters 300000 --beta 2 --enum --z3\n";
}

int main(int argc, char** argv) {
  double beta = 1.0;
  int iters = 10000;
  int chains = 1;
  unsigned int seed = 12345;
  std::string fname;
  std::string expr;

  MCMCConfig cfg;
  cfg.max_iters = iters;
  cfg.chains = chains;
  cfg.seed = seed;
  cfg.beta = beta;
  cfg.test_count = 96;
  cfg.use_regression = false;
  cfg.cost_mode = 0;
  cfg.do_enum = false;
  cfg.enum_max_len = 3;
  cfg.do_z3 = false;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--beta" && i + 1 < argc) beta = std::stod(argv[++i]);
    else if (a == "--iters" && i + 1 < argc) iters = std::stoi(argv[++i]);
    else if (a == "--chains" && i + 1 < argc) chains = std::stoi(argv[++i]);
    else if (a == "--seed" && i + 1 < argc) seed = static_cast<unsigned int>(std::stoi(argv[++i]));
    else if ((a == "--expr" || a == "-e") && i + 1 < argc) expr = argv[++i];
    else if (a == "--enum") cfg.do_enum = true;
    else if (a == "--enum-max-len" && i + 1 < argc) cfg.enum_max_len = std::stoi(argv[++i]);
    else if (a == "--z3") cfg.do_z3 = true;
    else if (a == "--cost-mode" && i + 1 < argc) cfg.cost_mode = std::stoi(argv[++i]);
    else if (!a.empty() && a[0] != '-' && fname.empty()) fname = a;
    else { print_usage(argv[0]); return 1; }
  }

  cfg.max_iters = iters;
  cfg.chains = chains;
  cfg.seed = seed;
  cfg.beta = beta;

  if (!expr.empty()) {
    ProgramSpec spec;
    std::string err;
    if (!parse_expression_program(expr, spec, err)) {
      std::cerr << "parse error: " << err << "\n";
      return 1;
    }
    derive_temps(spec);
    spec.max_len = (int)spec.code.size();
    return run_and_report(spec, cfg);
  }

  if (!fname.empty()) {
    ProgramSpec spec;
    if (!load_sopt(fname, spec)) return 1;
    spec.max_len = (int)spec.code.size();
    if (spec.code.empty()) spec.max_len = 4;
    return run_and_report(spec, cfg);
  }

  repl(cfg);
  return 0;
}
// Generate labelled basic blocks for the learned cost model (Source A).
// Uses the exact runtime feature extractor and the bundling simulator so that
// training and inference cannot drift apart.
//
// Build with `make tools/gen_dataset` (or `make dataset` to label a dataset).
//
// Usage:
//   tools/gen_dataset --rows 20000 --out results/dataset.csv [--seed 1]
//                     [--min-len 1] [--max-len 12]

#include "isa.hpp"
#include "features.hpp"
#include "sim.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace sopt;

namespace {

const int kFeatureNames = kFeatureDim;

std::string feature_header() {
  return "n_insns,hist_nop,hist_add,hist_sub,hist_and,hist_or,hist_xor,"
         "hist_sll,hist_srl,hist_sra,hist_slt,hist_sltu,hist_addi,hist_andi,"
         "hist_ori,hist_xori,hist_slli,hist_srli,hist_srai,hist_slti,"
         "hist_sltiu,hist_lui,dep_depth,live_peak,roots,avg_dep_distance,"
         "n_waw,n_war,max_width,cycles_dynamic,cycles_bundled";
}

int rand_int(std::mt19937& rng, int lo, int hi) {
  std::uniform_int_distribution<int> d(lo, hi);
  return d(rng);
}

double rand_double(std::mt19937& rng) {
  std::uniform_real_distribution<double> d(0.0, 1.0);
  return d(rng);
}

int pick_source(std::mt19937& rng, double p_dep, int window,
                const std::vector<std::pair<int, int>>& writes, int pos) {
  if (p_dep > 0.0 && !writes.empty() && rand_double(rng) < p_dep) {
    std::vector<int> candidates;
    for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
      if (it->first < pos - window) break;
      candidates.push_back(it->second);
    }
    if (!candidates.empty()) {
      return candidates[rand_int(rng, 0, static_cast<int>(candidates.size()) - 1)];
    }
  }
  if (rand_int(rng, 0, 99) < 5) return 0;
  return rand_int(rng, 1, 31);
}

int pick_imm(std::mt19937& rng, Fmt fmt) {
  if (fmt == Fmt::IShift) return rand_int(rng, 0, 31);
  static const int base[] = {0, 1, -1, 2, 4, 8, 16, 31, 32, 255, 2047, -2048};
  if (rand_int(rng, 0, 99) < 80) {
    return base[rand_int(rng, 0, static_cast<int>(sizeof(base) / sizeof(base[0])) - 1)];
  }
  return rand_int(rng, -2048, 2047);
}

std::vector<Insn> random_program(std::mt19937& rng, int n) {
  std::vector<Insn> prog;
  prog.reserve(n);
  std::vector<std::pair<int, int>> writes; // (position, register)

  double p_dep = rand_double(rng);
  static const int windows[] = {1, 2, 4, 8};
  int window = windows[rand_int(rng, 0, 3)];

  for (int i = 0; i < n; i++) {
    Op op = static_cast<Op>(rand_int(rng, 1, 20));
    Fmt fmt = kOps[static_cast<size_t>(op)].fmt;

    Insn insn{};
    insn.op = op;
    insn.rd = static_cast<uint8_t>(rand_int(rng, 1, 31));
    insn.rs1 = insn.rs2 = 0;
    insn.imm = 0;

    switch (fmt) {
      case Fmt::R:
        insn.rs1 = static_cast<uint8_t>(pick_source(rng, p_dep, window, writes, i));
        insn.rs2 = static_cast<uint8_t>(pick_source(rng, p_dep, window, writes, i));
        break;
      case Fmt::I:
      case Fmt::IShift:
        insn.rs1 = static_cast<uint8_t>(pick_source(rng, p_dep, window, writes, i));
        insn.imm = pick_imm(rng, fmt);
        break;
      case Fmt::U:
        insn.imm = rand_int(rng, 0, 0xFFFFF);
        break;
      case Fmt::None:
        break;
    }

    writes.emplace_back(i, insn.rd);
    prog.push_back(insn);
  }
  return prog;
}

std::vector<int> pick_live_out(std::mt19937& rng, const std::vector<Insn>& prog) {
  std::set<int> regs;
  for (const auto& insn : prog) {
    if (insn.op != Op::NOP && insn.rd != 0) regs.insert(insn.rd);
  }
  if (regs.empty()) return {10};

  std::vector<int> pool(regs.begin(), regs.end());
  int k = 1 + rand_int(rng, 0, std::min<int>(2, static_cast<int>(pool.size()) - 1));
  std::shuffle(pool.begin(), pool.end(), rng);
  pool.resize(k);
  return pool;
}

} // namespace

int main(int argc, char** argv) {
  int rows = 20000;
  int min_len = 1;
  int max_len = 12;
  unsigned seed = 1;
  std::string out_path = "results/dataset.csv";

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--rows" && i + 1 < argc) rows = std::stoi(argv[++i]);
    else if (a == "--min-len" && i + 1 < argc) min_len = std::stoi(argv[++i]);
    else if (a == "--max-len" && i + 1 < argc) max_len = std::stoi(argv[++i]);
    else if (a == "--seed" && i + 1 < argc) seed = static_cast<unsigned>(std::stoul(argv[++i]));
    else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
    else {
      std::cerr << "usage: " << argv[0]
                << " [--rows N] [--min-len N] [--max-len N] [--seed S] [--out file.csv]\n";
      return 1;
    }
  }
  if (min_len < 1) min_len = 1;
  if (max_len < min_len) max_len = min_len;

  std::ofstream out(out_path);
  if (!out) {
    std::cerr << "cannot open " << out_path << "\n";
    return 1;
  }
  out << feature_header() << "\n";

  std::mt19937 rng(seed);
  for (int row = 0; row < rows; row++) {
    int n = rand_int(rng, min_len, max_len);
    std::vector<Insn> prog = random_program(rng, n);
    std::vector<int> live_out = pick_live_out(rng, prog);

    BlockFeatures f = extract_features(prog.data(), static_cast<int>(prog.size()), live_out);
    double vec[kFeatureDim];
    features_to_vector(f, vec);

    SimResult dyn = sim_cycles(prog.data(), static_cast<int>(prog.size()), SimMode::DYNAMIC);
    SimResult bun = sim_cycles(prog.data(), static_cast<int>(prog.size()), SimMode::BUNDLED);

    for (int j = 0; j < kFeatureNames; j++) {
      out << vec[j];
      if (j + 1 < kFeatureNames) out << ',';
    }
    out << ',' << dyn.cycles << ',' << bun.cycles << '\n';
  }

  std::cerr << "wrote " << rows << " rows to " << out_path << "\n";
  return 0;
}

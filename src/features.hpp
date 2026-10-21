#pragma once

#include "isa.hpp"
#include <vector>
#include <cstdint>

namespace sopt {

// Feature vector for a basic block, matching architecture.md §6.4.
// Computed identically in C++ (runtime inference) and Python (training)
// to avoid train/serve skew.
struct BlockFeatures {
  int n_insns = 0;                 // non-NOP count
  int hist[21] = {};               // per-opcode histogram (indexed by Op enum)
  int dep_depth = 0;               // longest RAW chain via def-use DAG
  int n_live_regs_peak = 0;        // max simultaneously live registers
  int n_independent_roots = 0;     // insns with no in-block producer
  double avg_dep_distance = 0.0;   // mean producer->consumer distance
  int n_waw = 0;                   // write-after-write name dependencies
  int n_war = 0;                   // write-after-read name dependencies
  double max_width_estimate = 0.0; // n_insns / dep_depth
};

// Compute features for `seq` (length n). live_out registers are considered
// live at block end for liveness analysis.
BlockFeatures extract_features(const Insn* seq, int n,
                               const std::vector<int>& live_out);

// Flatten features into a fixed-length double vector for model input.
// Order: [n_insns, hist[0..20], dep_depth, n_live_regs_peak,
//         n_independent_roots, avg_dep_distance, n_waw, n_war,
//         max_width_estimate]
constexpr int kFeatureDim = 1 + 21 + 7; // 29
void features_to_vector(const BlockFeatures& f, double* out);

// Predict cycles from features using the generated gradient-boosted model.
// Returns -1 if no model is available (fall back to instruction count).
double predict_cycles(const BlockFeatures& f);

} // namespace sopt

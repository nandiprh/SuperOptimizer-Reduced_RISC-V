#include "features.hpp"
#include "model_gen.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sopt {

BlockFeatures extract_features(const Insn* seq, int n,
                               const std::vector<int>& live_out) {
  BlockFeatures f;

  // Per-register last writer and first reader positions.
  std::vector<int> last_writer(32, -1);
  std::vector<int> last_reader(32, -1);
  // For liveness: a register is live from its definition until its last use
  // (or until block end if it's live_out).
  std::vector<int> def_pos(32, -1);   // position where reg was last defined
  std::vector<int> last_use_pos(32, -1); // last position where reg is read

  std::set<int> live_out_set(live_out.begin(), live_out.end());

  // First pass: find last use position of each register
  for (int i = 0; i < n; i++) {
    if (seq[i].op == Op::NOP) continue;
    f.n_insns++;
    f.hist[static_cast<size_t>(seq[i].op)]++;

    for (int r : { (int)seq[i].rs1, (int)seq[i].rs2 }) {
      if (r != 0) last_use_pos[r] = i;
    }
  }

  // dep_depth: longest RAW chain
  std::vector<int> depth(n, 0);
  std::vector<int> lw(32, -1);
  int max_depth = 0;
  int n_roots = 0;
  long long total_dep_dist = 0;
  int n_dep_edges = 0;

  for (int i = 0; i < n; i++) {
    if (seq[i].op == Op::NOP) continue;
    int d = 0;
    bool has_producer = false;
    for (int r : { (int)seq[i].rs1, (int)seq[i].rs2 }) {
      if (r == 0) continue;
      int w = lw[r];
      if (w >= 0) {
        d = std::max(d, depth[w] + 1);
        has_producer = true;
        total_dep_dist += (i - w);
        n_dep_edges++;
      }
    }
    depth[i] = d;
    max_depth = std::max(max_depth, d);
    if (!has_producer) n_roots++;

    int wr = (seq[i].rd != 0) ? (int)seq[i].rd : -1;
    if (wr >= 0) lw[wr] = i;
  }
  f.dep_depth = max_depth;
  f.n_independent_roots = n_roots;
  f.avg_dep_distance = (n_dep_edges > 0) ? (double)total_dep_dist / n_dep_edges : 0.0;

  // Liveness: compute live register count at each point.
  // A register is live after position i if it has been defined at or before i
  // and will be used after i (or is live_out and defined at or before i).
  // We compute live_count after each instruction.
  std::vector<bool> defined(32, false);
  std::vector<bool> will_be_used(32, false);
  for (int r = 0; r < 32; r++) {
    if (last_use_pos[r] >= 0) will_be_used[r] = true;
  }
  // Also live_out registers are "used" at block end
  for (int r : live_out) {
    if (r >= 0 && r < 32) will_be_used[r] = true;
  }

  int peak = 0;
  for (int i = 0; i < n; i++) {
    if (seq[i].op == Op::NOP) continue;
    // Mark reads as consumed
    for (int r : { (int)seq[i].rs1, (int)seq[i].rs2 }) {
      if (r != 0 && last_use_pos[r] == i) {
        will_be_used[r] = false;
      }
    }
    // Mark write
    int wr = (seq[i].rd != 0) ? (int)seq[i].rd : -1;
    if (wr >= 0) defined[wr] = true;
    // Count live: defined and will be used later (or live_out)
    int live_count = 0;
    for (int r = 1; r < 32; r++) { // skip x0
      if (defined[r] && will_be_used[r]) live_count++;
    }
    peak = std::max(peak, live_count);
  }
  f.n_live_regs_peak = peak;

  // WAW and WAR name dependencies
  std::vector<int> lw2(32, -1);
  std::vector<int> lr2(32, -1);
  int n_waw = 0, n_war = 0;
  for (int i = 0; i < n; i++) {
    if (seq[i].op == Op::NOP) continue;
    int wr = (seq[i].rd != 0) ? (int)seq[i].rd : -1;
    if (wr >= 0) {
      if (lw2[wr] >= 0) n_waw++;
      lw2[wr] = i;
    }
    for (int r : { (int)seq[i].rs1, (int)seq[i].rs2 }) {
      if (r == 0) continue;
      if (lr2[r] >= 0 && lw2[r] >= 0 && lw2[r] > lr2[r]) {
        // WAR: earlier instruction wrote r (at lw2[r]), current reads r
        // Actually WAR: i writes r, earlier j reads r. We track differently.
      }
      lr2[r] = i;
    }
  }
  // Simpler WAR/WAW: count pairs where same register is written multiple times
  // (WAW) or written after being read (WAR).
  {
    std::vector<int> wpos(32, -1);
    std::vector<int> rpos(32, -1);
    for (int i = 0; i < n; i++) {
      if (seq[i].op == Op::NOP) continue;
      int wr = (seq[i].rd != 0) ? (int)seq[i].rd : -1;
      if (wr >= 0) {
        if (wpos[wr] >= 0) n_waw++;
        wpos[wr] = i;
      }
      for (int r : { (int)seq[i].rs1, (int)seq[i].rs2 }) {
        if (r == 0) continue;
        rpos[r] = i;
        // WAR: if a later write happens after this read
        if (wpos[r] > i) n_war++; // approximate: count reads followed by writes
      }
    }
  }
  f.n_waw = n_waw;
  f.n_war = n_war;

  f.max_width_estimate = (max_depth > 0) ? (double)f.n_insns / max_depth : (double)f.n_insns;
  return f;
}

void features_to_vector(const BlockFeatures& f, double* out) {
  int k = 0;
  out[k++] = (double)f.n_insns;
  for (int i = 0; i < 21; i++) out[k++] = (double)f.hist[i];
  out[k++] = (double)f.dep_depth;
  out[k++] = (double)f.n_live_regs_peak;
  out[k++] = (double)f.n_independent_roots;
  out[k++] = f.avg_dep_distance;
  out[k++] = (double)f.n_waw;
  out[k++] = (double)f.n_war;
  out[k++] = f.max_width_estimate;
}

double predict_cycles(const BlockFeatures& f) {
  double vec[kFeatureDim];
  features_to_vector(f, vec);
  return model_predict(vec);
}

} // namespace sopt

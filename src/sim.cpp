#include "sim.hpp"
#include <algorithm>
#include <limits>
#include <vector>

namespace sopt {

namespace {

const SimConfig kDefaultCfg{4, 16, 4, 1, 2, true};

int op_latency(const Insn& insn, const SimConfig& cfg) {
  if (insn.op == Op::NOP) return 0;
  int lat = static_cast<int>(kOps[static_cast<size_t>(insn.op)].latency);
  if (lat <= 0) lat = cfg.latency_alu > 0 ? cfg.latency_alu : 1;
  return lat;
}

// Registers read by an instruction. NOP reads nothing, and x0 reads are
// ignored because x0 is hard-wired to zero.
std::vector<int> read_regs(const Insn& insn) {
  std::vector<int> r;
  if (insn.op == Op::NOP) return r;
  switch (kOps[static_cast<size_t>(insn.op)].fmt) {
    case Fmt::R:
      if (insn.rs1 != 0) r.push_back(insn.rs1);
      if (insn.rs2 != 0) r.push_back(insn.rs2);
      break;
    case Fmt::I:
    case Fmt::IShift:
      if (insn.rs1 != 0) r.push_back(insn.rs1);
      break;
    case Fmt::U:
    case Fmt::None:
      break;
  }
  return r;
}

int written_reg(const Insn& insn) {
  if (insn.op == Op::NOP || insn.rd == 0) return -1;
  if (kOps[static_cast<size_t>(insn.op)].fmt == Fmt::None) return -1;
  return static_cast<int>(insn.rd);
}

struct Dag {
  std::vector<std::vector<int>> preds;
  std::vector<std::vector<int>> succs;
};

// Build the dependency DAG from register def-use chains.
//   RAW: consumer -> reaching definition.
//   WAW: writer -> previous writer (name dependence).
//   WAR: writer -> every reader since the previous write (name dependence).
// With include_name_deps=false only true data (RAW) edges are emitted.
// All edges go from a lower to a higher instruction index, so the nodes are
// already in topological order.
Dag build_dag(const Insn* seq, int n, bool include_name_deps) {
  Dag dag;
  dag.preds.assign(n, {});
  dag.succs.assign(n, {});

  std::vector<int> last_writer(32, -1);
  std::vector<std::vector<int>> readers_since_write(32);

  for (int i = 0; i < n; i++) {
    for (int r : read_regs(seq[i])) {
      int w = last_writer[r];
      if (w >= 0) dag.preds[i].push_back(w);
    }

    if (include_name_deps) {
      int wr = written_reg(seq[i]);
      if (wr >= 0) {
        // WAR: every outstanding reader of wr must read before i overwrites it.
        for (int j : readers_since_write[wr]) dag.preds[i].push_back(j);
        // WAW: i must not overtake the previous writer of wr.
        if (last_writer[wr] >= 0) dag.preds[i].push_back(last_writer[wr]);
      }
    }

    for (int r : read_regs(seq[i])) readers_since_write[r].push_back(i);
    int wr = written_reg(seq[i]);
    if (wr >= 0) {
      last_writer[wr] = i;
      readers_since_write[wr].clear();
    }
  }

  for (int i = 0; i < n; i++) {
    auto& p = dag.preds[i];
    std::sort(p.begin(), p.end());
    p.erase(std::unique(p.begin(), p.end()), p.end());
    for (int j : p) dag.succs[j].push_back(i);
  }
  return dag;
}

} // namespace

std::vector<int> dep_depths(const Insn* seq, int n) {
  std::vector<int> depth(n, 0);
  if (n <= 0) return depth;
  Dag dag = build_dag(seq, n, false);
  for (int i = 0; i < n; i++) {
    int d = 0;
    for (int p : dag.preds[i]) d = std::max(d, depth[p] + 1);
    depth[i] = d;
  }
  return depth;
}

namespace {

// Height = cycles on the longest dependency path starting at this instruction.
std::vector<int> compute_heights(const Insn* seq, int n, const Dag& dag,
                                 const SimConfig& cfg) {
  std::vector<int> height(n, 0);
  for (int i = n - 1; i >= 0; i--) {
    int h = 0;
    for (int s : dag.succs[i]) h = std::max(h, height[s]);
    height[i] = op_latency(seq[i], cfg) + h;
  }
  return height;
}

SimResult run_bundled(const Insn* seq, int n, const SimConfig& cfg) {
  Dag dag = build_dag(seq, n, cfg.model_waw_war);
  std::vector<int> height = compute_heights(seq, n, dag, cfg);

  std::vector<int> finish(n, -1);
  std::vector<bool> issued(n, false), completed(n, false);

  int completed_count = 0;
  int cycle = 0;
  long long total_cycles = 0;
  long long issue_span = 0;
  long long exec_span = 0;

  while (completed_count < n) {
    for (int i = 0; i < n; i++) {
      if (issued[i] && !completed[i] && finish[i] <= cycle) {
        completed[i] = true;
        completed_count++;
        total_cycles = std::max(total_cycles, static_cast<long long>(finish[i]));
      }
    }
    if (completed_count == n) break;

    std::vector<int> ready;
    ready.reserve(n);
    for (int i = 0; i < n; i++) {
      if (issued[i]) continue;
      bool ok = true;
      for (int p : dag.preds[i]) {
        if (!completed[p]) { ok = false; break; }
      }
      if (ok) ready.push_back(i);
    }

    // Critical-path-first (Hu's height ranking), then program order for ties.
    std::sort(ready.begin(), ready.end(), [&](int a, int b) {
      if (height[a] != height[b]) return height[a] > height[b];
      return a < b;
    });

    int issued_this_cycle = 0;
    for (int i : ready) {
      if (issued_this_cycle >= cfg.issue_width) break;
      issued[i] = true;
      finish[i] = cycle + op_latency(seq[i], cfg);
      issued_this_cycle++;
      issue_span = std::max(issue_span, static_cast<long long>(cycle + 1));
      exec_span = std::max(exec_span, static_cast<long long>(finish[i]));
    }

    if (issued_this_cycle == 0) {
      // No progress possible: only happens if the DAG is malformed. Jump to the
      // earliest completion to avoid spinning forever.
      int next = std::numeric_limits<int>::max();
      for (int i = 0; i < n; i++) {
        if (issued[i] && !completed[i]) next = std::min(next, finish[i]);
      }
      if (next == std::numeric_limits<int>::max()) break;
      cycle = std::max(cycle + 1, next);
    } else {
      cycle++;
    }
  }

  return {total_cycles, issue_span, exec_span};
}

SimResult run_tomasulo(const Insn* seq, int n, const SimConfig& cfg) {
  // Only RAW dependencies matter: register renaming at issue removes WAR/WAW.
  // Instructions issue in program order into reservation stations, execute out
  // of order once their captured operands are ready, and commit in order from
  // the reorder buffer.

  const int max_rs = cfg.rs_alu;
  std::vector<int> producer_of(32, -1);      // most recently issued writer
  std::vector<std::vector<int>> src_prod(n); // producers captured at issue
  std::vector<int> issue_cycle(n, -1), exec_start(n, -1), exec_finish(n, -1),
      commit_cycle(n, -1);
  std::vector<bool> issued(n, false), started(n, false), done(n, false),
      committed(n, false);

  struct RSEntry {
    int insn = -1;
  };
  std::vector<RSEntry> rs(max_rs);

  std::vector<int> rob(cfg.rob_size, -1);
  int rob_head = 0, rob_tail = 0, rob_count = 0;

  int next_to_issue = 0;
  int committed_count = 0;
  int cycle = 0;
  long long total_cycles = 0;
  long long issue_span = 0;
  long long exec_span = 0;

  while (committed_count < n) {
    // 1. Issue in program order, up to issue_width per cycle.
    int issued_this_cycle = 0;
    while (next_to_issue < n && issued_this_cycle < cfg.issue_width &&
           rob_count < cfg.rob_size) {
      int free_rs = -1;
      for (int k = 0; k < max_rs; k++) {
        if (rs[k].insn < 0) { free_rs = k; break; }
      }
      if (free_rs < 0) break;

      int i = next_to_issue;
      src_prod[i].clear();
      for (int r : read_regs(seq[i])) {
        int p = producer_of[r];
        if (p >= 0) src_prod[i].push_back(p);
      }

      rs[free_rs].insn = i;
      issued[i] = true;
      issue_cycle[i] = cycle;
      issue_span = std::max(issue_span, static_cast<long long>(cycle + 1));

      int wr = written_reg(seq[i]);
      if (wr >= 0) producer_of[wr] = i;

      rob[rob_tail] = i;
      rob_tail = (rob_tail + 1) % cfg.rob_size;
      rob_count++;

      next_to_issue++;
      issued_this_cycle++;
    }

    // 2. Execute: start any RS entry whose captured producers have written back.
    for (int k = 0; k < max_rs; k++) {
      int i = rs[k].insn;
      if (i < 0 || started[i]) continue;
      bool ready = true;
      for (int p : src_prod[i]) {
        // A producer's value is available as soon as its execution completes;
        // write-back and a dependent's execute can happen in the same cycle.
        if (exec_finish[p] < 0 || exec_finish[p] > cycle) { ready = false; break; }
      }
      if (!ready) continue;
      started[i] = true;
      exec_start[i] = cycle;
      exec_finish[i] = cycle + op_latency(seq[i], cfg);
      exec_span = std::max(exec_span, static_cast<long long>(exec_finish[i]));
    }

    // 3. Write back completed results; this frees the reservation station.
    for (int k = 0; k < max_rs; k++) {
      int i = rs[k].insn;
      if (i < 0 || !started[i] || done[i]) continue;
      if (exec_finish[i] <= cycle) {
        done[i] = true;
        rs[k].insn = -1;
      }
    }

    // 4. Commit in order from the ROB head.
    while (rob_count > 0) {
      int i = rob[rob_head];
      if (i < 0 || !done[i]) break;
      committed[i] = true;
      commit_cycle[i] = cycle;
      total_cycles = std::max(total_cycles, static_cast<long long>(cycle));
      rob[rob_head] = -1;
      rob_head = (rob_head + 1) % cfg.rob_size;
      rob_count--;
      committed_count++;
    }

    cycle++;
    if (cycle > 1000000) break; // safety valve
  }

  return {total_cycles, issue_span, exec_span};
}

} // namespace

SimResult sim_cycles(const Insn* seq, int n, SimMode mode, const SimConfig& cfg) {
  if (n <= 0) return {0, 0, 0};

  SimConfig c = cfg;
  if (c.issue_width <= 0) c.issue_width = kDefaultCfg.issue_width;
  if (c.rob_size <= 0) c.rob_size = kDefaultCfg.rob_size;
  if (c.rs_alu <= 0) c.rs_alu = kDefaultCfg.rs_alu;
  if (c.latency_alu <= 0) c.latency_alu = kDefaultCfg.latency_alu;

  // The block has no memory ops in v1; fall back to ALU latency everywhere.
  (void)c.latency_mem;

  if (mode == SimMode::BUNDLED) return run_bundled(seq, n, c);
  return run_tomasulo(seq, n, c);
}

} // namespace sopt

#pragma once

#include "isa.hpp"
#include <cstdint>
#include <vector>

namespace sopt {

// Bundling-simulator: produces cycle-count labels for a basic block.
// Two modes: static bundling (height-ranked list scheduling) and
// dynamic scheduling (Tomasulo + ROB).
//
// Provides Algorithm 3 (cost oracle) from review2-doc.pdf / architecture.md §5.

struct SimConfig {
  int issue_width;          // max instructions issued per cycle
  int rob_size;             // reorder buffer entries (dynamic mode)
  int rs_alu;               // ALU reservation stations (dynamic mode)
  int latency_alu;          // cycles for ALU ops (v1: all 1)
  int latency_mem;          // not used in v1 (no memory)
  bool model_waw_war;       // static mode: stall on name dependencies
};

enum class SimMode { DYNAMIC, BUNDLED };

struct SimResult {
  long long cycles;         // total cycles to execute the block
  long long issue_cycles;   // cycles spent in issue stage
  long long exec_cycles;    // cycles spent in execution
};

// Build the dependency DAG: for each instruction, the set of producer
// instruction indices it depends on (RAW via register def-use).
// Returns dep_depth[i] = length of longest dependency chain ending at i.
std::vector<int> dep_depths(const Insn* seq, int n);

// Main entry point. Simulate `seq` (length n) and return cycle count.
// `cfg` may use defaults; pass SimConfig{} for defaults.
SimResult sim_cycles(const Insn* seq, int n, SimMode mode, const SimConfig& cfg);

// Convenience: simulate with default config.
inline SimResult sim_cycles(const Insn* seq, int n, SimMode mode) {
  SimConfig cfg{4, 16, 4, 1, 2, true};
  return sim_cycles(seq, n, mode, cfg);
}

} // namespace sopt

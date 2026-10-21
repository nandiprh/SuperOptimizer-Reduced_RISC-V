#pragma once

#include "isa.hpp"
#include <vector>
#include <string>
#include <optional>

namespace sopt {

struct ProgramSpec;
struct TestCase;

// Result of an exact equivalence check.
struct VerifyResult {
  bool equivalent;   // true => Z3 proved UNSAT (programs equivalent)
  std::string status; // "unsat", "sat", "unknown", "z3_unavailable"
  // Counterexample input values (register index -> value) when sat.
  std::vector<std::pair<int, uint32_t>> counterexample;
};

// Encode both programs as 32-bit bitvector formulas (QF_BV) and ask Z3
// whether any input makes a live-out register differ.
// Returns equivalent=true if UNSAT (proven equivalent).
// If SAT, counterexample holds the input assignment that distinguishes them.
VerifyResult z3_verify_equivalent(const ProgramSpec& target,
                                  const std::vector<Insn>& candidate,
                                  const std::vector<int>& live_out);

// Run the counterexample-guided loop: check candidate with Z3; if SAT, add the
// counterexample as a new test case and return it (so the caller can resume
// search). Returns nullopt if equivalent or Z3 unavailable.
std::optional<TestCase> z3_counterexample(const ProgramSpec& target,
                                          const std::vector<Insn>& candidate,
                                          const std::vector<int>& live_out);

// Whether the z3 binary is available on PATH.
bool z3_available();

} // namespace sopt

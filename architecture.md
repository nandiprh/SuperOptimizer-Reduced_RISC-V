# Architecture: Regression-Guided STOKE-style Superoptimizer for Reduced RV32I

Language: C++17 (search, interpreter, cost inference). Python only for offline regression training.
Build: plain Makefile, no dependencies except optional Z3 (stretch).

---

## 0. Scope in one paragraph

Given a loop-free, register-only RV32I basic block (the *target*), find a cheaper sequence with identical live-out register values for all live-in inputs. Search = Metropolis-Hastings MCMC (STOKE [1]). Cost = correctness distance + performance term. Performance term starts as instruction count, then is replaced by a regression model trained on cycle counts from the Bundling-simulator.

---

## 1. Honest risks (read first)

1. **Label quality.** With no loads/stores/branches/mul/div, every v1 instruction is a 1-cycle ALU op. Simulated cycles will be almost fully explained by `instruction_count` and `dependency_depth` (and issue width). The regression model may add nothing over those two numbers. **Mandatory check:** the model must beat the baseline `cycles ~ a*len + b*depth` on held-out MAE and rank correlation. If it doesn't, say so in the report. That is a valid result.
2. **Speed claim.** The proposal claims the model makes search "dramatically faster". For 1–16 ALU instructions, a simulator call may already be microseconds. **Measure** simulator-call latency vs. model inference latency before claiming anything.
3. **Distribution shift.** MCMC visits mostly *incorrect, weird* programs. A model trained only on tidy random sequences will mispredict there. Fix: iterative data collection (Section 6.5).
4. **Proposal features mismatch.** `memory_op_count` and `branch_count` are always 0 in v1. Drop them from the feature vector until those instructions exist.
5. **Interpreter bugs poison everything.** Validate against a real RV32I implementation (Section 8.1), not just hand-written tests.

---

## 2. Instruction set

### 2.1 v1 instruction list (register-only, loop-free)

| Class | Instructions | Semantics (all 32-bit, two's complement) |
|---|---|---|
| R-type arithmetic | `ADD SUB` | wraparound add/sub |
| R-type logic | `AND OR XOR` | bitwise |
| R-type shift | `SLL SRL SRA` | shift amount = `rs2[4:0]` (low 5 bits only); SRA arithmetic |
| R-type compare | `SLT SLTU` | result is 0 or 1; SLT signed, SLTU unsigned |
| I-type arithmetic | `ADDI` | imm 12-bit signed, sign-extended |
| I-type logic | `ANDI ORI XORI` | imm 12-bit signed, sign-extended (so `XORI rd,rs,-1` = bitwise NOT) |
| I-type shift | `SLLI SRLI SRAI` | shamt 0..31 |
| I-type compare | `SLTI SLTIU` | imm sign-extended *then* SLTIU compares as unsigned |
| U-type | `LUI` | `rd = imm20 << 12` |

20 opcodes total.

### 2.2 Deliberately excluded (and why)

| Excluded | Reason |
|---|---|
| `AUIPC` | reads PC; PC is meaningless for a block searched out of context, makes equivalence address-dependent |
| `BEQ BNE BLT BGE BLTU BGEU JAL JALR` | basic blocks have no internal control flow |
| `LB LH LW LBU LHU SB SH SW` | adds aliasing/memory-state equivalence; defer to v2 |
| `MUL MULH MULHSU MULHU DIV DIVU REM REMU` (M ext) | scope decision; also removes the only multi-cycle ALU ops |
| `FENCE ECALL EBREAK CSR*` | not computational |
| All F/D/Q | out of scope |

### 2.3 Semantics gotchas the interpreter must get right

- `x0` always reads 0; writes to `x0` are discarded. Mutator may still generate `rd = x0` (acts as a NOP, useful for deletion).
- Shifts use only the low 5 bits of the amount. `SLL rd, rs1, rs2` with `rs2 = 33` shifts by 1.
- `ADDI/ANDI/ORI/XORI/SLTI/SLTIU` immediates are sign-extended from 12 bits. `SLTIU rd, rs, 1` is `rs == 0` (the `SEQZ` idiom).
- `LUI` loads a 20-bit value into bits 31:12, low 12 bits zero.
- Building a 32-bit constant `C` with `LUI+ADDI`: because ADDI sign-extends, upper = `(C + 0x800) >> 12`, lower = `C & 0xFFF` interpreted as signed 12-bit.
- Signed overflow wraps. Do arithmetic in `uint32_t` to avoid C++ UB; cast to `int32_t` only for SLT/SRA.
- SRA on a negative `int32_t` is implementation-defined pre-C++20. Implement it explicitly via `uint32_t` arithmetic.

### 2.4 Pseudo-instructions to know (these appear in compiler output and as benchmark idioms)

| Pseudo | Expands to |
|---|---|
| `nop` | `addi x0,x0,0` |
| `mv rd,rs` | `addi rd,rs,0` |
| `not rd,rs` | `xori rd,rs,-1` |
| `neg rd,rs` | `sub rd,x0,rs` |
| `seqz rd,rs` | `sltiu rd,rs,1` |
| `snez rd,rs` | `sltu rd,x0,rs` |
| `sltz rd,rs` | `slt rd,rs,x0` |
| `sgtz rd,rs` | `slt rd,x0,rs` |
| `li rd,imm` | `addi` (fits 12-bit) or `lui`+`addi` |

The search never needs pseudos; the *front end* (gcc output parser) must expand them.

### 2.5 Optional stretch: bit-manipulation extensions

Zbb/Zba (ratified) give `ANDN ORN XNOR MIN MINU MAX MAXU ROL ROR RORI CLZ CTZ CPOP SEXT.B SEXT.H ZEXT.H SH1ADD SH2ADD SH3ADD`. Adding them makes superoptimization results far more interesting (e.g. `SH1ADD` replaces shift+add). But then the baseline compiler must be told to emit them (`-march=rv32i_zbb_zba`) or the comparison is unfair. Keep as v2; ISA table must be data-driven so this is a table edit, not a rewrite.

### 2.6 Representation

```cpp
enum class Op : uint8_t {
  NOP,                                   // fixed-length buffer filler (see 4.3)
  ADD, SUB, AND, OR, XOR, SLL, SRL, SRA, SLT, SLTU,
  ADDI, ANDI, ORI, XORI, SLLI, SRLI, SRAI, SLTI, SLTIU,
  LUI
};
enum class Fmt : uint8_t { R, I, IShift, U, None };

struct Insn { Op op; uint8_t rd, rs1, rs2; int32_t imm; };

struct OpInfo {            // single source of truth, one row per opcode
  const char* name; Fmt fmt; uint8_t latency;
  int32_t imm_min, imm_max;
};
extern const OpInfo kOps[];
```

All of mutator, printer, parser, feature extractor and SMT encoder read `kOps[]`. Do not scatter `switch (op)` over modules except in the interpreter and SMT encoder.

---

## 3. Program specification (target + interface)

A target is not just instructions. It needs:

```
live_in:  {a0, a1}          // registers whose initial values are inputs
live_out: {a0}              // registers whose final values must match
temps:    {t0..t6, a2..}    // registers the candidate may clobber
code:     ...
```

- Correctness compares **only live-outs**. Everything else is free scratch. Without this, the search is forced to reproduce dead values and finds nothing.
- Registers not in `live_in` are initialized to a fixed random constant per test case (so reading them is detectable, not silently 0).
- Restrict the register pool the mutator may use to `live_in ∪ live_out ∪ temps` (reduces the space a lot). Typical pool: 6–10 registers, not all 32.
- File format: tiny text format (`.sopt`) with the header above followed by assembly. Parser is ~100 lines.

---

## 4. Components

### 4.1 Interpreter (`interpreter.{hpp,cpp}`)

- `State` = `std::array<uint32_t,32>`.
- `run(const Insn*, int n, State&)` — one tight loop, one switch, no allocation, no exceptions.
- Perf target: >10 M instructions/s; it runs `num_tests × length` times per MCMC step.
- Optional later: JIT-free speedups = test-case batching in a structure-of-arrays layout (run one instruction across all test cases, auto-vectorized).

### 4.2 Cost function (`cost.{hpp,cpp}`)

```
cost(P) = w_c * eq(P) + w_p * perf(P)
```

**eq(P)**: for each test case *t*, for each live-out register *r*:
`popcount(P_out[t][r] XOR T_out[t][r])`, summed. This is STOKE's Hamming-distance-style graded correctness [1]. 0 ⇔ passes all tests.

Refinement from STOKE [1]: partial credit if the correct value appears in a *different* register (penalty for wrong location, smaller than penalty for wrong value). Implement after baseline works.

**perf(P)**:
- Phase A: `instruction_count` (excluding NOPs).
- Phase B: regression prediction from features (Section 6.4).
- Optional hybrid: `perf = predicted_cycles`, with final answers re-scored by the real simulator.

**Two-phase search (from STOKE [1])**:
1. *Synthesis phase:* `w_p = 0`. Start from random/NOP-filled program, look only for any correct program.
2. *Optimization phase:* start from a correct program (the target or synthesis result), `w_p > 0`, perturb while keeping eq at 0 as much as possible.

Early termination: if running tests in order and `eq` already exceeds the current accept threshold, stop early (reject without finishing the rest). Compute the threshold from the pre-drawn uniform random number: accept iff `cost_new < cost_old − ln(u)/β`. This is a large, easy speedup.

### 4.3 Mutator (`mutator.{hpp,cpp}`)

Use a **fixed-length buffer of L slots**, with `NOP` as the "unused" token (STOKE does this [1]). Then insert/delete are just "replace NOP with insn" / "replace insn with NOP", and all moves are symmetric, so the Metropolis acceptance needs no proposal-ratio correction.

Moves (pick one uniformly per iteration):

| Move | Action |
|---|---|
| Opcode | change opcode of slot *i* to another opcode with the **same format** (keeps operands valid) |
| Operand | change one of rd/rs1/rs2/imm of slot *i* (registers from the pool; imm from the constant pool) |
| Swap | swap slots *i* and *j* |
| Instruction | replace slot *i* with a fully random instruction (may be NOP) |

**Immediate pool** (do not draw uniformly from 2^12; the search would never hit useful values):
`{0, 1, -1, 2, 4, 8, 16, 31, 32, 0xFF, 0x7FF, -2048, powers of two, immediates appearing in the target, target immediates ±1}`, plus occasionally a uniform random draw.

L = target length (optimization phase) or target length + a few (synthesis phase).

### 4.4 MCMC driver (`mcmc.{hpp,cpp}`)

```
cur = init; cur_cost = cost(cur); best = cur
for i in 1..N:
    cand = mutate(cur)
    u = uniform(0,1)
    if cost(cand) < cur_cost - ln(u)/beta:     // Metropolis accept, with early-exit
        cur = cand; cur_cost = cost(cand)
        if cur_cost < best_cost and eq(cur) == 0 and verified_ok(cur): best = cur
```

- `beta`: fixed. Tune so the accept rate for cost-increasing moves is roughly a few percent to tens of percent; sweep β ∈ {0.1, 0.5, 1, 2, 5} on 3 benchmarks and pick empirically. (I'm not asserting STOKE's exact value; tune your own.)
- Run K independent chains with different seeds in parallel (`std::thread`), keep the best. Embarrassingly parallel.
- Log `(iteration, cost, eq, perf, accepted)` every N steps to CSV. You need this for the report plots and for the dataset in 6.5.

### 4.5 Verifier (`verify.{hpp,cpp}`)

- **Tier 1 (inner loop):** test cases inside `eq`. Test set generation: boundary values `{0, 1, -1, INT_MIN, INT_MAX, 0x55555555, 0xAAAAAAAA, 31, 32}` crossed over live-ins (capped), plus random 32-bit values. Start with 32–64 cases.
- **Counterexample loop (from STOKE [1]):** when a candidate with `eq == 0` is found, run Tier 2. If it fails, add the counterexample to the test set and resume search.
- **Tier 2 (exact):** Z3, `QF_BV` 32-bit bitvectors. Encode both programs as SSA bitvector expressions, assert `live_outs differ`, check `unsat`. Shifts: `bvshl x (bvand y 31)`. SLT: `ite (bvslt a b) 1 0`. If Z3 is unavailable, fall back to a large random test (10^6+) and **label the result "probabilistically verified", not "proven".**
- Never report a result as correct on Tier 1 alone.

### 4.6 Enumerative baseline (`enumerate.{hpp,cpp}`)

For lengths 1..k (k ≈ 3–4), enumerate all sequences over the register pool and immediate pool, test with a small test set, then verify survivors. Needed as: (a) a comparison point (Bansal & Aiken [4]), (b) ground truth for "is MCMC finding the true optimum on small targets". Prune: skip sequences writing a dead value, canonicalize operand order for commutative ops, skip `rd == rs1 == rs2` no-ops.

Search space size check: ~20 opcodes × pool³ × imm choices per slot; length 4 with an 8-register pool is already on the order of 10^10+. Prune or cap at length 3.

---

## 5. Cost oracle: Bundling-simulator

### 5.1 Modularize

Split into:
- `libsched` — pure functions: build dependency DAG → schedule (list scheduling for bundling, Tomasulo/ROB for dynamic) → return cycles. **No file I/O, no printing, no `exit()`, no globals** (use a context struct).
- `frontend` — existing CLI/file reading.

Target API:

```c
typedef struct { uint8_t op, rd, rs1, rs2; int32_t imm; } SimInsn;
typedef enum { MODE_DYNAMIC, MODE_BUNDLED } SimMode;
int sim_cycles(const SimInsn* seq, int n, SimMode mode, const SimConfig* cfg);
```

### 5.2 Decisions to fix up front

- **Which mode is the target machine?** Dynamic (Tomasulo) and static bundling rank sequences differently. Pick one as primary; record both columns in the dataset; evaluate the model on the primary only.
- Record the config (issue width, ROB size, #reservation stations, functional-unit latencies) in the dataset metadata. A model is only valid for the config it was trained on.
- Check fixed-array limits (max instructions, ROB depth) vs. your max length L. Make the cap explicit and `assert`.
- **Make latencies non-uniform if the simulator allows**, otherwise point 1 in Section 1 bites. At minimum, make the cost table configurable.

### 5.3 Sanity tests for the oracle (do before generating 100k labels)

- Empty/1-instruction sequences → expected cycles.
- Chain of n dependent ADDs vs. n independent ADDs: dependent must cost ≥ independent.
- Monotonicity: appending an instruction never decreases cycles.
- Determinism: same input → same output.

---

## 6. Dataset generation (for the regression model)

### 6.1 What a row is

```
(program, features, cycles_dynamic, cycles_bundled, source_tag, length, config_id)
```

### 6.2 Sources (mix these; no single source is sufficient)

| Source | Purpose | Share (suggested) |
|---|---|---|
| **A. Random generator** | broad coverage | ~40% |
| **B. Mutated benchmarks** (apply 1–10 random mutator moves to each benchmark program) | realistic structure + MCMC-like perturbation | ~30% |
| **C. Compiler-derived blocks** (`gcc`/`clang` `-O0` and `-O2`, `-march=rv32i -mabi=ilp32`, extract basic blocks, keep only those entirely within the v1 ISA; expand pseudos; drop blocks containing branches/loads/stores/mul) | realistic dependency patterns | ~15% (it will be a small pool after filtering; report how many you got) |
| **D. MCMC trajectory samples** (Section 6.5) | match the distribution the model is used on | ~15%, added iteratively |

### 6.3 Random generator design (source A)

Uniform random operands give unrealistically shallow dependency graphs. Control dependency structure explicitly:

```
for each instruction i in 0..n-1:
    op   = random opcode (uniform, or weighted so logic/arith/shift/compare are balanced)
    rd   = random from pool
    rs1  = with prob p_dep: a register written within the last w instructions
           else: random live-in or random pool register
    rs2  = same rule (R-type only)
    imm  = from the immediate pool (4.3)
```

- Sample `n` uniformly from 1..16 (or a distribution matching your benchmark lengths; Hacker's Delight programs are short).
- Sample `p_dep` per program from U(0, 1) so the dataset spans fully-parallel to fully-serial programs.
- Sample `w` per program from {1, 2, 4, 8}.
- Stratify: ensure each length 1..16 has ≥ some minimum count; ensure the dependency-depth/length ratio covers its range.
- Programs need not be *meaningful*, only valid. Do **not** require them to be correct for any target — the cost model never sees correctness.
- Deduplicate on canonical form (after register renaming by first-use order).

### 6.4 Features (computed in C++, identical code for training and inference)

Compute features in one C++ function used by both the dataset tool and the runtime cost function, to avoid train/serve skew. Python reads the CSV the C++ tool writes; do not reimplement features in Python.

| Feature | Definition |
|---|---|
| `n_insns` | non-NOP count |
| `hist[op]` | one counter per opcode (or per class: arith, logic, shift, compare, lui, to keep it small) |
| `dep_depth` | longest RAW chain via def-use DAG (DP in program order) |
| `n_live_regs_peak` | max simultaneously live registers (register-pressure proxy) |
| `n_independent_roots` | instructions with no in-block producer (ILP proxy) |
| `avg_dep_distance` | mean distance (in instructions) between producer and consumer |
| `n_waw`, `n_war` | name-dependence counts (matter if the simulator doesn't rename) |
| `max_width_estimate` | `n_insns / dep_depth` |

Drop `mem_ops` and `branches` for v1 (constant zero).

### 6.5 Iterative collection (fixes distribution shift)

1. Train model v0 on A + B + C.
2. Run MCMC on all benchmarks; log every accepted/proposed program (subsample).
3. Label those with the real simulator, add as source D.
4. Retrain → v1. Repeat 1–2 rounds. Report held-out error on source-D data, which is the honest measure of usefulness.

### 6.6 Splits (leakage control)

- Split **by program family**, not by row: all mutants of benchmark *k* go to the same split, otherwise test error is optimistic.
- 70/15/15 train/val/test. Keep one extra **MCMC-trajectory test set** that no training round touched.
- Dataset size: 50k–200k rows is plenty for tree models; label generation is cheap.

### 6.7 Training and export

- Python: `scikit-learn` `GradientBoostingRegressor` or `lightgbm`. Baselines to report against: (a) `n_insns`, (b) linear on `{n_insns, dep_depth}`, (c) linear on full features, (d) GBT.
- Metrics: MAE, MAPE, **Spearman rank correlation** (what matters for MCMC is correct *ordering* of nearby programs, i.e. the sign of Δcost, not absolute error), and *pairwise ordering accuracy* on pairs one mutation apart.
- Export: dump trees as a generated C++ header (`model_gen.hpp`): arrays of `{feature_idx, threshold, left, right, value}`. Inference = iterative traversal, no allocation. Add a unit test: C++ prediction equals Python prediction on 1000 rows to 1e-6.
- Pin the Python library versions in the repo.

---

## 7. Benchmarks

| Set | Use |
|---|---|
| Toy set (hand-written, 10–20) | cancelling ops (`+k, -k`), double negation, `x*2 → x<<1` shapes via `add x,x`, `(a+b)-b`, shift/mask patterns, abs, sign, min/max via SLT, `x & (x-1)` |
| Hacker's Delight p01–p25 | standard STOKE comparison set [1], originally from Gulwani et al. [6]; port each to v1 ISA; a few use ops outside v1 (mul etc.) — note which and either rewrite or exclude |
| Compiler-derived blocks | source C. Compile small C functions to `.s`, extract basic blocks |

Baselines: `riscv32-unknown-elf-gcc` / `clang --target=riscv32` at `-O0`, `-O2`, same `-march`. Compare: instruction count, simulated cycles (primary mode), and wall time to find the result.

---

## 8. Testing strategy

### 8.1 Interpreter conformance (do first)
- Differential test against a reference: assemble random v1 sequences with `riscv32-*-as`, run under `spike` or `qemu-riscv32`, compare final registers against your interpreter for thousands of random programs and inputs. This catches every shift/sign/immediate mistake.
- Directed tests for each gotcha in 2.3.

### 8.2 Unit tests per module
- Mutator: every output is a valid instruction (imm in range, registers in pool, shamt 0..31, format preserved by the opcode move).
- Cost: `eq(target) == 0`; `eq` of a known-wrong program > 0; early-exit gives same accept decisions as full evaluation.
- Verifier: Z3 and test-based verdicts agree on a set of known-equivalent / known-different pairs; SMT encoding is differential-tested against the interpreter on random inputs.
- Feature extractor: hand-computed `dep_depth` on 5–10 tiny programs.

### 8.3 End-to-end smoke test
`add t1,a,b; sub t2,t1,b; slli t3,t2,1` must reduce to a single instruction (`add t3,a,a`), verified. If this fails, nothing else matters yet.

---

## 9. Repository layout

```
Makefile
architecture.md
src/
  isa.hpp isa.cpp              // OpInfo table, Insn, printer, parser
  interpreter.hpp .cpp
  testgen.hpp .cpp             // test-case generation + counterexample store
  cost.hpp .cpp                // eq, perf, total
  features.hpp .cpp            // shared by runtime + dataset tool
  model_gen.hpp                // generated by tools/export_model.py
  mutator.hpp .cpp
  mcmc.hpp .cpp
  enumerate.hpp .cpp
  verify.hpp .cpp              // Tier 1 helpers + optional Z3
  frontend_asm.cpp             // parse gcc .s -> basic blocks -> Program
  main.cpp                     // CLI: superopt <file.sopt> [--beta --iters --chains --seed]
tools/
  gen_dataset.cpp              // sources A/B/C/D -> labeled CSV via libsched
  train_model.py
  export_model.py              // -> src/model_gen.hpp
third_party/
  bundling-simulator/          // libsched (modularized)
bench/
  toy/  hd/  compiler/         // .sopt files
tests/
  test_interpreter.cpp test_mutator.cpp test_cost.cpp test_verify.cpp
  diff_vs_spike.sh
results/                        // CSV logs, plots
```

---

## 10. Build order (each step has an exit test)

| # | Step | Exit test |
|---|---|---|
| 1 | `isa` + parser/printer | round-trip parse→print→parse |
| 2 | Interpreter | 8.1 differential test passes |
| 3 | Test generation + `eq` | `eq(target)=0`, wrong program `>0` |
| 4 | Mutator | validity property test |
| 5 | MCMC (synthesis + optimization phases), `perf = n_insns` | 8.3 smoke test passes |
| 6 | Verifier Tier 2 (Z3) + counterexample loop | known-bad candidate gets rejected and CE added |
| 7 | Toy + HD benchmarks, enumerative baseline | MCMC matches enumeration optimum on all length ≤3 targets |
| 8 | Modularize Bundling-simulator, oracle sanity tests (5.3) | all sanity tests pass |
| 9 | Dataset sources A/B/C, features, training, baseline comparison | model beats `n_insns` baseline on rank correlation (or report that it doesn't) |
| 10 | Export model, plug in as `perf`, measure latency and result quality | same-or-better results, latency measured |
| 11 | Iterative collection (6.5), retrain | error on MCMC-trajectory test set reported |
| 12 | gcc/clang comparison, plots, report | tables: insn count, cycles, time-to-result |

Do steps 1–7 before touching the regression layer. A working STOKE reproduction is the base; the regression model is an increment on top of it.

---

## 11. Evaluation protocol

- Per benchmark: best verified program, instruction count, simulated cycles (primary mode), seconds to first-correct, seconds to best, over ≥10 seeds (report median and range, not best-of).
- Compare: gcc -O0, gcc -O2, clang -O2, enumerative (where feasible), MCMC with `perf=n_insns`, MCMC with regression `perf`.
- Ablations: with/without two-phase search; with/without early-exit; β sweep; test-case count sweep; regression vs. simulator-in-loop (quality and speed).
- Report negative results (benchmarks where nothing improves on gcc -O2).

---

## 12. References

[1] E. Schkufza, R. Sharma, A. Aiken, "Stochastic Superoptimization," ASPLOS 2013. doi:10.1145/2451116.2451150
[2] C. Mendis et al., "Ithemal: Accurate, Portable and Fast Basic Block Throughput Estimation using Deep Neural Networks," ICML 2019.
[3] A. Wei et al., "SuperCoder: Assembly Program Superoptimization with Large Language Models," arXiv:2505.11480, 2025.
[4] S. Bansal, A. Aiken, "Automatic Generation of Peephole Superoptimizers," ASPLOS 2006. doi:10.1145/1168857.1168906
[5] P. M. Phothilimthana et al., "GreenThumb: Superoptimizer Construction Framework," UCB/EECS-2016-8.
[6] S. Gulwani, S. Jha, A. Tiwari, R. Venkatesan, "Synthesis of Loop-free Programs," PLDI 2011 (source of the 25 Hacker's Delight benchmark programs used by STOKE). Verify the exact benchmark list against this paper before relying on it.
[7] H. S. Warren Jr., *Hacker's Delight*, 2nd ed., Addison-Wesley, 2012.
[8] A. Waterman, K. Asanović (eds.), *The RISC-V Instruction Set Manual, Volume I: Unprivileged ISA*, RISC-V International (RV32I chapter; Zbb/Zba in the bit-manipulation extension spec).
[9] L. de Moura, N. Bjørner, "Z3: An Efficient SMT Solver," TACAS 2008.
[10] N. Metropolis et al., "Equation of State Calculations by Fast Computing Machines," J. Chem. Phys. 21, 1953; W. K. Hastings, "Monte Carlo Sampling Methods Using Markov Chains and Their Applications," Biometrika 57, 1970.

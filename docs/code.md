# Code Walkthrough

All sources live in `src/`. Build is `make` (plain Makefile, `g++ -std=c++17
-Wall -Wextra -O2 -g -Isrc`). Everything is in namespace `sopt`.

```
src/
  isa.{hpp,cpp}        Op/Insn definitions, kOps table, register map, printer, parser
  interpreter.{hpp,cpp}  State, run_instruction, run_program
  testgen.{hpp,cpp}      TestCase/ProgramSpec, RNG, boundary+random test cases
  cost.{hpp,cpp}         eq distance, perf, popcount
  mutator.{hpp,cpp}      random instruction generation + 4 mutation moves
  mcmc.{hpp,cpp}         Metropolis–Hastings search driver
  main.cpp               CLI: parse .sopt → test gen → MCMC → verify → report
```

Dependency order (bottom-up): `isa` → `interpreter` → `testgen`/`cost` →
`mutator` → `mcmc` → `main`.

---

## `isa.hpp` / `isa.cpp` — the instruction-set table

The module that everything else reads. Central definitions:

```cpp
enum class Op : uint8_t { NOP, ADD, SUB, AND, OR, XOR, SLL, SRL, SRA, SLT, SLTU,
                          ADDI, ANDI, ORI, XORI, SLLI, SRLI, SRAI, SLTI, SLTIU, LUI };
enum class Fmt : uint8_t { R, I, IShift, U, None };

struct Insn { Op op; uint8_t rd, rs1, rs2; int32_t imm; };

struct OpInfo { const char* name; Fmt fmt; uint8_t latency; int32_t imm_min, imm_max; };
```

`kOps[]` is the single source of truth: one row per opcode (`name`, `fmt`,
latency, valid immediate range), with a `static_assert` that there are exactly
21 entries. The mutator, printer, parser and cost function all read `kOps`
rather than scattering `switch (op)` everywhere.

Key functions:

- `reg_name_to_num(s)` — maps `xN`, ABI names (`a0`→10, `s0`/`fp`→8, `t3`→28, …)
  and plain indices to 0–31, or `-1` if invalid. Returns an `int` so callers can
  detect failure.
- `op_to_name` / `name_to_op` — case-insensitive name ↔ `Op`.
- `is_nop(i)` — true for a real `Op::NOP` **or** `addi x0,x0,0` (the RV32 `nop`
  pseudo-op), so both spellings act as buffer filler.
- `make_nop()` — canonical filler.
- `format_insn(i)` — prints `nop` for nops, else `op xRd, xRs1, xRs2` /
  `op xRd, xRs1, imm` / `op xRd, imm` depending on format.
- `parse_insn(line, out)` — the code-line parser:

  1. trim; drop blanks, `#`/`;` comments (also mid-line), `.`-directives and
     labels (a line whose trimmed tail is `:`).
  2. tokenise the opcode, then split the operand part on commas/spaces/tabs.
  3. dispatch on `kOps[op].fmt`: R/I/IShift take 3 operands, U takes 2, None
     takes none.
  4. assemble `Insn`, then validate: all of `rd/rs1/rs2` must be < 32, and I /
     IShift / U immediates must be within the opcode's `imm_min..imm_max`.
  5. normalise `addi x0,x0,0` to `Op::NOP`.

---

## `interpreter.hpp` / `interpreter.cpp` — bit-exact execution

```cpp
using State = std::array<uint32_t, 32>;
uint32_t run_instruction(const Insn& insn, State& state);
void     run_program(const Insn* prog, int n, State& state);
```

`run_instruction` is one big `switch (insn.op)` — a tight loop, no allocation,
no exceptions. Semantics that the docs care about:

- All arithmetic/logic in `uint32_t` → wraparound by construction, no UB from
  signed overflow. `SLT`/`SLTI` cast to `int32_t` for signed compares.
- Writes to `x0` are discarded (`if (insn.rd != 0)`).
- Register-form shifts mask the amount to 5 bits: `state[rs2] & 31u`.
- Immediate shifts mask `imm & 31u`.
- `sra32(x, shamt)` implements arithmetic shift right *explicitly* in
  `uint32_t` arithmetic (arch §2.3: pre-C++20 `>>` on negative `int32_t` is
  implementation-defined):
  ```cpp
  if (x & 0x80000000u) { sign = ~((1u << (32-shamt)) - 1); return (x >> shamt) | sign; }
  ```
- `SLTIU` compares `state[rs1] < (uint32_t)imm` where `imm` is the
  sign-extended 12-bit immediate.

---

## `testgen.hpp` / `testgen.cpp` — inputs and reference outputs

```cpp
struct TestCase { std::array<uint32_t,32> in; std::array<uint32_t,32> out_target; };

struct ProgramSpec {
  std::vector<Insn> code;      // the target
  std::vector<int>  live_in;   // registers carrying inputs
  std::vector<int>  live_out;  // registers that must match
  std::vector<int>  temps;     // scratch registers the search may use
  int max_len;                 // MCMC buffer length
};
```

- Global `std::mt19937`; `init_random_seed(s)` (0 → time-seeded), `rand32()`.
- `boundary_val(k)` cycles through the 8 boundary values
  `{0, 1, -1, INT_MIN, INT_MAX, 0x55555555, 0xAAAAAAAA, 31}`.
- `gen_test_case(target, live_in, live_out, idx, fixed_boundaries)`:
  1. fill all 32 registers with distinct random constants for this test case;
     `x0 = 0`.
  2. overwrite each live-in: boundary value for `idx < 64`, else random.
  3. run the target on that state; store live-out registers into `out_target`.

- `gen_test_set(...)` builds `count` cases.

Initialising *non-live-in* registers to random constants is deliberate: a
candidate that reads a register the target never wrote must reproduce that
random value to match, so such programs are penalised (arch §3).

---

## `cost.hpp` / `cost.cpp` — the objective

```cpp
long long popcount_xor(uint32_t a, uint32_t b);          // bit-count of a^b
long long eq_dist(prog, n, tests, live_out);             // Σ_t Σ_r popcount(P_t[r]⊕T_t[r])
long long cost_eq_only(...)                              // = eq_dist (eq weighting is not separately configurable)
long long perf_insn_count(prog, n);                      // non-NOP count
```

`eq_dist` re-executes the candidate on every test's input state (fresh copy of
`in`) and, for each live-out register, adds `popcount(candidate[r] ^ target[r])`.
Zero iff the candidate is equivalent to the target on all tests. `perf_insn_count`
counts instructions excluding NOPs.

---

## `mutator.hpp` / `mutator.cpp` — proposing new programs

Programs live in a **fixed-length buffer of `L = max_len` slots**, with `nop` as
the "unused" token. Mutations therefore preserve length, and insert/delete are
just "nop→insn" / "insn→nop".

- `choose_from_pool(spec)` — uniform pick from `live_in ∪ live_out ∪ temps`
  (fallback: uniform over 0–31 if the pool is empty).
- `choose_imm()` — draws from the curated immediate pool
  `{0, 1, -1, 2, 4, 8, 16, 31, 32, 255, 2047, -2048}` rather than uniform over
  2^12 (arch §4.3).
- `random_insn_same_fmt(spec, fmt)` — any opcode with the given format, filled
  with pool registers / pool immediates / shamt 0–31 / LUI imm.
- `random_insn(spec)` — any of the 20 real opcodes with random operands.
- `mutate_program(spec, buffer, len, probs)` — picks one move:

  | move | probability | action |
  |---|---|---|
  | opcode | 0.3 | rewrite slot *i* with a random op of the **same format** (operands stay valid) |
  | operand | 0.3 | change one field of slot *i* (rd/rs1/rs2/imm from pool) |
  | swap    | 0.2 | swap slots *i* and *j* |
  | replace | 0.2 | fill slot *i* with a random instruction (10% chance of `nop`, i.e. deletion) |

All four moves are **symmetric** (the reverse proposal has the same
probability), so the Metropolis–Hastings ratio reduces to `min(1, exp(-βΔC))` —
see [math.md](math.md#why-no-proposal-ratio).

---

## `mcmc.hpp` / `mcmc.cpp` — the search driver

```cpp
struct MCMCConfig { int max_iters; int chains; unsigned int seed; double beta;
                    int test_count; bool use_regression; };
struct MCMCResult { std::vector<Insn> best_prog; long long best_cost, best_eq, best_perf; double time_sec; };

MCMCResult run_mcmc(const ProgramSpec& spec, const std::vector<TestCase>& tests, const MCMCConfig& cfg);
```

`run_mcmc`:

1. `cur = [nop; nop; …; nop]` (length `max_len`), `cur_cost = total_cost(cur)`.
2. Loop `max_iters` times:
   - `cand = cur; mutate_program(spec, cand.data(), L);`
   - `cand_eq = eq_dist(...)`, `cand_perf = perf_insn_count(...)`,
     `cand_cost = 1000·cand_eq + cand_perf`.
   - **Metropolis accept:** if `cand_cost ≤ cur_cost` accept; else draw
     `u ~ Uniform(0,1)` and accept iff `u < exp(-β·(cand_cost - cur_cost))`.
   - on accept, move; remember the lowest-cost program seen so far.
3. Return the best program. `cfg.use_regression` is a no-op placeholder;
   `cfg.chains` is accepted but the loop runs one chain.

Because cost = `1000·eq + perf`, every correct program outranks every incorrect
one, so the low-cost optimum the chain samples around is the shortest *correct*
program (see [math.md](math.md#why-the-1000-weight-works)).

---

## `main.cpp` — CLI driver

1. Parse `--beta`, `--iters`, `--chains`, `--seed` (defaults 1.0, 10000, 1,
   12345). `argv[1]` is the `.sopt` path.
2. Parse the file into `ProgramSpec` (see [process.md](process.md#21-front-end--parsing)
   and [format.md](format.md)).
3. Derive `temps` = registers in the target code outside live_in/live_out.
4. `gen_test_set(..., 32, /*fixed_boundaries=*/true)` for search.
5. `run_mcmc(...)`.
6. Print `best_cost / best_eq / best_perf / time`, then the best program.
7. If `best_eq == 0`, **re-verify** on 10 000 fresh random tests
   (`fixed_boundaries=false`) and print `verify: OK (probabilistic)` or
   `verify: FAILED fresh_rand_eq=...`.
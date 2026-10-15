# The Expression Frontend (`--expr` and the REPL)

The frontend plays the role of *Clang → LLVM* for this superoptimizer: it lets you
type a small C-like expression and have it compiled to an RV32I basic block
(`ProgramSpec`) that is then handed to the MCMC search — no `.sopt` file needed.

| entry point | what it does |
|---|---|
| `./superopt --expr "a0 = (a0 ^ a1) & 0xFF;"` | one-shot: compile + optimize |
| `./superopt -e "…"` | alias for `--expr` |
| `./superopt` (no args) | interactive REPL; type blocks at the `sopt>` prompt |

## Language

A block is a sequence of statements of the form

```
<dst register> = <expression>;
```

e.g.

```c
t3 = (a0 + a1) - a1 << 1;
t1 = a0 + a1; t2 = t1 - a1; t3 = t2 << 1;
a0 = a0 < 10 ? ...          // (no ternary; see below)
```

### Registers

- Any named RV32I register is allowed as an operand: `a0`–`a7`, `t0`–`t6`,
  `s0`–`s11`, `x0`–`x31`, plus `zero/ra/sp/gp/tp/fp`.
- `x0` may be *read* (it is always 0) but cannot be an assignment target.
- Assignment target registers are not restricted to scratch registers; e.g.
  `a0 = f(a0)` writes the live-out through an extra `addi` if the computation was
  produced in a temp.

### Supported operators (C int semantics, `int32_t`)

| op | meaning |
|---|---|
| `+` `-` | add/sub (sub with small constant compiles to `addi` with negated imm) |
| `&` `\|` `^` | bitwise and/or/xor |
| `<<` `>>` | shift left / **arithmetic (signed) shift right** |
| `<` `<=` `>` `>=` | **signed** comparisons, produce 0/1 |
| `==` `!=` | equality / inequality, produce 0/1 |
| unary `-` `~` `!` | negate / bitwise not / logical not (0/1) |
| `( )` | grouping |
| integer constants | decimal or `0x…` |

Precedence (low → high): `|` 1, `^` 2, `&` 3, `== !=` 4, relational 5,
`<< >>` 6, `+ -` 7, `* / %` 8, unary, primary.

### Not supported (compile error)

- `*` `/` `%` — the search ISA is the register-only RV32I **I** subset, no M
  extension: `unsupported operator '*' (no M extension: */% unsupported)`.
- `&&`/`||` tokens are lexed but not valid operators.
- Ternary, calls, memory, branches — everything is straight-line register code.
- Shift amounts are range-checked to `[0, 31]` (constant) or masked to 5 bits
  (register).

## How a block becomes a `ProgramSpec`

Compilation follows the ABI calling convention for live-ins/live-outs:

- **`live_in`**: every register that is *read before it is written* in the block.
  In `a0 = 1 < a0;` the register `a0` is an input — it is *not* dropped just
  because the statement also writes it (each statement keeps its earlier reads).
- **`live_out`**: the destination of the **last** statement.
- Registers read only after being written by an earlier statement are internal.
- Intermediate values are placed in the scratch pool in priority order and the
  final result is moved into the statement target.

Scratch pool (search space registers that are *not* live):

```
t1 t2 t3 t4 t5 t6  a7 a6 a5 a4 a3 a2  s1 s10 s11
x6 x7 x28..x31     x17 x16 x15 x14 x13 x12 x11 x9 x26 x25
```

So a REPL session for `a0 = 1 < a0;` produces:

```text
target program (3 insns):
  addi x7, x0, 1
  slt x6, x7, x10
  addi x10, x6, 0
```

with `live_in = {a0}`.

### Codegen notes

- Constants in `[-2048,2047]` become a single `addi`; anything else uses the
  `lui` + `addi` idiom.
- `<<` → `slli`/`sll`; `>>` → `srai`/`sra` (arithmetic).
- `x < c` → `slti`; `x <= c` → `slti` + `xori 1`; `c < x` materializes `c` and
  uses `slt` (SLTI cannot take the immediate on the left); `>=`/`<=` invert.
- `x == 0`/`x != 0` use `xori` + `sltiu`/`sltu`.
- `a0 = a0;`-style identities compress away in the optimizer (`simple.sopt`
  degenerates to zero instructions).

## REPL reference

```
sopt> t3 = (a0 + a1) - a1 << 1;
best_cost=1 best_eq=0 best_perf=1 time=0.0251
target program (3 insns):
  add x6, x10, x11
  sub x7, x6, x11
  slli x28, x7, 1
optimized program (1 insn):
  add x28, x10, x10
verify: OK (probabilistic) fresh_rand_eq=0
sopt> :iters 200000
sopt> :help
```

Commands:

| command | meaning |
|---|---|
| `:iters N` | set MCMC iterations (default 10000) |
| `:beta B`  | set inverse temperature (default 1.0) |
| `:seed S`  | set RNG seed (default 12345) |
| `:show`    | print current settings |
| `:help`    | list commands |
| `:quit` / `:q` / `:exit` | leave the REPL |

A block may span multiple lines: lines without a trailing `;` accumulate until a
statement ends with `;`. Comments (`#` or `//`) are stripped.

## Correctness caveats

- The optimized result is equivalence-checked **probabilistically**
  (4096 uniform-random + 4096 small-signed-integer inputs). The target itself is
  compiled by this frontend and is trusted; miscompiles in the codegen would be
  faithfully optimized. See `src/frontend.cpp` for the emitter.
- Interpretation of live-ins for comparison-heavy expressions matters: a pure
  random test set is blind to rewrites that only differ on a small range (e.g.
  `a<=5` vs `a<31`), so verification also samples `[-2048,2047]` densely and the
  search itself is scored on 96 boundary-heavy tests (values in `[0,31] ∪
  [-16,-1] ∪ extremes). Always read `best_eq`/`verify:` before trusting a result.
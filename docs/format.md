# The `.sopt` File Format

A `.sopt` file describes a superoptimization *target*: the registers that carry
the program's inputs and outputs, plus the basic block to beat. It is plain
text, parsed by `src/main.cpp` (header) and `src/isa.cpp` (`parse_insn`, code
lines).

## Grammar

```text
file        := header code
header      := {live_in | live_out}*
live_in     := "live_in:"  reg-list
live_out    := "live_out:" reg-list
code        := "code:" {instruction-line | blank | comment}
instruction := opcode operands  ["# comment"]
```

- `reg-list` is a comma-separated set, optionally wrapped in `{ }`.
- A register may be written as `xN` (`x10`), an ABI name (`a0`, `t1`, `s2`, …),
  or a bare integer register index. They map onto the internal 0–31 numbering.
- Blank lines and lines starting with `#` or `;` are ignored everywhere.
- Code lines support trailing `#` / `;` comments. Lines starting with `.`
  (assembly directives) or ending with `:` (labels) are skipped.
- Unrecognised code lines are reported as warnings and skipped.

## Register names

| ABI | xN | index |
|---|---|---|
| zero | x0 | 0 |
| ra   | x1 | 1 |
| sp   | x2 | 2 |
| gp   | x3 | 3 |
| tp   | x4 | 4 |
| t0–t2 | x5–x7 | 5–7 |
| s0/fp, s1 | x8–x9 | 8–9 |
| a0–a7 | x10–x17 | 10–17 |
| s2–s11 | x18–x27 | 18–27 |
| t3–t6 | x28–x31 | 28–31 |

## Instructions

All 20 VM opcodes plus `nop`:

```text
R-type:  add sub and or xor sll srl sra slt sltu     rd, rs1, rs2
I-type:  addi andi ori xori slti sltiu               rd, rs1, imm      # imm ∈ [-2048, 2047]
IShift:  slli srli srai                              rd, rs1, shamt    # shamt ∈ [0, 31]
U-type:  lui                                       rd, imm             # imm ∈ [0, 0xFFFFF]
Filler:  nop
```

Immediates accept decimal or `0x`/`0X` hex, with optional sign. Register operands
accept any of the three notations.

## Example

```text
# smoke test from bench/toy/smoke.sopt
live_in: {a0, a1}     # inputs in x10, x11
live_out: {t3}        # only t3 (x28) matters afterwards
code:
add t1, a0, a1        # t1 = a0 + a1
sub t2, t1, a1        # t2 = t1 - a1 = a0
slli t3, t2, 1        # t3 = a0 << 1
```

The optimal result the search finds for this target is a single instruction:

```text
add x28, x10, x10     # t3 = a0 + a0 = a0 << 1
```

## Semantics you must get right when writing targets

- `x0` reads as 0 and discards writes. `addi x0, x0, 0` is normalised to `nop`.
- Shifts use only the low 5 bits of the amount (`SLL rd, x, y` with `y` containing
  33 shifts by 1).
- I‑type immediates are sign-extended from 12 bits.
- `SLTIU rd, rs, 1` is the `SEQZ` idiom (`rd = (rs == 0)`).
- `LUI rd, imm` sets bits 31:12 to `imm`; low 12 bits are zero.
- Arithmetic wraps on 32 bits (two's complement).

Registered pools: the search is restricted to `live_in ∪ live_out ∪ {registers
the target touches}`, so a target that should be optimised into scratch
registers should reference those registers itself if you want the search to use
them. See [process.md](process.md#21-front-end--parsing).
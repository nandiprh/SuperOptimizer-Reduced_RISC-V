# Instruction Set Reference (v1)

Reduced RV32I subset for the superoptimizer. Register-only, loop-free, integer-only.
20 opcodes plus an internal `NOP`.

Source: *The RISC-V Instruction Set Manual, Volume I: Unprivileged ISA*, RISC-V International (RV32I chapter).

---

## 1. Conventions

| Item | Rule |
|---|---|
| Registers | 32 registers, 32 bits each. `x0` always reads 0 and ignores writes. |
| Operands | `op rd, rs1, rs2` (R-type) or `op rd, rs1, imm` (I-type). `rd` = destination, `rs1`/`rs2` = inputs. |
| Immediates | I-type constants are 12-bit signed (-2048 to 2047), sign-extended to 32 bits before use. |
| Overflow | Arithmetic wraps in 32 bits. No traps. |
| Shift amount | Register shifts use only the low 5 bits of the amount (shift by 33 = shift by 1). Constant shifts are 0 to 31. |
| Names used in examples | `a0, a1, a2` inputs/outputs. `t0..t4` temporaries. |
| `x0` as destination | Acts as a no-op. The mutator can use it to delete an instruction. |

---

## 2. Instructions

### Arithmetic

| Opcode | Format | Operation | Example | Result |
|---|---|---|---|---|
| `ADD` | R | `rd = rs1 + rs2` | `add t1, a0, a1` | t1 = a0 + a1 |
| `SUB` | R | `rd = rs1 - rs2` | `sub t2, t1, a1` | t2 = t1 - a1 |
| `ADDI` | I | `rd = rs1 + imm` | `addi t0, a0, 5` | t0 = a0 + 5 |

`ADDI` also copies a register (`addi rd, rs, 0`) and loads a small constant (`addi rd, x0, 11`).

### Logic (bit by bit)

| Opcode | Format | Operation | Example | Result |
|---|---|---|---|---|
| `AND` | R | `rd = rs1 & rs2` | `and t0, a0, a1` | bits set in both |
| `OR` | R | `rd = rs1 \| rs2` | `or t0, a0, a1` | bits set in either |
| `XOR` | R | `rd = rs1 ^ rs2` | `xor t0, a0, a0` | t0 = 0 |
| `ANDI` | I | `rd = rs1 & imm` | `andi t0, a0, 255` | low 8 bits of a0 |
| `ORI` | I | `rd = rs1 \| imm` | `ori t0, a0, 1` | a0 with bit 0 set |
| `XORI` | I | `rd = rs1 ^ imm` | `xori t0, a0, -1` | t0 = NOT a0 |

### Shifts

| Opcode | Format | Operation | Example | Result |
|---|---|---|---|---|
| `SLL` | R | `rd = rs1 << (rs2 & 31)` | `sll t0, a0, a1` | a0 x 2^a1 |
| `SRL` | R | `rd = rs1 >> (rs2 & 31)`, fills 0 | `srl t0, a0, a1` | unsigned divide by 2^a1 |
| `SRA` | R | `rd = rs1 >> (rs2 & 31)`, keeps sign | `sra t0, a0, a1` | signed divide by 2^a1 (rounds down) |
| `SLLI` | I | `rd = rs1 << shamt` | `slli t3, t2, 1` | t3 = t2 x 2 |
| `SRLI` | I | `rd = rs1 >> shamt`, fills 0 | `srli t0, a0, 8` | a0 / 256, unsigned |
| `SRAI` | I | `rd = rs1 >> shamt`, keeps sign | `srai t0, a0, 31` | 0 if a0 >= 0, -1 if a0 < 0 |

### Compare (result is 1 or 0)

| Opcode | Format | Operation | Example | Result |
|---|---|---|---|---|
| `SLT` | R | `rd = (rs1 < rs2) ? 1 : 0`, signed | `slt t0, a0, a1` | 1 if a0 < a1 |
| `SLTU` | R | same, unsigned | `sltu t0, x0, a0` | 1 if a0 != 0 |
| `SLTI` | I | signed, against a constant | `slti t0, a0, 0` | 1 if a0 is negative |
| `SLTIU` | I | unsigned; imm sign-extended first | `sltiu t0, a0, 1` | 1 if a0 == 0 |

### Constant loading

| Opcode | Format | Operation | Example | Result |
|---|---|---|---|---|
| `LUI` | U | `rd = imm20 << 12` | `lui t0, 1` | t0 = 4096 |

### Internal (not a real RISC-V instruction)

| Opcode | Meaning |
|---|---|
| `NOP` | Filler that keeps candidate sequences a fixed length. The search swaps it in to "delete" an instruction. Not counted in instruction count. |

---

## 3. Semantics that are easy to get wrong

- `x0` reads 0 and discards writes.
- Register shifts take the low 5 bits of the amount only.
- `ADDI`, `ANDI`, `ORI`, `XORI`, `SLTI`, `SLTIU` sign-extend the 12-bit immediate. `XORI rd, rs, -1` is bitwise NOT.
- `SLTIU rd, rs, 1` tests `rs == 0`, because the immediate is sign-extended and then compared unsigned.
- `LUI` sets bits 31:12 and zeroes the low 12 bits.
- Building a 32-bit constant `C` with `LUI` + `ADDI`: ADDI sign-extends, so use `upper = (C + 0x800) >> 12` and `lower = C & 0xFFF` read as a signed 12-bit value.
- Do arithmetic in `uint32_t` in C++ to avoid signed-overflow undefined behaviour. Cast to `int32_t` only for `SLT`, `SLTI`, `SRA`, `SRAI`.
- `SRA` on a negative `int32_t` is implementation-defined before C++20. Implement it explicitly.

---

## 4. Pseudo-instructions (appear in compiler output)

The search never uses these. The compiler-output parser must expand them.

| Pseudo | Expands to |
|---|---|
| `nop` | `addi x0, x0, 0` |
| `mv rd, rs` | `addi rd, rs, 0` |
| `not rd, rs` | `xori rd, rs, -1` |
| `neg rd, rs` | `sub rd, x0, rs` |
| `seqz rd, rs` | `sltiu rd, rs, 1` |
| `snez rd, rs` | `sltu rd, x0, rs` |
| `sltz rd, rs` | `slt rd, rs, x0` |
| `sgtz rd, rs` | `slt rd, x0, rs` |
| `li rd, imm` | `addi` if it fits 12 bits, else `lui` + `addi` |

---

## 5. Excluded from v1

| Left out | Why |
|---|---|
| `AUIPC` | Reads the program counter, which has no stable meaning for a block searched out of context. |
| `BEQ BNE BLT BGE BLTU BGEU JAL JALR` | Basic blocks have no internal control flow. |
| `LB LH LW LBU LHU SB SH SW` | Need memory-state equivalence checks. Deferred to v2. |
| `MUL MULH MULHSU MULHU DIV DIVU REM REMU` | M extension, excluded by scope. Also removes the only multi-cycle ALU ops. |
| `FENCE ECALL EBREAK CSR*` | Not computational. |
| `F`, `D`, `Q` extensions | Excluded by scope. |

---

## 6. C++ representation

```cpp
enum class Op : uint8_t {
  NOP,
  ADD, SUB, AND, OR, XOR, SLL, SRL, SRA, SLT, SLTU,
  ADDI, ANDI, ORI, XORI, SLLI, SRLI, SRAI, SLTI, SLTIU,
  LUI
};
enum class Fmt : uint8_t { R, I, IShift, U, None };

struct Insn { Op op; uint8_t rd, rs1, rs2; int32_t imm; };

struct OpInfo {            // one row per opcode: single source of truth
  const char* name; Fmt fmt; uint8_t latency;
  int32_t imm_min, imm_max;
};
extern const OpInfo kOps[];
```

Mutator, printer, parser, feature extractor and SMT encoder all read `kOps[]`. Only the interpreter and the SMT encoder have their own `switch (op)`.

---

## 7. Worked example

```
add  t1, a0, a1     # t1 = a + b
sub  t2, t1, a1     # t2 = (a + b) - b = a
slli t3, t2, 1      # t3 = a * 2
```

reduces to one instruction with the same result in `t3`:

```
add  t3, a0, a0     # t3 = a + a
```

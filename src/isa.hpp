#pragma once

#include <cstdint>
#include <string>
#include <array>

namespace sopt {

enum class Op : uint8_t {
  NOP,                                   // filler
  ADD, SUB, AND, OR, XOR, SLL, SRL, SRA, SLT, SLTU,
  ADDI, ANDI, ORI, XORI, SLLI, SRLI, SRAI, SLTI, SLTIU,
  LUI
};

enum class Fmt : uint8_t { R, I, IShift, U, None };

struct Insn {
  Op op;
  uint8_t rd;
  uint8_t rs1;
  uint8_t rs2;
  int32_t imm;
};

struct OpInfo {
  const char* name;
  Fmt fmt;
  uint8_t latency;
  int32_t imm_min;
  int32_t imm_max;
};

extern const OpInfo kOps[];
extern const std::array<const char*, 32> kRegNames;

std::string op_to_name(Op op);
Op name_to_op(const std::string& name);
bool is_valid_reg(uint8_t r);
int reg_name_to_num(const std::string& s);
bool is_nop(const Insn& insn);
Insn make_nop();

std::string format_insn(const Insn& insn);
bool parse_insn(const std::string& line, Insn& out);

} // namespace sopt

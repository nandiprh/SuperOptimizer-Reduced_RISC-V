#include "interpreter.hpp"
#include <algorithm>

namespace sopt {

// SRA on signed int32_t must be explicit for pre-C++20 portability in behavior; implement via uint32_t
static inline uint32_t sra32(uint32_t x, uint32_t shamt) {
  if (shamt == 0) return x;
  shamt &= 31;
  if (x & 0x80000000u) {
    uint32_t sign = ~((1u << (32 - shamt)) - 1);
    return (x >> shamt) | sign;
  } else {
    return x >> shamt;
  }
}

uint32_t run_instruction(const Insn& insn, State& state) {
  // writeback target
  switch (insn.op) {
    case Op::NOP: {
      return state[0];
    }
    case Op::ADD: {
      uint32_t rdv = state[insn.rs1] + state[insn.rs2];
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::SUB: {
      uint32_t rdv = state[insn.rs1] - state[insn.rs2];
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::AND: {
      uint32_t rdv = state[insn.rs1] & state[insn.rs2];
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::OR: {
      uint32_t rdv = state[insn.rs1] | state[insn.rs2];
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::XOR: {
      uint32_t rdv = state[insn.rs1] ^ state[insn.rs2];
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::SLL: {
      uint32_t shamt = state[insn.rs2] & 31u;
      uint32_t rdv = state[insn.rs1] << shamt;
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::SRL: {
      uint32_t shamt = state[insn.rs2] & 31u;
      uint32_t rdv = state[insn.rs1] >> shamt;
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::SRA: {
      uint32_t shamt = state[insn.rs2] & 31u;
      uint32_t rdv = sra32(state[insn.rs1], shamt);
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::SLT: {
      int32_t a = static_cast<int32_t>(state[insn.rs1]);
      int32_t b = static_cast<int32_t>(state[insn.rs2]);
      uint32_t rdv = (a < b) ? 1u : 0u;
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::SLTU: {
      uint32_t rdv = (state[insn.rs1] < state[insn.rs2]) ? 1u : 0u;
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::ADDI: {
      // imm is sign-extended 12-bit
      int32_t imm = insn.imm; // passed as int32_t
      uint32_t rdv = state[insn.rs1] + static_cast<uint32_t>(imm);
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::ANDI: {
      int32_t imm = insn.imm;
      uint32_t rdv = state[insn.rs1] & static_cast<uint32_t>(imm);
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::ORI: {
      int32_t imm = insn.imm;
      uint32_t rdv = state[insn.rs1] | static_cast<uint32_t>(imm);
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::XORI: {
      int32_t imm = insn.imm;
      uint32_t rdv = state[insn.rs1] ^ static_cast<uint32_t>(imm);
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::SLLI: {
      uint32_t shamt = static_cast<uint32_t>(insn.imm) & 31u;
      uint32_t rdv = state[insn.rs1] << shamt;
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::SRLI: {
      uint32_t shamt = static_cast<uint32_t>(insn.imm) & 31u;
      uint32_t rdv = state[insn.rs1] >> shamt;
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::SRAI: {
      uint32_t shamt = static_cast<uint32_t>(insn.imm) & 31u;
      uint32_t rdv = sra32(state[insn.rs1], shamt);
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::SLTI: {
      int32_t a = static_cast<int32_t>(state[insn.rs1]);
      int32_t imm = insn.imm;
      uint32_t rdv = (a < imm) ? 1u : 0u;
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::SLTIU: {
      uint32_t a = state[insn.rs1];
      uint32_t imm = static_cast<uint32_t>(insn.imm); // sign-extended imm as 32-bit mask value
      uint32_t rdv = (a < imm) ? 1u : 0u;
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    case Op::LUI: {
      uint32_t rdv = static_cast<uint32_t>(insn.imm) << 12;
      if (insn.rd != 0) state[insn.rd] = rdv;
      return rdv;
    }
    default: {
      return state[0];
    }
  }
}

void run_program(const Insn* prog, int n, State& state) {
  for (int i = 0; i < n; i++) {
    run_instruction(prog[i], state);
  }
}

} // namespace sopt

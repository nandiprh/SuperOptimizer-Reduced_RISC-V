#include "frontend.hpp"
#include <cctype>
#include <cstdint>
#include <vector>

namespace sopt {

namespace {

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

enum class TokKind { Num, Reg, Op, LParen, RParen, Semi, End };

struct Token {
  TokKind kind;
  std::string text;
  long long val = 0;   // for Num
  int reg = -1;        // for Reg
};

static const char* kTwoCharOps[] = {"<<", ">>", "<=", ">=", "==", "!=", "&&", "||"};

static bool is_op_char(char c) {
  switch (c) {
    case '+': case '-': case '*': case '/': case '%':
    case '&': case '|': case '^': case '~': case '!':
    case '<': case '>': case '=': return true;
    default: return false;
  }
}

static std::vector<Token> tokenize(const std::string& src) {
  std::vector<Token> out;
  size_t pos = 0;
  while (pos < src.size()) {
    char c = src[pos];
    if (std::isspace(static_cast<unsigned char>(c))) { pos++; continue; }
    if (c == '#' || (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/')) {
      while (pos < src.size() && src[pos] != '\n') pos++;
      continue;
    }
    bool matched = false;
    for (const char* s : kTwoCharOps) {
      if (src.compare(pos, 2, s) == 0) {
        out.push_back({TokKind::Op, s});
        pos += 2;
        matched = true;
        break;
      }
    }
    if (matched) continue;
    if (c == '(') { out.push_back({TokKind::LParen, "("}); pos++; continue; }
    if (c == ')') { out.push_back({TokKind::RParen, ")"}); pos++; continue; }
    if (c == ';') { out.push_back({TokKind::Semi, ";"}); pos++; continue; }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      bool hex = false;
      size_t start = pos;
      if (pos + 1 < src.size() && c == '0' && (src[pos + 1] == 'x' || src[pos + 1] == 'X')) {
        hex = true;
        pos += 2;
      }
      long long v = 0;
      bool any = false;
      while (pos < src.size()) {
        char d = src[pos];
        int dv = -1;
        if (std::isdigit(static_cast<unsigned char>(d))) dv = d - '0';
        else if (hex && d >= 'a' && d <= 'f') dv = 10 + (d - 'a');
        else if (hex && d >= 'A' && d <= 'F') dv = 10 + (d - 'A');
        if (dv < 0) break;
        v = v * (hex ? 16 : 10) + dv;
        any = true;
        pos++;
      }
      if (any) {
        out.push_back({TokKind::Num, src.substr(start, pos - start), v});
        continue;
      }
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
      size_t start = pos;
      while (pos < src.size() &&
             (std::isalnum(static_cast<unsigned char>(src[pos])) || src[pos] == '_' || src[pos] == '$'))
        pos++;
      std::string name = src.substr(start, pos - start);
      int r = reg_name_to_num(name);
      if (r >= 0) out.push_back({TokKind::Reg, name, 0, r});
      else out.push_back({TokKind::Op, name});  // unknown identifier → error later
      continue;
    }
    if (is_op_char(c)) { out.push_back({TokKind::Op, std::string(1, c)}); pos++; continue; }
    out.push_back({TokKind::Op, "?"});
    pos++;
  }
  out.push_back({TokKind::End, ""});
  return out;
}

// ---------------------------------------------------------------------------
// Operand representation + code generator
// ---------------------------------------------------------------------------

struct Operand {
  enum Kind { Reg, Const } kind;
  uint8_t reg = 0;
  int32_t val = 0;
  static Operand mk_reg(uint8_t r) { return {Reg, r, 0}; }
  static Operand mk_const(int32_t v) { return {Const, 0, v}; }
};

// Scratch registers in priority order (t1,t2,t3..t6, a7..a2, s1, s10/s11).
const int kScratch[] = {6, 7, 28, 29, 30, 31, 17, 16, 15, 14, 13, 12, 11, 9, 26, 25};

static int op_prec(const std::string& op) {
  if (op == "|") return 1;
  if (op == "^") return 2;
  if (op == "&") return 3;
  if (op == "==" || op == "!=") return 4;
  if (op == "<" || op == "<=" || op == ">" || op == ">=") return 5;
  if (op == "<<" || op == ">>") return 6;
  if (op == "+" || op == "-") return 7;
  if (op == "*" || op == "/" || op == "%") return 8;
  return -1;
}

struct Compiler {
  std::vector<Insn>& code;
  std::vector<int>& live_in;
  std::vector<int>& written;
  std::string* err;

  bool used[32] = {false};
  int scratch_next = 0;

  Compiler(std::vector<Insn>& c, std::vector<int>& li, std::vector<int>& w, std::string* e)
      : code(c), live_in(li), written(w), err(e) {}

  bool fail(const std::string& msg) {
    if (err) *err = msg;
    code.clear();
    return false;
  }

  bool was_written(int r) const {
    for (int x : written) if (x == r) return true;
    return false;
  }

  // Note a register read; if it has not been produced earlier in the block it
  // becomes a live_in input.
  void read_reg(int r) {
    if (r == 0 || was_written(r)) return;
    for (int x : live_in) if (x == r) return;
    live_in.push_back(r);
    used[r] = true;
  }

  void mark_written(int r) {
    for (int i = 0; i < (int)written.size(); i++)
      if (written[i] == r) return;
    written.push_back(r);
    used[r] = true;
  }

  uint8_t alloc_temp() {
    for (int i = 0; i < 16; i++) {
      uint8_t r = static_cast<uint8_t>(kScratch[(scratch_next + i) % 16]);
      if (!used[r] && !was_written(r)) {
        scratch_next = (scratch_next + i + 1) % 16;
        used[r] = true;
        return r;
      }
    }
    return 0;  // callers treat 0 as "fail"
  }

  void emit(Op op, uint8_t rd, uint8_t rs1, uint8_t rs2, int32_t imm) {
    code.push_back(Insn{op, rd, rs1, rs2, imm});
  }

  // Materialize any 32-bit constant (LUI+ADDI idiom for out-of-12-bit values).
  void materialize_const(uint8_t rd, int32_t c) {
    if (c >= -2048 && c <= 2047) { emit(Op::ADDI, rd, 0, 0, c); return; }
    int64_t v = c;
    int32_t lo = static_cast<int32_t>(v & 0xFFF);
    if (lo > 0x7FF) lo -= 0x1000;
    int64_t hi = v >> 12;
    if ((v & 0x800) != 0) hi += 1;
    emit(Op::LUI, rd, 0, 0, static_cast<int32_t>(hi & 0xFFFFF));
    if (lo != 0) emit(Op::ADDI, rd, rd, 0, lo);
  }

  // Turn an operand into a register (optionally a specific one).
  Operand to_reg(Operand o, uint8_t forced = 0) {
    if (o.kind == Operand::Reg) {
      if (forced != 0 && forced != o.reg) {
        emit(Op::ADDI, forced, o.reg, 0, 0);
        return Operand::mk_reg(forced);
      }
      return o;
    }
    uint8_t r = forced != 0 ? forced : alloc_temp();
    if (r == 0) return Operand::mk_const(o.val);  // report via caller check
    materialize_const(r, o.val);
    return Operand::mk_reg(r);
  }
};

} // namespace

// ---------------------------------------------------------------------------
// Parser + code generation of the whole block
// ---------------------------------------------------------------------------

namespace {

struct Parser {
  const std::vector<Token>& toks;
  size_t cur = 0;
  Compiler cc;

  Parser(const std::vector<Token>& t, std::vector<Insn>& code,
         std::vector<int>& li, std::vector<int>& w, std::string* e)
      : toks(t), cc(code, li, w, e) {}

  const Token& peek() const { return toks[cur]; }
  const Token& next() { return toks[cur++]; }

  bool expect(TokKind k, const char* what, std::string& e) {
    if (peek().kind != k) { e = std::string("expected ") + what + " at '" + peek().text + "'"; return false; }
    return true;
  }

  Operand parse_primary(std::string& e) {
    const Token& t = peek();
    if (t.kind == TokKind::Num) {
      next();
      return Operand::mk_const(static_cast<int32_t>(t.val & 0xFFFFFFFF));
    }
    if (t.kind == TokKind::Reg) {
      next();
      cc.read_reg(t.reg);
      return Operand::mk_reg(static_cast<uint8_t>(t.reg));
    }
    if (t.kind == TokKind::LParen) {
      next();
      Operand o = parse_expr(0, e);
      if (!e.empty()) return o;
      if (peek().kind != TokKind::RParen) { e = "expected ')'"; return o; }
      next();
      return o;
    }
    e = "expected number or register, got '" + t.text + "'";
    return Operand::mk_const(0);
  }

  Operand parse_unary(std::string& e) {
    const std::string& op = peek().text;
    if (peek().kind == TokKind::Op && op_prec(op) >= 0 && (op == "-" || op == "~" || op == "!")) {
      next();
      Operand a = parse_unary(e);
      if (!e.empty()) return a;
      uint8_t rd = cc.alloc_temp();
      if (rd == 0) { e = "ran out of scratch registers"; return a; }
      if (a.kind == Operand::Const) {
        int32_t v = a.val;
        if (op == "-") v = -v;
        else if (op == "~") v = ~v;
        else v = (v == 0) ? 1 : 0;
        cc.materialize_const(rd, v);
        return Operand::mk_reg(rd);
      }
      if (op == "-") cc.emit(Op::SUB, rd, 0, a.reg, 0);
      else if (op == "~") cc.emit(Op::XORI, rd, a.reg, 0, -1);
      else cc.emit(Op::SLTIU, rd, a.reg, 0, 1);  // !x == (x == 0)
      return Operand::mk_reg(rd);
    }
    return parse_primary(e);
  }

  // Emit a comparison whose result is 0/1 in rd. Signed semantics (C int).
  void emit_cmp(const std::string& op, Operand a, Operand b, uint8_t rd) {
    if (op == "==" || op == "!=") {
      if (a.kind == Operand::Const && b.kind == Operand::Const) {
        bool eq = (a.val == b.val);
        cc.materialize_const(rd, (eq == (op == "==")) ? 1 : 0);
        return;
      }
      if (a.kind == Operand::Const) std::swap(a, b);   // b becomes the const
      uint8_t t = cc.alloc_temp();
      if (t == 0) t = rd;
      if (b.kind == Operand::Const) {
        if (b.val >= -2048 && b.val <= 2047) {
          cc.emit(Op::XORI, t, a.reg, 0, b.val);
        } else {
          Operand bt = cc.to_reg(b);
          cc.emit(Op::XOR, t, a.reg, bt.reg, 0);
        }
      } else {
        cc.emit(Op::XOR, t, a.reg, b.reg, 0);
      }
      if (op == "==") cc.emit(Op::SLTIU, rd, t, 0, 1);    // (a^b)==0
      else cc.emit(Op::SLTU, rd, 0, t, 0);                // (a^b)!=0
      return;
    }
    // relational: canonicalize to `slt(lhs, rhs)`.
    // a<b  → slt(a,b);  a>=b → slt(a,b)^1;  a>b → slt(b,a);  a<=b → slt(b,a)^1
    Operand l = a, r = b;
    bool negate = false;
    if (op == "<" || op == ">=") {
      // slt(a,b)
    } else {
      std::swap(l, r);                                  // slt(b,a)
    }
    if (op == ">=" || op == "<=") negate = true;
    // emit slt(rd, l, r)
    if (l.kind == Operand::Reg && r.kind == Operand::Const) {
      if (r.val >= -2048 && r.val <= 2047) {
        cc.emit(Op::SLTI, rd, l.reg, 0, r.val);               // l < imm
      } else {
        Operand rt = cc.to_reg(r);
        cc.emit(Op::SLT, rd, l.reg, rt.reg, 0);
      }
    } else if (l.kind == Operand::Const) {
      // SLTI can't put the immediate on the left, so materialize and SLT.
      Operand lt = cc.to_reg(l);
      Operand rt = cc.to_reg(r);
      cc.emit(Op::SLT, rd, lt.reg, rt.reg, 0);
    } else {
      cc.emit(Op::SLT, rd, l.reg, r.reg, 0);
    }
    if (negate) cc.emit(Op::XORI, rd, rd, 0, 1);
  }

  Operand apply_binop(const std::string& op, Operand a, Operand b, std::string& e) {
    Operand res;
    res.kind = Operand::Const;
    uint8_t rd = cc.alloc_temp();
    if (rd == 0) { e = "ran out of scratch registers"; return res; }

    // constant folding when both sides are constant
    auto fold = [&](int64_t v) {
      cc.materialize_const(rd, static_cast<int32_t>(v & 0xFFFFFFFF));
      return Operand::mk_reg(rd);
    };
    if (a.kind == Operand::Const && b.kind == Operand::Const) {
      int64_t x = a.val, y = b.val;
      if (op == "+") return fold(x + y);
      if (op == "-") return fold(x - y);
      if (op == "&") return fold(x & y);
      if (op == "|") return fold(x | y);
      if (op == "^") return fold(x ^ y);
      if (op == "<<") return fold((x << (y & 63)) & 0xFFFFFFFF);
      if (op == ">>") return fold(x >> (y & 63));
    }

    if (op == "+" || op == "&" || op == "|" || op == "^") {
      Op ir = op == "+" ? Op::ADDI : op == "&" ? Op::ANDI : op == "|" ? Op::ORI : Op::XORI;
      Op rr = op == "+" ? Op::ADD : op == "&" ? Op::AND : op == "|" ? Op::OR : Op::XOR;
      if (a.kind == Operand::Const && b.kind == Operand::Reg) std::swap(a, b);
      if (b.kind == Operand::Const) {
        if (b.val >= -2048 && b.val <= 2047) {
          Operand at = cc.to_reg(a);
          cc.emit(ir, rd, at.reg, 0, b.val);
        } else {
          Operand at = cc.to_reg(a);
          Operand bt = cc.to_reg(b);
          cc.emit(rr, rd, at.reg, bt.reg, 0);
        }
      } else {
        Operand at = cc.to_reg(a);
        Operand bt = cc.to_reg(b);
        cc.emit(rr, rd, at.reg, bt.reg, 0);
      }
      return Operand::mk_reg(rd);
    }

    if (op == "-") {
      if (b.kind == Operand::Const && b.val >= -2048 && b.val <= 2047) {
        Operand at = cc.to_reg(a);                    // a - c  →  addi rd,a,-c
        cc.emit(Op::ADDI, rd, at.reg, 0, -b.val);
      } else {
        Operand at = cc.to_reg(a);
        Operand bt = cc.to_reg(b);
        cc.emit(Op::SUB, rd, at.reg, bt.reg, 0);
      }
      return Operand::mk_reg(rd);
    }

    if (op == "<<" || op == ">>") {
      bool sr = (op == ">>");
      if (a.kind == Operand::Const) a = cc.to_reg(a);
      if (b.kind == Operand::Const) {
        if (b.val < 0 || b.val > 31) { e = "shift amount out of range [0,31]"; return res; }
        Operand at = cc.to_reg(a);
        cc.emit(sr ? Op::SRAI : Op::SLLI, rd, at.reg, 0, static_cast<int32_t>(b.val));
        return Operand::mk_reg(rd);
      }
      Operand at = cc.to_reg(a);
      Operand bt = cc.to_reg(b);
      cc.emit(sr ? Op::SRA : Op::SLL, rd, at.reg, bt.reg, 0);
      return Operand::mk_reg(rd);
    }

    e = "unsupported operator '" + op + "' (no M extension: */% unsupported)";
    return res;
  }

  Operand parse_binary(int min_prec, std::string& e) {
    Operand left = parse_unary(e);
    if (!e.empty()) return left;
    while (true) {
      if (peek().kind != TokKind::Op) break;
      const std::string& op = peek().text;
      int prec = op_prec(op);
      if (prec < 0 || prec < min_prec) break;
      next();
      Operand right = parse_binary(prec + 1, e);
      if (!e.empty()) return left;
      if (op == "<" || op == "<=" || op == ">" || op == ">=" || op == "==" || op == "!=") {
        uint8_t rd = cc.alloc_temp();
        if (rd == 0) { e = "ran out of scratch registers"; return left; }
        // equal to apply_binop comparisons
        if (left.kind == Operand::Const && right.kind == Operand::Const) {
          int64_t x = left.val, y = right.val;
          bool r = false;
          if (op == "<") r = x < y;
          else if (op == "<=") r = x <= y;
          else if (op == ">") r = x > y;
          else if (op == ">=") r = x >= y;
          else if (op == "==") r = x == y;
          else r = x != y;
          cc.materialize_const(rd, r ? 1 : 0);
        } else {
          emit_cmp(op, left, right, rd);
        }
        left = Operand::mk_reg(rd);
        continue;
      }
      left = apply_binop(op, left, right, e);
      if (!e.empty()) return left;
    }
    return left;
  }

  Operand parse_expr(int min_prec, std::string& e) { return parse_binary(min_prec, e); }
};

} // namespace

bool parse_expression_program(const std::string& text, ProgramSpec& out, std::string& err) {
  out = ProgramSpec{};
  std::vector<int> written;
  std::vector<Token> toks = tokenize(text);
  Parser p(toks, out.code, out.live_in, written, &err);

  if (toks.empty() || toks[0].kind == TokKind::End) { err = "empty expression"; return false; }

  // program := stmt ( ';' stmt )*
  while (true) {
    if (p.peek().kind == TokKind::End) break;
    // statement:  dst '=' expr  (';')
    const Token& tt = p.peek();
    if (tt.kind != TokKind::Reg) { err = "expected assignment '<reg> =', got '" + tt.text + "'"; return false; }
    int dst = tt.reg;
    if (dst <= 0) { err = "cannot assign to x0"; return false; }
    p.next();
    if (p.peek().kind != TokKind::Op || p.peek().text != "=") { err = "expected '=' after register name"; return false; }
    p.next();
    Operand rhs = p.parse_expr(0, err);
    if (!err.empty()) return false;
    // write result into dst (mv if computed into a temp)
    Operand v = p.cc.to_reg(rhs, static_cast<uint8_t>(dst));
    if (v.kind != Operand::Reg || v.reg != (uint8_t)dst) { err = "internal: live-out write failed"; return false; }
    p.cc.mark_written(dst);
    out.live_out.clear();
    out.live_out.push_back(dst);
    if (p.peek().kind == TokKind::Semi) { p.next(); continue; }
    if (p.peek().kind == TokKind::End) break;
    err = "expected ';' or end of input, got '" + p.peek().text + "'";
    return false;
  }

  if (p.peek().kind != TokKind::End) { err = "unexpected trailing input"; return false; }
  if (out.code.empty()) { err = "no instructions generated"; return false; }
  return true;
}

} // namespace sopt
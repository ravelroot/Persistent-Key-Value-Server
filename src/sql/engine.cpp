#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include "sql/sql.hpp"
#include "util/coding.hpp"

namespace arca::sql {

namespace {

constexpr char kCatalogPrefix = '\x02';
constexpr char kRowPrefix = '\x03';
constexpr char kIndexPrefix = '\x04';
constexpr char kMetaPrefix = '\x05';

std::string Prefix32(char prefix, uint32_t id) {
  std::string k(1, prefix);
  PutBigEndian32(&k, id);
  return k;
}

void EncodeOrdered(std::string* out, const Value& v) {
  switch (v.type) {
    case Type::kNull:
      break;
    case Type::kInt:
      PutBigEndian64(out, static_cast<uint64_t>(v.i) ^ (1ull << 63));
      break;
    case Type::kReal: {
      uint64_t bits;
      std::memcpy(&bits, &v.d, 8);
      bits = (bits & (1ull << 63)) ? ~bits : bits | (1ull << 63);
      PutBigEndian64(out, bits);
      break;
    }
    case Type::kBool:
      out->push_back(v.b ? '\x01' : '\x00');
      break;
    case Type::kText:
      for (char c : v.s) {
        if (c == '\0') {
          out->push_back('\0');
          out->push_back('\xff');
        } else {
          out->push_back(c);
        }
      }
      out->push_back('\0');
      out->push_back('\x01');
      break;
  }
}

std::string IndexPrefix(uint32_t index_id, const Value& v) {
  std::string k = Prefix32(kIndexPrefix, index_id);
  k.push_back(v.is_null() ? '\x00' : '\x01');
  EncodeOrdered(&k, v);
  return k;
}

std::string IndexKey(uint32_t index_id, const Value& v, const Value& pk) {
  std::string k = IndexPrefix(index_id, v);
  EncodeOrdered(&k, pk);
  return k;
}

void EncodeValue(std::string* out, const Value& v) {
  switch (v.type) {
    case Type::kNull:
      out->push_back(0);
      break;
    case Type::kInt:
      out->push_back(1);
      PutVarint64(out, (static_cast<uint64_t>(v.i) << 1) ^ static_cast<uint64_t>(v.i >> 63));
      break;
    case Type::kReal: {
      out->push_back(2);
      uint64_t bits;
      std::memcpy(&bits, &v.d, 8);
      PutFixed64(out, bits);
      break;
    }
    case Type::kText:
      out->push_back(3);
      PutLengthPrefixed(out, v.s);
      break;
    case Type::kBool:
      out->push_back(4);
      out->push_back(v.b ? 1 : 0);
      break;
  }
}

bool DecodeValue(std::string_view* in, Value* v) {
  if (in->empty()) return false;
  char tag = (*in)[0];
  in->remove_prefix(1);
  switch (tag) {
    case 0:
      *v = Value::Null();
      return true;
    case 1: {
      uint64_t z;
      if (!GetVarint64(in, &z)) return false;
      *v = Value::Int(static_cast<int64_t>((z >> 1) ^ (~(z & 1) + 1)));
      return true;
    }
    case 2: {
      uint64_t bits;
      if (!GetFixed64(in, &bits)) return false;
      double d;
      std::memcpy(&d, &bits, 8);
      *v = Value::Real(d);
      return true;
    }
    case 3: {
      std::string_view s;
      if (!GetLengthPrefixed(in, &s)) return false;
      *v = Value::Text(std::string(s));
      return true;
    }
    case 4:
      if (in->empty()) return false;
      *v = Value::Bool((*in)[0] != 0);
      in->remove_prefix(1);
      return true;
    default:
      return false;
  }
}

std::string EncodeTableDef(const TableDef& def) {
  std::string out;
  PutVarint32(&out, def.id);
  PutVarint32(&out, static_cast<uint32_t>(def.pk));
  PutVarint32(&out, static_cast<uint32_t>(def.columns.size()));
  for (const auto& c : def.columns) {
    PutLengthPrefixed(&out, c.name);
    out.push_back(static_cast<char>(c.type));
    uint8_t flags = (c.primary_key ? 1 : 0) | (c.not_null ? 2 : 0) | (c.auto_increment ? 4 : 0) | (c.has_default ? 8 : 0);
    out.push_back(static_cast<char>(flags));
    EncodeValue(&out, c.default_value);
  }
  PutVarint32(&out, static_cast<uint32_t>(def.indexes.size()));
  for (const auto& ix : def.indexes) {
    PutLengthPrefixed(&out, ix.name);
    PutVarint32(&out, ix.id);
    PutVarint32(&out, static_cast<uint32_t>(ix.column));
  }
  return out;
}

bool DecodeTableDef(std::string_view name, std::string_view in, TableDef* def) {
  def->name = std::string(name);
  uint32_t pk = 0;
  uint32_t ncols = 0;
  if (!GetVarint32(&in, &def->id) || !GetVarint32(&in, &pk) || !GetVarint32(&in, &ncols)) return false;
  def->pk = static_cast<int>(pk);
  for (uint32_t i = 0; i < ncols; ++i) {
    ColumnDef c;
    std::string_view cname;
    if (!GetLengthPrefixed(&in, &cname) || in.size() < 2) return false;
    c.name = std::string(cname);
    c.type = static_cast<Type>(in[0]);
    uint8_t flags = static_cast<uint8_t>(in[1]);
    in.remove_prefix(2);
    c.primary_key = flags & 1;
    c.not_null = flags & 2;
    c.auto_increment = flags & 4;
    c.has_default = flags & 8;
    if (!DecodeValue(&in, &c.default_value)) return false;
    def->columns.push_back(std::move(c));
  }
  uint32_t nidx = 0;
  if (!GetVarint32(&in, &nidx)) return false;
  for (uint32_t i = 0; i < nidx; ++i) {
    IndexDef ix;
    std::string_view iname;
    uint32_t col = 0;
    if (!GetLengthPrefixed(&in, &iname) || !GetVarint32(&in, &ix.id) || !GetVarint32(&in, &col)) return false;
    ix.name = std::string(iname);
    ix.column = static_cast<int>(col);
    def->indexes.push_back(std::move(ix));
  }
  return true;
}

std::string FormatReal(double d) {
  if (std::isnan(d) || std::isinf(d)) return "null";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.15g", d);
  std::string s(buf);
  if (s.find_first_of(".eE") == std::string::npos) s += ".0";
  return s;
}

std::string JsonEscape(std::string_view s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out + "\"";
}

bool LikeMatch(std::string_view text, std::string_view pattern) {
  size_t t = 0;
  size_t p = 0;
  size_t star_p = std::string_view::npos;
  size_t star_t = 0;
  auto eq = [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  };
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '_' || eq(pattern[p], text[t]))) {
      ++t;
      ++p;
    } else if (p < pattern.size() && pattern[p] == '%') {
      star_p = p++;
      star_t = t;
    } else if (star_p != std::string_view::npos) {
      p = star_p + 1;
      t = ++star_t;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '%') ++p;
  return p == pattern.size();
}

bool IsAggregate(const std::string& name) {
  return name == "count" || name == "sum" || name == "avg" || name == "min" || name == "max";
}

bool HasAggregate(const Expr* e) {
  if (e == nullptr) return false;
  if (e->kind == Expr::Kind::kFunction && IsAggregate(e->name)) return true;
  for (const auto& a : e->args) {
    if (HasAggregate(a.get())) return true;
  }
  return false;
}

Status Bind(Expr* e, const TableDef& def) {
  if (e == nullptr) return Status::OK();
  if (e->kind == Expr::Kind::kColumn) {
    e->column_index = def.FindColumn(e->name);
    if (e->column_index < 0) return Status::InvalidArgument("coluna desconhecida: " + e->name);
  }
  for (auto& a : e->args) {
    Status s = Bind(a.get(), def);
    if (!s.ok()) return s;
  }
  return Status::OK();
}

Value Arith(const std::string& op, const Value& a, const Value& b) {
  if (op == "||") {
    if (a.is_null() || b.is_null()) return Value::Null();
    return Value::Text(a.ToString() + b.ToString());
  }
  if (a.is_null() || b.is_null()) return Value::Null();
  bool a_num = a.type == Type::kInt || a.type == Type::kReal;
  bool b_num = b.type == Type::kInt || b.type == Type::kReal;
  if (!a_num || !b_num) return Value::Null();
  if (a.type == Type::kInt && b.type == Type::kInt) {
    if (op == "+") return Value::Int(a.i + b.i);
    if (op == "-") return Value::Int(a.i - b.i);
    if (op == "*") return Value::Int(a.i * b.i);
    if (b.i == 0) return Value::Null();
    if (op == "/") return Value::Int(a.i / b.i);
    if (op == "%") return Value::Int(a.i % b.i);
  }
  double x = a.type == Type::kInt ? static_cast<double>(a.i) : a.d;
  double y = b.type == Type::kInt ? static_cast<double>(b.i) : b.d;
  if (op == "+") return Value::Real(x + y);
  if (op == "-") return Value::Real(x - y);
  if (op == "*") return Value::Real(x * y);
  if (y == 0) return Value::Null();
  if (op == "/") return Value::Real(x / y);
  if (op == "%") return Value::Real(std::fmod(x, y));
  return Value::Null();
}

Value Eval(const Expr* e, const std::vector<Value>& row) {
  switch (e->kind) {
    case Expr::Kind::kLiteral:
      return e->literal;
    case Expr::Kind::kColumn:
      return e->column_index >= 0 && static_cast<size_t>(e->column_index) < row.size() ? row[e->column_index]
                                                                                        : Value::Null();
    case Expr::Kind::kStar:
      return Value::Null();
    case Expr::Kind::kUnary: {
      Value v = Eval(e->args[0].get(), row);
      if (e->op == "not") return v.is_null() ? Value::Null() : Value::Bool(!Truthy(v));
      if (v.type == Type::kInt) return Value::Int(-v.i);
      if (v.type == Type::kReal) return Value::Real(-v.d);
      return Value::Null();
    }
    case Expr::Kind::kBinary: {
      const std::string& op = e->op;
      if (op == "and") {
        Value a = Eval(e->args[0].get(), row);
        if (!a.is_null() && !Truthy(a)) return Value::Bool(false);
        Value b = Eval(e->args[1].get(), row);
        if (!b.is_null() && !Truthy(b)) return Value::Bool(false);
        if (a.is_null() || b.is_null()) return Value::Null();
        return Value::Bool(true);
      }
      if (op == "or") {
        Value a = Eval(e->args[0].get(), row);
        if (!a.is_null() && Truthy(a)) return Value::Bool(true);
        Value b = Eval(e->args[1].get(), row);
        if (!b.is_null() && Truthy(b)) return Value::Bool(true);
        if (a.is_null() || b.is_null()) return Value::Null();
        return Value::Bool(false);
      }
      Value a = Eval(e->args[0].get(), row);
      Value b = Eval(e->args[1].get(), row);
      if (op == "=" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
        if (a.is_null() || b.is_null()) return Value::Null();
        int c = Compare(a, b);
        if (op == "=") return Value::Bool(c == 0);
        if (op == "!=") return Value::Bool(c != 0);
        if (op == "<") return Value::Bool(c < 0);
        if (op == "<=") return Value::Bool(c <= 0);
        if (op == ">") return Value::Bool(c > 0);
        return Value::Bool(c >= 0);
      }
      return Arith(op, a, b);
    }
    case Expr::Kind::kIsNull: {
      bool is_null = Eval(e->args[0].get(), row).is_null();
      return Value::Bool(e->negated ? !is_null : is_null);
    }
    case Expr::Kind::kIn: {
      Value v = Eval(e->args[0].get(), row);
      if (v.is_null()) return Value::Null();
      bool found = false;
      for (size_t i = 1; i < e->args.size() && !found; ++i) {
        Value item = Eval(e->args[i].get(), row);
        if (!item.is_null() && Compare(v, item) == 0) found = true;
      }
      return Value::Bool(e->negated ? !found : found);
    }
    case Expr::Kind::kBetween: {
      Value v = Eval(e->args[0].get(), row);
      Value lo = Eval(e->args[1].get(), row);
      Value hi = Eval(e->args[2].get(), row);
      if (v.is_null() || lo.is_null() || hi.is_null()) return Value::Null();
      bool in = Compare(v, lo) >= 0 && Compare(v, hi) <= 0;
      return Value::Bool(e->negated ? !in : in);
    }
    case Expr::Kind::kLike: {
      Value v = Eval(e->args[0].get(), row);
      Value p = Eval(e->args[1].get(), row);
      if (v.is_null() || p.is_null()) return Value::Null();
      bool m = LikeMatch(v.ToString(), p.ToString());
      return Value::Bool(e->negated ? !m : m);
    }
    case Expr::Kind::kFunction: {
      const std::string& f = e->name;
      std::vector<Value> args;
      for (const auto& a : e->args) args.push_back(Eval(a.get(), row));
      if (f == "lower" && args.size() == 1 && !args[0].is_null()) return Value::Text(Lower(args[0].ToString()));
      if (f == "upper" && args.size() == 1 && !args[0].is_null()) {
        std::string s = args[0].ToString();
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return Value::Text(s);
      }
      if (f == "length" && args.size() == 1 && !args[0].is_null()) {
        return Value::Int(static_cast<int64_t>(args[0].ToString().size()));
      }
      if (f == "abs" && args.size() == 1) {
        if (args[0].type == Type::kInt) return Value::Int(args[0].i < 0 ? -args[0].i : args[0].i);
        if (args[0].type == Type::kReal) return Value::Real(std::fabs(args[0].d));
      }
      if (f == "round" && !args.empty() && (args[0].type == Type::kReal || args[0].type == Type::kInt)) {
        double x = args[0].type == Type::kInt ? static_cast<double>(args[0].i) : args[0].d;
        int64_t digits = args.size() > 1 && args[1].type == Type::kInt ? args[1].i : 0;
        double m = std::pow(10.0, static_cast<double>(digits));
        return Value::Real(std::round(x * m) / m);
      }
      if (f == "coalesce" || f == "ifnull") {
        for (const auto& a : args) {
          if (!a.is_null()) return a;
        }
      }
      if ((f == "trim" || f == "ltrim" || f == "rtrim") && !args.empty() && !args[0].is_null()) {
        std::string v = args[0].ToString();
        std::string chars = args.size() > 1 && !args[1].is_null() ? args[1].ToString() : std::string(" \t\r\n");
        size_t b = 0;
        size_t en = v.size();
        if (f != "rtrim") {
          while (b < en && chars.find(v[b]) != std::string::npos) ++b;
        }
        if (f != "ltrim") {
          while (en > b && chars.find(v[en - 1]) != std::string::npos) --en;
        }
        return Value::Text(v.substr(b, en - b));
      }
      if ((f == "substr" || f == "substring") && args.size() >= 2 && !args[0].is_null() && args[1].type == Type::kInt) {
        std::string v = args[0].ToString();
        int64_t len = static_cast<int64_t>(v.size());
        int64_t start = args[1].i;
        if (start < 0) start = len + start + 1;
        if (start < 1) start = 1;
        int64_t count = len;
        if (args.size() > 2) {
          if (args[2].type != Type::kInt) return Value::Null();
          count = std::max<int64_t>(0, args[2].i);
        }
        if (start > len) return Value::Text("");
        return Value::Text(v.substr(static_cast<size_t>(start - 1), static_cast<size_t>(std::min(count, len - start + 1))));
      }
      if (f == "replace" && args.size() == 3 && !args[0].is_null() && !args[1].is_null() && !args[2].is_null()) {
        std::string v = args[0].ToString();
        std::string from = args[1].ToString();
        std::string to = args[2].ToString();
        if (from.empty()) return Value::Text(v);
        std::string res;
        size_t pos = 0;
        for (size_t hit; (hit = v.find(from, pos)) != std::string::npos; pos = hit + from.size()) {
          res.append(v, pos, hit - pos);
          res += to;
        }
        res.append(v, pos, std::string::npos);
        return Value::Text(res);
      }
      if (f == "concat") {
        std::string res;
        for (const auto& a : args) {
          if (!a.is_null()) res += a.ToString();
        }
        return Value::Text(res);
      }
      if (f == "instr" && args.size() == 2 && !args[0].is_null() && !args[1].is_null()) {
        size_t pos = args[0].ToString().find(args[1].ToString());
        return Value::Int(pos == std::string::npos ? 0 : static_cast<int64_t>(pos) + 1);
      }
      if (f == "nullif" && args.size() == 2) {
        if (!args[0].is_null() && !args[1].is_null() && Compare(args[0], args[1]) == 0) return Value::Null();
        return args[0];
      }
      if ((f == "floor" || f == "ceil") && args.size() == 1) {
        if (args[0].type == Type::kInt) return args[0];
        if (args[0].type == Type::kReal) {
          double r = f == "floor" ? std::floor(args[0].d) : std::ceil(args[0].d);
          return Value::Int(static_cast<int64_t>(r));
        }
      }
      if (f == "typeof" && args.size() == 1) {
        switch (args[0].type) {
          case Type::kInt:
            return Value::Text("int");
          case Type::kReal:
            return Value::Text("real");
          case Type::kText:
            return Value::Text("text");
          case Type::kBool:
            return Value::Text("bool");
          default:
            return Value::Text("null");
        }
      }
      return Value::Null();
    }
  }
  return Value::Null();
}

Status ValidateFunctions(const Expr* e) {
  if (e == nullptr) return Status::OK();
  if (e->kind == Expr::Kind::kFunction) {
    static const std::set<std::string> known = {"count",  "sum",    "avg",    "min",     "max",       "lower",
                                                "upper",  "length", "abs",    "round",   "coalesce",  "ifnull",
                                                "trim",   "ltrim",  "rtrim",  "substr",  "substring", "replace",
                                                "concat", "instr",  "nullif", "floor",   "ceil",      "typeof"};
    if (known.count(e->name) == 0) return Status::InvalidArgument("função desconhecida: " + e->name);
  }
  for (const auto& a : e->args) {
    Status s = ValidateFunctions(a.get());
    if (!s.ok()) return s;
  }
  return Status::OK();
}

Status Coerce(const ColumnDef& col, Value* v) {
  if (v->is_null()) return Status::OK();
  switch (col.type) {
    case Type::kInt:
      if (v->type == Type::kInt) return Status::OK();
      if (v->type == Type::kReal && std::floor(v->d) == v->d) {
        *v = Value::Int(static_cast<int64_t>(v->d));
        return Status::OK();
      }
      if (v->type == Type::kBool) {
        *v = Value::Int(v->b ? 1 : 0);
        return Status::OK();
      }
      break;
    case Type::kReal:
      if (v->type == Type::kReal) return Status::OK();
      if (v->type == Type::kInt) {
        *v = Value::Real(static_cast<double>(v->i));
        return Status::OK();
      }
      break;
    case Type::kText:
      if (v->type != Type::kText) *v = Value::Text(v->ToString());
      return Status::OK();
    case Type::kBool:
      if (v->type == Type::kBool) return Status::OK();
      if (v->type == Type::kInt) {
        *v = Value::Bool(v->i != 0);
        return Status::OK();
      }
      break;
    case Type::kNull:
      return Status::OK();
  }
  return Status::InvalidArgument("valor " + v->ToString() + " não serve para a coluna " + col.name + " (" +
                                 TypeName(col.type) + ")");
}

const Expr* FindEquality(const Expr* e, int column) {
  if (e == nullptr) return nullptr;
  if (e->kind == Expr::Kind::kBinary && e->op == "and") {
    const Expr* left = FindEquality(e->args[0].get(), column);
    return left != nullptr ? left : FindEquality(e->args[1].get(), column);
  }
  if (e->kind == Expr::Kind::kBinary && e->op == "=") {
    const Expr* a = e->args[0].get();
    const Expr* b = e->args[1].get();
    if (a->kind == Expr::Kind::kColumn && a->column_index == column && b->kind == Expr::Kind::kLiteral) return b;
    if (b->kind == Expr::Kind::kColumn && b->column_index == column && a->kind == Expr::Kind::kLiteral) return a;
  }
  return nullptr;
}

std::string ExprName(const Expr* e) {
  switch (e->kind) {
    case Expr::Kind::kColumn:
      return e->name;
    case Expr::Kind::kStar:
      return "*";
    case Expr::Kind::kLiteral:
      return e->literal.ToString();
    case Expr::Kind::kFunction: {
      std::string s = e->name + "(";
      for (size_t i = 0; i < e->args.size(); ++i) {
        if (i > 0) s += ", ";
        s += ExprName(e->args[i].get());
      }
      return s + ")";
    }
    case Expr::Kind::kBinary:
      return ExprName(e->args[0].get()) + " " + e->op + " " + ExprName(e->args[1].get());
    default:
      return "expr";
  }
}

struct Accumulator {
  int64_t count = 0;
  bool has = false;
  bool all_int = true;
  int64_t isum = 0;
  double dsum = 0;
  Value best;
  std::set<std::string> distinct_seen;
};

void Accumulate(const Expr* fn, const std::vector<Value>& row, Accumulator* acc) {
  if (!fn->args.empty() && fn->args[0]->kind == Expr::Kind::kStar) {
    ++acc->count;
    return;
  }
  Value v = fn->args.empty() ? Value::Null() : Eval(fn->args[0].get(), row);
  if (v.is_null()) return;
  if (fn->distinct && !acc->distinct_seen.insert(v.ToString()).second) return;
  ++acc->count;
  if (v.type == Type::kInt) {
    acc->isum += v.i;
    acc->dsum += static_cast<double>(v.i);
  } else if (v.type == Type::kReal) {
    acc->all_int = false;
    acc->dsum += v.d;
  } else {
    acc->all_int = false;
  }
  if (!acc->has) {
    acc->best = v;
    acc->has = true;
  } else if ((fn->name == "min" && Compare(v, acc->best) < 0) || (fn->name == "max" && Compare(v, acc->best) > 0)) {
    acc->best = v;
  }
}

Value Finalize(const Expr* fn, const Accumulator& acc) {
  if (fn->name == "count") return Value::Int(acc.count);
  if (acc.count == 0) return Value::Null();
  if (fn->name == "sum") return acc.all_int ? Value::Int(acc.isum) : Value::Real(acc.dsum);
  if (fn->name == "avg") return Value::Real(acc.dsum / static_cast<double>(acc.count));
  return acc.best;
}

void CollectAggregates(const Expr* e, std::vector<const Expr*>* out) {
  if (e == nullptr) return;
  if (e->kind == Expr::Kind::kFunction && IsAggregate(e->name)) {
    out->push_back(e);
    return;
  }
  for (const auto& a : e->args) CollectAggregates(a.get(), out);
}

Value EvalWithAggregates(const Expr* e, const std::vector<Value>& first_row, const std::map<const Expr*, Value>& aggs) {
  if (e->kind == Expr::Kind::kFunction && IsAggregate(e->name)) {
    auto it = aggs.find(e);
    return it != aggs.end() ? it->second : Value::Null();
  }
  if (!HasAggregate(e)) return Eval(e, first_row);
  Expr copy;
  copy.kind = Expr::Kind::kLiteral;
  if (e->kind == Expr::Kind::kBinary) {
    Value a = EvalWithAggregates(e->args[0].get(), first_row, aggs);
    Value b = EvalWithAggregates(e->args[1].get(), first_row, aggs);
    auto la = std::make_unique<Expr>();
    la->literal = a;
    auto lb = std::make_unique<Expr>();
    lb->literal = b;
    Expr bin;
    bin.kind = Expr::Kind::kBinary;
    bin.op = e->op;
    bin.args.push_back(std::move(la));
    bin.args.push_back(std::move(lb));
    return Eval(&bin, first_row);
  }
  if (e->kind == Expr::Kind::kFunction) {
    Expr fn;
    fn.kind = Expr::Kind::kFunction;
    fn.name = e->name;
    for (const auto& a : e->args) {
      auto lit = std::make_unique<Expr>();
      lit->literal = EvalWithAggregates(a.get(), first_row, aggs);
      fn.args.push_back(std::move(lit));
    }
    return Eval(&fn, first_row);
  }
  return Eval(e, first_row);
}

}

const char* TypeName(Type t) {
  switch (t) {
    case Type::kNull:
      return "NULL";
    case Type::kInt:
      return "INT";
    case Type::kReal:
      return "REAL";
    case Type::kText:
      return "TEXT";
    case Type::kBool:
      return "BOOL";
  }
  return "?";
}

std::string Value::ToString() const {
  switch (type) {
    case Type::kNull:
      return "NULL";
    case Type::kInt:
      return std::to_string(i);
    case Type::kReal:
      return FormatReal(d);
    case Type::kText:
      return s;
    case Type::kBool:
      return b ? "true" : "false";
  }
  return {};
}

std::string Value::ToJson() const {
  switch (type) {
    case Type::kNull:
      return "null";
    case Type::kInt:
      return std::to_string(i);
    case Type::kReal:
      return FormatReal(d);
    case Type::kText:
      return JsonEscape(s);
    case Type::kBool:
      return b ? "true" : "false";
  }
  return "null";
}

int Compare(const Value& a, const Value& b) {
  if (a.is_null() || b.is_null()) return a.is_null() == b.is_null() ? 0 : (a.is_null() ? -1 : 1);
  bool a_num = a.type == Type::kInt || a.type == Type::kReal;
  bool b_num = b.type == Type::kInt || b.type == Type::kReal;
  if (a_num && b_num) {
    if (a.type == Type::kInt && b.type == Type::kInt) return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
    double x = a.type == Type::kInt ? static_cast<double>(a.i) : a.d;
    double y = b.type == Type::kInt ? static_cast<double>(b.i) : b.d;
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  if (a.type == Type::kBool && b.type == Type::kBool) return static_cast<int>(a.b) - static_cast<int>(b.b);
  int r = a.ToString().compare(b.ToString());
  return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

bool Truthy(const Value& v) {
  switch (v.type) {
    case Type::kNull:
      return false;
    case Type::kInt:
      return v.i != 0;
    case Type::kReal:
      return v.d != 0;
    case Type::kText:
      return !v.s.empty();
    case Type::kBool:
      return v.b;
  }
  return false;
}

int TableDef::FindColumn(std::string_view n) const {
  std::string lower = Lower(n);
  for (size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].name == lower || columns[i].name == n) return static_cast<int>(i);
  }
  return -1;
}

std::string EncodeRowKey(uint32_t table_id, const Value& pk) {
  std::string k = Prefix32(kRowPrefix, table_id);
  EncodeOrdered(&k, pk);
  return k;
}

std::string EncodeRow(const std::vector<Value>& row) {
  std::string out;
  PutVarint32(&out, static_cast<uint32_t>(row.size()));
  for (const auto& v : row) EncodeValue(&out, v);
  return out;
}

bool DecodeRow(std::string_view data, size_t columns, std::vector<Value>* row) {
  uint32_t n = 0;
  if (!GetVarint32(&data, &n)) return false;
  row->assign(columns, Value::Null());
  for (uint32_t i = 0; i < n; ++i) {
    Value v;
    if (!DecodeValue(&data, &v)) return false;
    if (i < columns) (*row)[i] = std::move(v);
  }
  return true;
}

Engine::Engine(DB* db) : db_(db) {}

Status Engine::Load() {
  std::unique_lock<std::shared_mutex> lock(catalog_mu_);
  tables_.clear();
  auto it = db_->NewIterator({});
  std::string prefix(1, kCatalogPrefix);
  for (it->Seek(prefix); it->Valid() && !it->key().empty() && it->key()[0] == kCatalogPrefix; it->Next()) {
    auto state = std::make_shared<TableState>();
    if (!DecodeTableDef(it->key().substr(1), it->value(), &state->def)) {
      return Status::Corruption("definição de tabela inválida: " + std::string(it->key().substr(1)));
    }
    tables_[state->def.name] = state;
  }
  return it->status();
}

std::shared_ptr<Engine::TableState> Engine::FindTable(const std::string& name) {
  std::shared_lock<std::shared_mutex> lock(catalog_mu_);
  auto it = tables_.find(Lower(name));
  return it == tables_.end() ? nullptr : it->second;
}

Status Engine::SaveTable(const TableDef& def, WriteBatch* batch) {
  batch->Put(std::string(1, kCatalogPrefix) + def.name, EncodeTableDef(def));
  return Status::OK();
}

int64_t Engine::ReadCounter(const std::string& key) {
  std::string v;
  if (!db_->Get({}, key, &v).ok() || v.size() != 8) return 0;
  return static_cast<int64_t>(DecodeFixed64(v.data()));
}

void Engine::WriteCounter(WriteBatch* batch, const std::string& key, int64_t value) {
  std::string v;
  PutFixed64(&v, static_cast<uint64_t>(value));
  batch->Put(key, v);
}

uint32_t Engine::NextId() {
  std::lock_guard<std::mutex> lock(id_mu_);
  std::string key = std::string(1, kMetaPrefix) + "next_id";
  int64_t next = ReadCounter(key) + 1;
  WriteBatch batch;
  WriteCounter(&batch, key, next);
  db_->Write({}, &batch);
  return static_cast<uint32_t>(next);
}

std::vector<TableSummary> Engine::Tables() {
  std::vector<std::shared_ptr<TableState>> states;
  {
    std::shared_lock<std::shared_mutex> lock(catalog_mu_);
    for (const auto& [name, st] : tables_) states.push_back(st);
  }
  std::vector<TableSummary> out;
  for (const auto& st : states) {
    std::shared_lock<std::shared_mutex> lock(st->mu);
    TableSummary s;
    s.def = st->def;
    s.rows = ReadCounter(Prefix32(kMetaPrefix, st->def.id) + "rows");
    out.push_back(std::move(s));
  }
  return out;
}

size_t Engine::TableCount() {
  std::shared_lock<std::shared_mutex> lock(catalog_mu_);
  return tables_.size();
}

std::map<uint32_t, std::string> Engine::TableNames() {
  std::shared_lock<std::shared_mutex> lock(catalog_mu_);
  std::map<uint32_t, std::string> names;
  for (const auto& [name, st] : tables_) names[st->def.id] = name;
  return names;
}

std::string Engine::DescribeKey(std::string_view key, const std::map<uint32_t, std::string>& names) {
  auto printable = [](std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
      if (c >= 32 && c < 127) {
        out.push_back(static_cast<char>(c));
      } else if (c >= 0x80) {
        out.push_back(static_cast<char>(c));
      } else {
        out.push_back('.');
      }
    }
    return out;
  };
  if (key.empty()) return "";
  char p = key[0];
  std::string_view rest = key.substr(1);
  switch (p) {
    case '\x01':
      return "kv:" + printable(rest);
    case '\x02':
      return "catalogo:" + printable(rest);
    case '\x03':
    case '\x04': {
      if (rest.size() < 4) return printable(key);
      uint32_t id = DecodeBigEndian32(rest.data());
      std::string_view enc = rest.substr(4);
      std::string label;
      if (p == '\x03') {
        auto it = names.find(id);
        label = "row:" + (it != names.end() ? it->second : std::to_string(id)) + ":";
        if (enc.size() == 8) {
          label += std::to_string(static_cast<int64_t>(DecodeBigEndian64(enc.data()) ^ (1ull << 63)));
        } else {
          size_t end = enc.find(std::string_view("\0\x01", 2));
          label += printable(enc.substr(0, end));
        }
      } else {
        label = "idx:" + std::to_string(id);
      }
      return label;
    }
    case '\x05':
      return "meta:" + printable(rest);
    case '\x06':
      return rest.size() > 8 ? "ttl:" + printable(rest.substr(8)) : "ttl";
    default:
      return printable(key);
  }
}

Status Engine::Execute(std::string_view text, Result* r, const Snapshot* snapshot) {
  auto start = std::chrono::steady_clock::now();
  Statement st;
  Status s = Parse(text, &st);
  if (!s.ok()) return s;
  switch (st.kind) {
    case Statement::Kind::kSelect:
      s = ExecSelect(st, r, snapshot);
      break;
    case Statement::Kind::kInsert:
      s = ExecInsert(st, r);
      break;
    case Statement::Kind::kUpdate:
      s = ExecUpdate(st, r);
      break;
    case Statement::Kind::kDelete:
      s = ExecDelete(st, r);
      break;
    case Statement::Kind::kCreateTable:
      s = ExecCreateTable(st, r);
      break;
    case Statement::Kind::kDropTable:
      s = ExecDropTable(st, r);
      break;
    case Statement::Kind::kAlterAddColumn:
      s = ExecAlterAddColumn(st, r);
      break;
    case Statement::Kind::kCreateIndex:
      s = ExecCreateIndex(st, r);
      break;
    case Statement::Kind::kDropIndex:
      s = ExecDropIndex(st, r);
      break;
    case Statement::Kind::kShowTables:
      s = ExecShowTables(r);
      break;
    case Statement::Kind::kDescribe:
      s = ExecDescribe(st, r);
      break;
  }
  if (st.explain && s.ok() && st.kind == Statement::Kind::kSelect) {
    std::string plan = r->plan;
    *r = Result();
    r->has_rows = true;
    r->columns = {"plano"};
    r->types = {"TEXT"};
    r->rows.push_back({Value::Text(plan)});
    r->plan = plan;
  }
  r->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return s;
}

Status Engine::ScanRows(const TableDef& def, const Expr* where, const Snapshot* snap,
                        std::vector<std::vector<Value>>* rows, std::string* plan) {
  ReadOptions ro;
  ro.snapshot = snap;
  auto accept = [&](const std::vector<Value>& row) { return where == nullptr || Truthy(Eval(where, row)); };
  if (const Expr* lit = FindEquality(where, def.pk)) {
    *plan = "busca pela chave primária em " + def.name;
    Value pk = lit->literal;
    if (!Coerce(def.columns[def.pk], &pk).ok() || pk.is_null()) return Status::OK();
    std::string data;
    Status s = db_->Get(ro, EncodeRowKey(def.id, pk), &data);
    if (s.IsNotFound()) return Status::OK();
    if (!s.ok()) return s;
    std::vector<Value> row;
    if (!DecodeRow(data, def.columns.size(), &row)) return Status::Corruption("linha inválida em " + def.name);
    if (accept(row)) rows->push_back(std::move(row));
    return Status::OK();
  }
  for (const auto& ix : def.indexes) {
    const Expr* lit = FindEquality(where, ix.column);
    if (lit == nullptr) continue;
    Value v = lit->literal;
    if (!Coerce(def.columns[ix.column], &v).ok()) continue;
    *plan = "busca pelo índice " + ix.name + " em " + def.name;
    std::string prefix = IndexPrefix(ix.id, v);
    auto it = db_->NewIterator(ro);
    std::vector<std::string> keys;
    for (it->Seek(prefix); it->Valid() && it->key().substr(0, prefix.size()) == prefix; it->Next()) {
      keys.push_back(Prefix32(kRowPrefix, def.id) + std::string(it->key().substr(prefix.size())));
    }
    for (const auto& k : keys) {
      std::string data;
      Status s = db_->Get(ro, k, &data);
      if (s.IsNotFound()) continue;
      if (!s.ok()) return s;
      std::vector<Value> row;
      if (!DecodeRow(data, def.columns.size(), &row)) return Status::Corruption("linha inválida em " + def.name);
      if (accept(row)) rows->push_back(std::move(row));
    }
    return it->status();
  }
  *plan = "varredura completa de " + def.name;
  std::string prefix = Prefix32(kRowPrefix, def.id);
  auto it = db_->NewIterator(ro);
  for (it->Seek(prefix); it->Valid() && it->key().substr(0, prefix.size()) == prefix; it->Next()) {
    std::vector<Value> row;
    if (!DecodeRow(it->value(), def.columns.size(), &row)) return Status::Corruption("linha inválida em " + def.name);
    if (accept(row)) rows->push_back(std::move(row));
  }
  return it->status();
}

Status Engine::ExecSelect(Statement& st, Result* r, const Snapshot* snap) {
  auto table = FindTable(st.table);
  if (!table) return Status::NotFound("tabela não existe: " + st.table);
  std::shared_lock<std::shared_mutex> lock(table->mu);
  const TableDef& def = table->def;
  Status s = Bind(st.where.get(), def);
  if (!s.ok()) return s;
  if (HasAggregate(st.where.get())) return Status::InvalidArgument("funções de agregação não são permitidas no WHERE");
  for (auto& item : st.select) {
    s = Bind(item.expr.get(), def);
    if (!s.ok()) return s;
    s = ValidateFunctions(item.expr.get());
    if (!s.ok()) return s;
  }
  for (auto& g : st.group_by) {
    s = Bind(g.get(), def);
    if (!s.ok()) return s;
  }

  std::vector<const Expr*> out_exprs;
  for (const auto& item : st.select) {
    if (item.expr->kind == Expr::Kind::kStar) {
      for (const auto& c : def.columns) {
        r->columns.push_back(c.name);
        r->types.push_back(TypeName(c.type));
      }
      out_exprs.push_back(item.expr.get());
    } else {
      r->columns.push_back(!item.alias.empty() ? item.alias : ExprName(item.expr.get()));
      if (item.expr->kind == Expr::Kind::kColumn) {
        r->types.push_back(TypeName(def.columns[item.expr->column_index].type));
      } else {
        r->types.push_back("");
      }
      out_exprs.push_back(item.expr.get());
    }
  }

  for (auto& o : st.order_by) {
    if (o.expr->kind == Expr::Kind::kColumn) {
      auto pos = std::find(r->columns.begin(), r->columns.end(), o.expr->name);
      if (pos != r->columns.end() && def.FindColumn(o.expr->name) < 0) {
        o.expr->column_index = -2 - static_cast<int>(pos - r->columns.begin());
        continue;
      }
    }
    s = Bind(o.expr.get(), def);
    if (!s.ok()) return s;
  }

  std::vector<std::vector<Value>> rows;
  s = ScanRows(def, st.where.get(), snap, &rows, &r->plan);
  if (!s.ok()) return s;

  bool aggregate = !st.group_by.empty();
  for (const auto& item : st.select) aggregate = aggregate || HasAggregate(item.expr.get());

  struct OutRow {
    std::vector<Value> source;
    std::vector<Value> values;
  };
  std::vector<OutRow> output;

  auto project = [&](const std::vector<Value>& source, const std::map<const Expr*, Value>* aggs) {
    OutRow o;
    o.source = source;
    for (const Expr* e : out_exprs) {
      if (e->kind == Expr::Kind::kStar) {
        o.values.insert(o.values.end(), source.begin(), source.end());
      } else if (aggs != nullptr) {
        o.values.push_back(EvalWithAggregates(e, source, *aggs));
      } else {
        o.values.push_back(Eval(e, source));
      }
    }
    return o;
  };

  if (aggregate) {
    std::vector<const Expr*> agg_exprs;
    for (const auto& item : st.select) CollectAggregates(item.expr.get(), &agg_exprs);
    for (const auto& o : st.order_by) CollectAggregates(o.expr.get(), &agg_exprs);
    struct Group {
      std::vector<Value> first;
      std::vector<Accumulator> accs;
    };
    std::map<std::string, Group> groups;
    std::vector<std::string> order;
    for (const auto& row : rows) {
      std::string key;
      for (const auto& g : st.group_by) {
        EncodeValue(&key, Eval(g.get(), row));
      }
      auto [it, inserted] = groups.try_emplace(key);
      if (inserted) {
        it->second.first = row;
        it->second.accs.resize(agg_exprs.size());
        order.push_back(key);
      }
      for (size_t i = 0; i < agg_exprs.size(); ++i) Accumulate(agg_exprs[i], row, &it->second.accs[i]);
    }
    if (groups.empty() && st.group_by.empty()) {
      Group g;
      g.first.assign(def.columns.size(), Value::Null());
      g.accs.resize(agg_exprs.size());
      groups[""] = std::move(g);
      order.push_back("");
    }
    for (const auto& key : order) {
      const Group& g = groups[key];
      std::map<const Expr*, Value> aggs;
      for (size_t i = 0; i < agg_exprs.size(); ++i) aggs[agg_exprs[i]] = Finalize(agg_exprs[i], g.accs[i]);
      OutRow o = project(g.first, &aggs);
      for (const auto& ord : st.order_by) {
        if (ord.expr->column_index <= -2) continue;
        o.source.push_back(EvalWithAggregates(ord.expr.get(), g.first, aggs));
      }
      output.push_back(std::move(o));
    }
  } else {
    for (const auto& row : rows) output.push_back(project(row, nullptr));
  }

  if (!st.order_by.empty()) {
    size_t base = def.columns.size();
    std::stable_sort(output.begin(), output.end(), [&](const OutRow& a, const OutRow& b) {
      size_t extra_index = 0;
      for (const auto& ord : st.order_by) {
        Value va;
        Value vb;
        if (ord.expr->column_index <= -2) {
          size_t idx = static_cast<size_t>(-2 - ord.expr->column_index);
          va = a.values[idx];
          vb = b.values[idx];
        } else if (aggregate) {
          va = a.source[base + extra_index];
          vb = b.source[base + extra_index];
          ++extra_index;
        } else {
          va = Eval(ord.expr.get(), a.source);
          vb = Eval(ord.expr.get(), b.source);
        }
        int c = Compare(va, vb);
        if (c != 0) return ord.desc ? c > 0 : c < 0;
      }
      return false;
    });
  }

  size_t begin = std::min(output.size(), static_cast<size_t>(st.offset));
  size_t end = st.limit < 0 ? output.size() : std::min(output.size(), begin + static_cast<size_t>(st.limit));
  r->has_rows = true;
  for (size_t i = begin; i < end; ++i) r->rows.push_back(std::move(output[i].values));
  for (size_t c = 0; c < r->types.size(); ++c) {
    if (!r->types[c].empty()) continue;
    for (const auto& row : r->rows) {
      if (!row[c].is_null()) {
        r->types[c] = TypeName(row[c].type);
        break;
      }
    }
    if (r->types[c].empty()) r->types[c] = "NULL";
  }
  return Status::OK();
}

Status Engine::ExecInsert(Statement& st, Result* r) {
  auto table = FindTable(st.table);
  if (!table) return Status::NotFound("tabela não existe: " + st.table);
  std::unique_lock<std::shared_mutex> lock(table->mu);
  const TableDef& def = table->def;
  std::vector<int> targets;
  if (st.insert_columns.empty()) {
    for (size_t i = 0; i < def.columns.size(); ++i) targets.push_back(static_cast<int>(i));
  } else {
    for (const auto& name : st.insert_columns) {
      int idx = def.FindColumn(name);
      if (idx < 0) return Status::InvalidArgument("coluna desconhecida: " + name);
      targets.push_back(idx);
    }
  }
  std::string ai_key = Prefix32(kMetaPrefix, def.id) + "ai";
  std::string rows_key = Prefix32(kMetaPrefix, def.id) + "rows";
  int64_t ai = ReadCounter(ai_key);
  int64_t row_count = ReadCounter(rows_key);
  WriteBatch batch;
  std::set<std::string> seen;
  std::vector<Value> empty_row;
  for (auto& exprs : st.values) {
    if (exprs.size() != targets.size()) {
      return Status::InvalidArgument("esperados " + std::to_string(targets.size()) + " valores, recebidos " +
                                     std::to_string(exprs.size()));
    }
    std::vector<Value> row(def.columns.size(), Value::Null());
    std::vector<bool> provided(def.columns.size(), false);
    for (size_t i = 0; i < targets.size(); ++i) {
      Status s = Bind(exprs[i].get(), def);
      if (!s.ok()) return s;
      row[targets[i]] = Eval(exprs[i].get(), empty_row);
      provided[targets[i]] = true;
    }
    for (size_t c = 0; c < def.columns.size(); ++c) {
      const ColumnDef& col = def.columns[c];
      if (!provided[c] && col.has_default) row[c] = col.default_value;
      if (row[c].is_null() && col.auto_increment) row[c] = Value::Int(++ai);
      Status s = Coerce(col, &row[c]);
      if (!s.ok()) return s;
      if (col.auto_increment && row[c].type == Type::kInt && row[c].i > ai) ai = row[c].i;
      if (row[c].is_null() && col.not_null) return Status::InvalidArgument("a coluna " + col.name + " não aceita NULL");
    }
    std::string key = EncodeRowKey(def.id, row[def.pk]);
    std::string existing;
    if (!seen.insert(key).second || db_->Get({}, key, &existing).ok()) {
      return Status::InvalidArgument("chave primária duplicada: " + row[def.pk].ToString());
    }
    batch.Put(key, EncodeRow(row));
    for (const auto& ix : def.indexes) batch.Put(IndexKey(ix.id, row[ix.column], row[def.pk]), "");
    ++row_count;
  }
  WriteCounter(&batch, ai_key, ai);
  WriteCounter(&batch, rows_key, row_count);
  Status s = db_->Write({}, &batch);
  if (!s.ok()) return s;
  r->affected = static_cast<int64_t>(st.values.size());
  r->tag = "INSERT " + std::to_string(r->affected);
  return Status::OK();
}

Status Engine::ExecUpdate(Statement& st, Result* r) {
  auto table = FindTable(st.table);
  if (!table) return Status::NotFound("tabela não existe: " + st.table);
  std::unique_lock<std::shared_mutex> lock(table->mu);
  const TableDef& def = table->def;
  Status s = Bind(st.where.get(), def);
  if (!s.ok()) return s;
  std::vector<std::pair<int, Expr*>> sets;
  for (auto& [name, expr] : st.assignments) {
    int idx = def.FindColumn(name);
    if (idx < 0) return Status::InvalidArgument("coluna desconhecida: " + name);
    s = Bind(expr.get(), def);
    if (!s.ok()) return s;
    sets.emplace_back(idx, expr.get());
  }
  std::vector<std::vector<Value>> rows;
  s = ScanRows(def, st.where.get(), nullptr, &rows, &r->plan);
  if (!s.ok()) return s;
  WriteBatch batch;
  std::set<std::string> new_keys;
  for (const auto& old : rows) {
    std::vector<Value> row = old;
    for (const auto& [idx, expr] : sets) row[idx] = Eval(expr, old);
    for (size_t c = 0; c < def.columns.size(); ++c) {
      s = Coerce(def.columns[c], &row[c]);
      if (!s.ok()) return s;
      if (row[c].is_null() && def.columns[c].not_null) {
        return Status::InvalidArgument("a coluna " + def.columns[c].name + " não aceita NULL");
      }
    }
    std::string old_key = EncodeRowKey(def.id, old[def.pk]);
    std::string new_key = EncodeRowKey(def.id, row[def.pk]);
    if (old_key != new_key) {
      std::string existing;
      if (!new_keys.insert(new_key).second || db_->Get({}, new_key, &existing).ok()) {
        return Status::InvalidArgument("chave primária duplicada: " + row[def.pk].ToString());
      }
      batch.Delete(old_key);
    }
    batch.Put(new_key, EncodeRow(row));
    for (const auto& ix : def.indexes) {
      std::string ok = IndexKey(ix.id, old[ix.column], old[def.pk]);
      std::string nk = IndexKey(ix.id, row[ix.column], row[def.pk]);
      if (ok != nk) {
        batch.Delete(ok);
        batch.Put(nk, "");
      }
    }
  }
  s = db_->Write({}, &batch);
  if (!s.ok()) return s;
  r->affected = static_cast<int64_t>(rows.size());
  r->tag = "UPDATE " + std::to_string(r->affected);
  return Status::OK();
}

Status Engine::ExecDelete(Statement& st, Result* r) {
  auto table = FindTable(st.table);
  if (!table) return Status::NotFound("tabela não existe: " + st.table);
  std::unique_lock<std::shared_mutex> lock(table->mu);
  const TableDef& def = table->def;
  Status s = Bind(st.where.get(), def);
  if (!s.ok()) return s;
  std::vector<std::vector<Value>> rows;
  s = ScanRows(def, st.where.get(), nullptr, &rows, &r->plan);
  if (!s.ok()) return s;
  WriteBatch batch;
  for (const auto& row : rows) {
    batch.Delete(EncodeRowKey(def.id, row[def.pk]));
    for (const auto& ix : def.indexes) batch.Delete(IndexKey(ix.id, row[ix.column], row[def.pk]));
  }
  std::string rows_key = Prefix32(kMetaPrefix, def.id) + "rows";
  WriteCounter(&batch, rows_key, std::max<int64_t>(0, ReadCounter(rows_key) - static_cast<int64_t>(rows.size())));
  s = db_->Write({}, &batch);
  if (!s.ok()) return s;
  r->affected = static_cast<int64_t>(rows.size());
  r->tag = "DELETE " + std::to_string(r->affected);
  return Status::OK();
}

Status Engine::ExecCreateTable(Statement& st, Result* r) {
  std::unique_lock<std::shared_mutex> lock(catalog_mu_);
  if (tables_.count(st.table) > 0) {
    if (st.if_not_exists) {
      r->tag = "CREATE TABLE";
      return Status::OK();
    }
    return Status::InvalidArgument("a tabela já existe: " + st.table);
  }
  if (st.columns.empty()) return Status::InvalidArgument("a tabela precisa de pelo menos uma coluna");
  TableDef def;
  def.name = st.table;
  def.columns = st.columns;
  int pk = -1;
  std::set<std::string> names;
  for (size_t i = 0; i < def.columns.size(); ++i) {
    if (!names.insert(def.columns[i].name).second) return Status::InvalidArgument("coluna repetida: " + def.columns[i].name);
    if (def.columns[i].primary_key) {
      if (pk >= 0) return Status::InvalidArgument("só uma coluna pode ser chave primária");
      pk = static_cast<int>(i);
    }
    if (def.columns[i].auto_increment && def.columns[i].type != Type::kInt) {
      return Status::InvalidArgument("AUTOINCREMENT só vale para colunas INT");
    }
    if (def.columns[i].has_default) {
      Status s = Coerce(def.columns[i], &def.columns[i].default_value);
      if (!s.ok()) return s;
    }
  }
  if (pk < 0) return Status::InvalidArgument("defina uma chave primária (PRIMARY KEY)");
  def.pk = pk;
  lock.unlock();
  def.id = NextId();
  lock.lock();
  WriteBatch batch;
  SaveTable(def, &batch);
  Status s = db_->Write({}, &batch);
  if (!s.ok()) return s;
  auto state = std::make_shared<TableState>();
  state->def = def;
  tables_[def.name] = state;
  r->tag = "CREATE TABLE";
  return Status::OK();
}

Status Engine::ExecDropTable(Statement& st, Result* r) {
  std::unique_lock<std::shared_mutex> lock(catalog_mu_);
  auto it = tables_.find(st.table);
  if (it == tables_.end()) {
    if (st.if_exists) {
      r->tag = "DROP TABLE";
      return Status::OK();
    }
    return Status::NotFound("tabela não existe: " + st.table);
  }
  auto table = it->second;
  std::unique_lock<std::shared_mutex> tlock(table->mu);
  const TableDef& def = table->def;
  WriteBatch batch;
  std::vector<std::string> prefixes = {Prefix32(kRowPrefix, def.id)};
  for (const auto& ix : def.indexes) prefixes.push_back(Prefix32(kIndexPrefix, ix.id));
  for (const auto& prefix : prefixes) {
    auto iter = db_->NewIterator({});
    for (iter->Seek(prefix); iter->Valid() && iter->key().substr(0, prefix.size()) == prefix; iter->Next()) {
      batch.Delete(iter->key());
    }
  }
  batch.Delete(std::string(1, kCatalogPrefix) + def.name);
  batch.Delete(Prefix32(kMetaPrefix, def.id) + "ai");
  batch.Delete(Prefix32(kMetaPrefix, def.id) + "rows");
  Status s = db_->Write({}, &batch);
  if (!s.ok()) return s;
  tables_.erase(it);
  r->tag = "DROP TABLE";
  return Status::OK();
}

Status Engine::ExecAlterAddColumn(Statement& st, Result* r) {
  auto table = FindTable(st.table);
  if (!table) return Status::NotFound("tabela não existe: " + st.table);
  std::unique_lock<std::shared_mutex> lock(table->mu);
  ColumnDef col = st.columns[0];
  if (table->def.FindColumn(col.name) >= 0) return Status::InvalidArgument("a coluna já existe: " + col.name);
  if (col.primary_key || col.auto_increment) return Status::InvalidArgument("não é possível adicionar outra chave primária");
  if (col.not_null && !col.has_default) return Status::InvalidArgument("coluna NOT NULL nova precisa de DEFAULT");
  if (col.has_default) {
    Status s = Coerce(col, &col.default_value);
    if (!s.ok()) return s;
  }
  TableDef def = table->def;
  def.columns.push_back(col);
  WriteBatch batch;
  if (col.has_default) {
    std::string prefix = Prefix32(kRowPrefix, def.id);
    auto it = db_->NewIterator({});
    for (it->Seek(prefix); it->Valid() && it->key().substr(0, prefix.size()) == prefix; it->Next()) {
      std::vector<Value> row;
      if (!DecodeRow(it->value(), def.columns.size() - 1, &row)) return Status::Corruption("linha inválida");
      row.push_back(col.default_value);
      batch.Put(it->key(), EncodeRow(row));
    }
  }
  SaveTable(def, &batch);
  Status s = db_->Write({}, &batch);
  if (!s.ok()) return s;
  table->def = def;
  r->tag = "ALTER TABLE";
  return Status::OK();
}

Status Engine::ExecCreateIndex(Statement& st, Result* r) {
  auto table = FindTable(st.table);
  if (!table) return Status::NotFound("tabela não existe: " + st.table);
  {
    std::shared_lock<std::shared_mutex> clock(catalog_mu_);
    for (const auto& [name, t] : tables_) {
      for (const auto& ix : t->def.indexes) {
        if (ix.name == st.index) {
          if (st.if_not_exists) {
            r->tag = "CREATE INDEX";
            return Status::OK();
          }
          return Status::InvalidArgument("o índice já existe: " + st.index);
        }
      }
    }
  }
  uint32_t id = NextId();
  std::unique_lock<std::shared_mutex> lock(table->mu);
  int col = table->def.FindColumn(st.column);
  if (col < 0) return Status::InvalidArgument("coluna desconhecida: " + st.column);
  TableDef def = table->def;
  IndexDef ix;
  ix.name = st.index;
  ix.id = id;
  ix.column = col;
  def.indexes.push_back(ix);
  WriteBatch batch;
  std::string prefix = Prefix32(kRowPrefix, def.id);
  auto it = db_->NewIterator({});
  int64_t count = 0;
  for (it->Seek(prefix); it->Valid() && it->key().substr(0, prefix.size()) == prefix; it->Next()) {
    std::vector<Value> row;
    if (!DecodeRow(it->value(), def.columns.size(), &row)) return Status::Corruption("linha inválida");
    batch.Put(IndexKey(id, row[col], row[def.pk]), "");
    ++count;
  }
  SaveTable(def, &batch);
  Status s = db_->Write({}, &batch);
  if (!s.ok()) return s;
  table->def = def;
  r->tag = "CREATE INDEX";
  r->affected = count;
  return Status::OK();
}

Status Engine::ExecDropIndex(Statement& st, Result* r) {
  std::shared_ptr<TableState> owner;
  {
    std::shared_lock<std::shared_mutex> clock(catalog_mu_);
    for (const auto& [name, t] : tables_) {
      for (const auto& ix : t->def.indexes) {
        if (ix.name == st.index) owner = t;
      }
    }
  }
  if (!owner) {
    if (st.if_exists) {
      r->tag = "DROP INDEX";
      return Status::OK();
    }
    return Status::NotFound("índice não existe: " + st.index);
  }
  std::unique_lock<std::shared_mutex> lock(owner->mu);
  TableDef def = owner->def;
  auto pos = std::find_if(def.indexes.begin(), def.indexes.end(), [&](const IndexDef& i) { return i.name == st.index; });
  if (pos == def.indexes.end()) return Status::NotFound("índice não existe: " + st.index);
  WriteBatch batch;
  std::string prefix = Prefix32(kIndexPrefix, pos->id);
  auto it = db_->NewIterator({});
  for (it->Seek(prefix); it->Valid() && it->key().substr(0, prefix.size()) == prefix; it->Next()) batch.Delete(it->key());
  def.indexes.erase(pos);
  SaveTable(def, &batch);
  Status s = db_->Write({}, &batch);
  if (!s.ok()) return s;
  owner->def = def;
  r->tag = "DROP INDEX";
  return Status::OK();
}

Status Engine::ExecShowTables(Result* r) {
  r->has_rows = true;
  r->columns = {"tabela", "colunas", "linhas", "indices"};
  r->types = {"TEXT", "INT", "INT", "INT"};
  for (const auto& t : Tables()) {
    r->rows.push_back({Value::Text(t.def.name), Value::Int(static_cast<int64_t>(t.def.columns.size())),
                       Value::Int(t.rows), Value::Int(static_cast<int64_t>(t.def.indexes.size()))});
  }
  return Status::OK();
}

Status Engine::ExecDescribe(Statement& st, Result* r) {
  auto table = FindTable(st.table);
  if (!table) return Status::NotFound("tabela não existe: " + st.table);
  std::shared_lock<std::shared_mutex> lock(table->mu);
  r->has_rows = true;
  r->columns = {"coluna", "tipo", "chave", "nulo", "padrao", "indice"};
  r->types = {"TEXT", "TEXT", "TEXT", "BOOL", "TEXT", "TEXT"};
  for (size_t i = 0; i < table->def.columns.size(); ++i) {
    const auto& c = table->def.columns[i];
    std::string index;
    for (const auto& ix : table->def.indexes) {
      if (ix.column == static_cast<int>(i)) index = ix.name;
    }
    r->rows.push_back({Value::Text(c.name), Value::Text(TypeName(c.type)),
                       Value::Text(c.primary_key ? (c.auto_increment ? "PK AUTOINCREMENT" : "PK") : ""),
                       Value::Bool(!c.not_null), c.has_default ? Value::Text(c.default_value.ToString()) : Value::Null(),
                       index.empty() ? Value::Null() : Value::Text(index)});
  }
  return Status::OK();
}

}

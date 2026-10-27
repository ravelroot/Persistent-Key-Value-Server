#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <unordered_map>

#include "server/server.hpp"
#include "util/coding.hpp"

namespace arca::server {

namespace {

enum Flags : int { kWrite = 1, kHeavy = 2, kNoAuth = 4, kNoMulti = 8, kRead = 16 };

std::string Upper(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

bool ParseInt64(const std::string& s, int64_t* out) {
  if (s.empty()) return false;
  auto r = std::from_chars(s.data(), s.data() + s.size(), *out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool ParseDouble(const std::string& s, double* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  *out = std::strtod(s.c_str(), &end);
  return end != nullptr && *end == '\0' && std::isfinite(*out);
}

std::string FormatDouble(double d) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", d);
  return buf;
}

std::string JsonString(std::string_view s) {
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

std::string JsonNumber(double d) {
  if (!std::isfinite(d)) return "0";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.3f", d);
  return buf;
}

bool GlobMatch(std::string_view pattern, std::string_view text) {
  size_t p = 0;
  size_t t = 0;
  size_t star_p = std::string_view::npos;
  size_t star_t = 0;
  while (t < text.size()) {
    if (p < pattern.size()) {
      char c = pattern[p];
      if (c == '*') {
        star_p = p++;
        star_t = t;
        continue;
      }
      if (c == '?') {
        ++p;
        ++t;
        continue;
      }
      if (c == '[') {
        size_t close = pattern.find(']', p + 1);
        if (close != std::string_view::npos) {
          bool negate = p + 1 < close && pattern[p + 1] == '^';
          bool match = false;
          for (size_t i = p + (negate ? 2 : 1); i < close; ++i) {
            if (i + 2 < close && pattern[i + 1] == '-') {
              if (text[t] >= pattern[i] && text[t] <= pattern[i + 2]) match = true;
              i += 2;
            } else if (pattern[i] == text[t]) {
              match = true;
            }
          }
          if (match != negate) {
            p = close + 1;
            ++t;
            continue;
          }
        }
      } else {
        if (c == '\\' && p + 1 < pattern.size()) c = pattern[++p];
        if (c == text[t]) {
          ++p;
          ++t;
          continue;
        }
      }
    }
    if (star_p == std::string_view::npos) return false;
    p = star_p + 1;
    t = ++star_t;
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

std::string LiteralPrefix(std::string_view pattern) {
  std::string out;
  for (size_t i = 0; i < pattern.size(); ++i) {
    char c = pattern[i];
    if (c == '*' || c == '?' || c == '[') break;
    if (c == '\\' && i + 1 < pattern.size()) c = pattern[++i];
    out.push_back(c);
  }
  return out;
}

std::string ToHex(std::string_view s) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) {
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 15]);
  }
  return out;
}

bool FromHex(std::string_view s, std::string* out) {
  if (s.size() % 2 != 0) return false;
  out->clear();
  for (size_t i = 0; i < s.size(); i += 2) {
    auto v = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    int hi = v(s[i]);
    int lo = v(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out->push_back(static_cast<char>(hi * 16 + lo));
  }
  return true;
}

std::string KvKey(std::string_view key) { return std::string(1, '\x01') + std::string(key); }

struct KvRecord {
  bool exists = false;
  int64_t expire_ms = 0;
  std::string value;
};

std::string EncodeKv(std::string_view value, int64_t expire_ms) {
  std::string out;
  out.push_back(expire_ms > 0 ? '\x01' : '\x00');
  if (expire_ms > 0) PutFixed64(&out, static_cast<uint64_t>(expire_ms));
  out.append(value.data(), value.size());
  return out;
}

bool DecodeKv(std::string_view raw, KvRecord* rec) {
  if (raw.empty()) return false;
  bool has_expire = raw[0] & 1;
  raw.remove_prefix(1);
  rec->expire_ms = 0;
  if (has_expire) {
    if (raw.size() < 8) return false;
    rec->expire_ms = static_cast<int64_t>(DecodeFixed64(raw.data()));
    raw.remove_prefix(8);
  }
  rec->value.assign(raw.data(), raw.size());
  rec->exists = true;
  return true;
}

void PutTtlIndex(WriteBatch* batch, std::string_view key, int64_t expire_ms) {
  std::string k(1, '\x06');
  PutBigEndian64(&k, static_cast<uint64_t>(expire_ms));
  k.append(key.data(), key.size());
  batch->Put(k, "");
}

}

struct Server::CommandSpec {
  const char* name;
  int arity;
  int flags;
  void (*fn)(Server&, Connection&, Args&, std::string*);
};

struct Commands {
  static Status ReadKv(Server& s, Connection& c, std::string_view key, KvRecord* rec) {
    std::string raw;
    ReadOptions ro;
    ro.snapshot = c.session.snapshot.get();
    Status st = s.db_->Get(ro, KvKey(key), &raw);
    *rec = KvRecord();
    if (st.IsNotFound()) return Status::OK();
    if (!st.ok()) return st;
    if (!DecodeKv(raw, rec)) return Status::Corruption("valor de chave inválido");
    if (rec->expire_ms > 0 && rec->expire_ms <= NowMs()) *rec = KvRecord();
    return Status::OK();
  }

  static void WriteKv(WriteBatch* batch, std::string_view key, std::string_view value, int64_t expire_ms) {
    batch->Put(KvKey(key), EncodeKv(value, expire_ms));
    if (expire_ms > 0) PutTtlIndex(batch, key, expire_ms);
  }

  static bool Commit(Server& s, WriteBatch* batch, std::string* out) {
    Status st = s.db_->Write({}, batch);
    if (!st.ok()) {
      resp::AppendError(out, "ERR " + st.ToString());
      return false;
    }
    s.writes_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  static bool CheckRead(Server& s, const Status& st, std::string* out) {
    if (st.ok()) return true;
    resp::AppendError(out, "ERR " + st.ToString());
    (void)s;
    return false;
  }

  static void CountRead(Server& s, bool hit) {
    s.reads_.fetch_add(1, std::memory_order_relaxed);
    (hit ? s.hits_ : s.misses_).fetch_add(1, std::memory_order_relaxed);
  }

  static void Ping(Server&, Connection&, Args& a, std::string* out) {
    if (a.size() > 1) {
      resp::AppendBulk(out, a[1]);
    } else {
      resp::AppendSimple(out, "PONG");
    }
  }

  static void Echo(Server&, Connection&, Args& a, std::string* out) { resp::AppendBulk(out, a[1]); }

  static void Auth(Server& s, Connection& c, Args& a, std::string* out) {
    const std::string& pass = a.size() == 3 ? a[2] : a[1];
    std::string expected;
    {
      std::lock_guard<std::mutex> lock(s.config_mu_);
      expected = s.config_.requirepass;
    }
    if (expected.empty()) {
      resp::AppendError(out, "ERR o servidor não tem senha configurada");
      return;
    }
    if (pass != expected) {
      resp::AppendError(out, "WRONGPASS senha inválida");
      return;
    }
    c.session.authenticated = true;
    resp::AppendSimple(out, "OK");
  }

  static void Hello(Server& s, Connection& c, Args& a, std::string* out) {
    size_t i = 1;
    if (a.size() > 1) {
      int64_t proto = 0;
      if (!ParseInt64(a[1], &proto)) {
        resp::AppendError(out, "ERR versão de protocolo inválida");
        return;
      }
      if (proto != 2) {
        resp::AppendError(out, "NOPROTO a Arca fala apenas RESP2");
        return;
      }
      i = 2;
    }
    while (i < a.size()) {
      std::string opt = Upper(a[i]);
      if (opt == "AUTH" && i + 2 < a.size()) {
        Args auth = {"AUTH", a[i + 1], a[i + 2]};
        std::string reply;
        Auth(s, c, auth, &reply);
        if (reply[0] == '-') {
          out->append(reply);
          return;
        }
        i += 3;
      } else if (opt == "SETNAME" && i + 1 < a.size()) {
        std::lock_guard<std::mutex> lock(c.info_mu);
        c.name = a[i + 1];
        i += 2;
      } else {
        resp::AppendError(out, "ERR opção desconhecida em HELLO");
        return;
      }
    }
    if (!c.session.authenticated) {
      resp::AppendError(out, "NOAUTH autenticação necessária");
      return;
    }
    resp::AppendArray(out, 14);
    resp::AppendBulk(out, "server");
    resp::AppendBulk(out, "arca");
    resp::AppendBulk(out, "version");
    resp::AppendBulk(out, kVersion);
    resp::AppendBulk(out, "proto");
    resp::AppendInteger(out, 2);
    resp::AppendBulk(out, "id");
    resp::AppendInteger(out, static_cast<int64_t>(c.id));
    resp::AppendBulk(out, "mode");
    resp::AppendBulk(out, "standalone");
    resp::AppendBulk(out, "role");
    resp::AppendBulk(out, "master");
    resp::AppendBulk(out, "modules");
    resp::AppendArray(out, 0);
  }

  static void Quit(Server&, Connection& c, Args&, std::string* out) {
    resp::AppendSimple(out, "OK");
    c.closing = true;
  }

  static void Select(Server&, Connection&, Args& a, std::string* out) {
    if (a[1] != "0") {
      resp::AppendError(out, "ERR a Arca tem um único banco (0)");
      return;
    }
    resp::AppendSimple(out, "OK");
  }

  static void Command(Server&, Connection&, Args& a, std::string* out);

  static void Time(Server&, Connection&, Args&, std::string* out) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    resp::AppendArray(out, 2);
    resp::AppendBulk(out, std::to_string(us / 1000000));
    resp::AppendBulk(out, std::to_string(us % 1000000));
  }

  static void Get(Server& s, Connection& c, Args& a, std::string* out) {
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    CountRead(s, rec.exists);
    if (rec.exists) {
      resp::AppendBulk(out, rec.value);
    } else {
      resp::AppendNull(out);
    }
  }

  static void Set(Server& s, Connection& c, Args& a, std::string* out) {
    int64_t expire_ms = 0;
    bool nx = false;
    bool xx = false;
    bool get = false;
    bool keepttl = false;
    for (size_t i = 3; i < a.size(); ++i) {
      std::string opt = Upper(a[i]);
      int64_t n = 0;
      if ((opt == "EX" || opt == "PX" || opt == "EXAT" || opt == "PXAT") && i + 1 < a.size()) {
        if (!ParseInt64(a[++i], &n) || n <= 0) {
          resp::AppendError(out, "ERR tempo de expiração inválido");
          return;
        }
        if (opt == "EX") expire_ms = NowMs() + n * 1000;
        if (opt == "PX") expire_ms = NowMs() + n;
        if (opt == "EXAT") expire_ms = n * 1000;
        if (opt == "PXAT") expire_ms = n;
      } else if (opt == "NX") {
        nx = true;
      } else if (opt == "XX") {
        xx = true;
      } else if (opt == "GET") {
        get = true;
      } else if (opt == "KEEPTTL") {
        keepttl = true;
      } else {
        resp::AppendError(out, "ERR erro de sintaxe");
        return;
      }
    }
    if (nx && xx) {
      resp::AppendError(out, "ERR erro de sintaxe");
      return;
    }
    auto locks = s.LockKeys({a[1]});
    KvRecord old;
    if (nx || xx || get || keepttl) {
      if (!CheckRead(s, ReadKv(s, c, a[1], &old), out)) return;
    }
    if ((nx && old.exists) || (xx && !old.exists)) {
      if (get) {
        old.exists ? resp::AppendBulk(out, old.value) : resp::AppendNull(out);
      } else {
        resp::AppendNull(out);
      }
      return;
    }
    if (keepttl && old.exists) expire_ms = old.expire_ms;
    WriteBatch batch;
    WriteKv(&batch, a[1], a[2], expire_ms);
    if (!Commit(s, &batch, out)) return;
    if (get) {
      old.exists ? resp::AppendBulk(out, old.value) : resp::AppendNull(out);
    } else {
      resp::AppendSimple(out, "OK");
    }
  }

  static void SetNx(Server& s, Connection& c, Args& a, std::string* out) {
    Args args = {"SET", a[1], a[2], "NX"};
    std::string reply;
    Set(s, c, args, &reply);
    resp::AppendInteger(out, reply[0] == '+' ? 1 : 0);
  }

  static void SetEx(Server& s, Connection& c, Args& a, std::string* out) {
    Args args = {"SET", a[1], a[3], Upper(a[0]) == "PSETEX" ? "PX" : "EX", a[2]};
    Set(s, c, args, out);
  }

  static void GetSet(Server& s, Connection& c, Args& a, std::string* out) {
    Args args = {"SET", a[1], a[2], "GET"};
    Set(s, c, args, out);
  }

  static void GetDel(Server& s, Connection& c, Args& a, std::string* out) {
    auto locks = s.LockKeys({a[1]});
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    if (!rec.exists) {
      resp::AppendNull(out);
      return;
    }
    WriteBatch batch;
    batch.Delete(KvKey(a[1]));
    if (!Commit(s, &batch, out)) return;
    resp::AppendBulk(out, rec.value);
  }

  static void MGet(Server& s, Connection& c, Args& a, std::string* out) {
    resp::AppendArray(out, a.size() - 1);
    for (size_t i = 1; i < a.size(); ++i) {
      KvRecord rec;
      ReadKv(s, c, a[i], &rec);
      CountRead(s, rec.exists);
      rec.exists ? resp::AppendBulk(out, rec.value) : resp::AppendNull(out);
    }
  }

  static void MSet(Server& s, Connection&, Args& a, std::string* out) {
    if (a.size() % 2 != 1) {
      resp::AppendError(out, "ERR número de argumentos inválido para 'mset'");
      return;
    }
    std::vector<std::string> keys;
    for (size_t i = 1; i < a.size(); i += 2) keys.push_back(a[i]);
    auto locks = s.LockKeys(keys);
    WriteBatch batch;
    for (size_t i = 1; i < a.size(); i += 2) WriteKv(&batch, a[i], a[i + 1], 0);
    if (Commit(s, &batch, out)) resp::AppendSimple(out, "OK");
  }

  static void Del(Server& s, Connection& c, Args& a, std::string* out) {
    std::vector<std::string> keys(a.begin() + 1, a.end());
    auto locks = s.LockKeys(keys);
    WriteBatch batch;
    int64_t removed = 0;
    for (const auto& k : keys) {
      KvRecord rec;
      if (!CheckRead(s, ReadKv(s, c, k, &rec), out)) return;
      if (rec.exists) {
        batch.Delete(KvKey(k));
        ++removed;
      }
    }
    if (removed > 0 && !Commit(s, &batch, out)) return;
    resp::AppendInteger(out, removed);
  }

  static void Exists(Server& s, Connection& c, Args& a, std::string* out) {
    int64_t n = 0;
    for (size_t i = 1; i < a.size(); ++i) {
      KvRecord rec;
      ReadKv(s, c, a[i], &rec);
      n += rec.exists ? 1 : 0;
    }
    resp::AppendInteger(out, n);
  }

  static void IncrBy(Server& s, Connection& c, Args& a, std::string* out) {
    std::string cmd = Upper(a[0]);
    int64_t delta = 1;
    if (cmd == "DECR") delta = -1;
    if (cmd == "INCRBY" || cmd == "DECRBY") {
      if (!ParseInt64(a[2], &delta)) {
        resp::AppendError(out, "ERR o valor não é um inteiro ou está fora do intervalo");
        return;
      }
      if (cmd == "DECRBY") delta = -delta;
    }
    auto locks = s.LockKeys({a[1]});
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    int64_t current = 0;
    if (rec.exists && !ParseInt64(rec.value, &current)) {
      resp::AppendError(out, "ERR o valor não é um inteiro ou está fora do intervalo");
      return;
    }
    int64_t result = 0;
    if ((delta > 0 && current > INT64_MAX - delta) || (delta < 0 && current < INT64_MIN - delta)) {
      resp::AppendError(out, "ERR incremento estouraria o limite de 64 bits");
      return;
    }
    result = current + delta;
    WriteBatch batch;
    WriteKv(&batch, a[1], std::to_string(result), rec.exists ? rec.expire_ms : 0);
    if (Commit(s, &batch, out)) resp::AppendInteger(out, result);
  }

  static void IncrByFloat(Server& s, Connection& c, Args& a, std::string* out) {
    double delta = 0;
    if (!ParseDouble(a[2], &delta)) {
      resp::AppendError(out, "ERR o valor não é um número válido");
      return;
    }
    auto locks = s.LockKeys({a[1]});
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    double current = 0;
    if (rec.exists && !ParseDouble(rec.value, &current)) {
      resp::AppendError(out, "ERR o valor não é um número válido");
      return;
    }
    std::string result = FormatDouble(current + delta);
    WriteBatch batch;
    WriteKv(&batch, a[1], result, rec.exists ? rec.expire_ms : 0);
    if (Commit(s, &batch, out)) resp::AppendBulk(out, result);
  }

  static void Append(Server& s, Connection& c, Args& a, std::string* out) {
    auto locks = s.LockKeys({a[1]});
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    std::string value = rec.value + a[2];
    WriteBatch batch;
    WriteKv(&batch, a[1], value, rec.exists ? rec.expire_ms : 0);
    if (Commit(s, &batch, out)) resp::AppendInteger(out, static_cast<int64_t>(value.size()));
  }

  static void StrLen(Server& s, Connection& c, Args& a, std::string* out) {
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    resp::AppendInteger(out, static_cast<int64_t>(rec.value.size()));
  }

  static void Expire(Server& s, Connection& c, Args& a, std::string* out) {
    std::string cmd = Upper(a[0]);
    int64_t n = 0;
    if (!ParseInt64(a[2], &n)) {
      resp::AppendError(out, "ERR o valor não é um inteiro ou está fora do intervalo");
      return;
    }
    int64_t expire_ms = cmd == "EXPIRE"     ? NowMs() + n * 1000
                        : cmd == "PEXPIRE"  ? NowMs() + n
                        : cmd == "EXPIREAT" ? n * 1000
                                            : n;
    auto locks = s.LockKeys({a[1]});
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    if (!rec.exists) {
      resp::AppendInteger(out, 0);
      return;
    }
    WriteBatch batch;
    if (expire_ms <= NowMs()) {
      batch.Delete(KvKey(a[1]));
    } else {
      WriteKv(&batch, a[1], rec.value, expire_ms);
    }
    if (Commit(s, &batch, out)) resp::AppendInteger(out, 1);
  }

  static void Ttl(Server& s, Connection& c, Args& a, std::string* out) {
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    if (!rec.exists) {
      resp::AppendInteger(out, -2);
    } else if (rec.expire_ms == 0) {
      resp::AppendInteger(out, -1);
    } else {
      int64_t left = rec.expire_ms - NowMs();
      resp::AppendInteger(out, Upper(a[0]) == "PTTL" ? left : (left + 999) / 1000);
    }
  }

  static void Persist(Server& s, Connection& c, Args& a, std::string* out) {
    auto locks = s.LockKeys({a[1]});
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    if (!rec.exists || rec.expire_ms == 0) {
      resp::AppendInteger(out, 0);
      return;
    }
    WriteBatch batch;
    WriteKv(&batch, a[1], rec.value, 0);
    if (Commit(s, &batch, out)) resp::AppendInteger(out, 1);
  }

  static void Type(Server& s, Connection& c, Args& a, std::string* out) {
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    resp::AppendSimple(out, rec.exists ? "string" : "none");
  }

  static void Rename(Server& s, Connection& c, Args& a, std::string* out) {
    bool nx = Upper(a[0]) == "RENAMENX";
    auto locks = s.LockKeys({a[1], a[2]});
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    if (!rec.exists) {
      resp::AppendError(out, "ERR a chave não existe");
      return;
    }
    if (nx) {
      KvRecord dst;
      ReadKv(s, c, a[2], &dst);
      if (dst.exists) {
        resp::AppendInteger(out, 0);
        return;
      }
    }
    WriteBatch batch;
    if (a[1] != a[2]) batch.Delete(KvKey(a[1]));
    WriteKv(&batch, a[2], rec.value, rec.expire_ms);
    if (!Commit(s, &batch, out)) return;
    nx ? resp::AppendInteger(out, 1) : resp::AppendSimple(out, "OK");
  }

  struct ScanItem {
    std::string key;
    int64_t ttl_ms;
    size_t size;
  };

  static std::string ScanKeys(Server& s, Connection& c, const std::string& cursor, const std::string& pattern,
                              size_t count, std::vector<ScanItem>* items) {
    std::string start = KvKey(LiteralPrefix(pattern));
    if (cursor != "0") {
      std::string decoded;
      if (FromHex(cursor, &decoded)) start = KvKey(decoded);
    }
    std::string prefix = KvKey(LiteralPrefix(pattern));
    ReadOptions ro;
    ro.snapshot = c.session.snapshot.get();
    ro.fill_cache = false;
    auto it = s.db_->NewIterator(ro);
    int64_t now = NowMs();
    size_t examined = 0;
    for (it->Seek(start); it->Valid(); it->Next()) {
      std::string_view k = it->key();
      if (k.empty() || k[0] != '\x01' || k.substr(0, prefix.size()) != prefix) return "0";
      if (examined >= count * 10 || items->size() >= count) return ToHex(k.substr(1));
      ++examined;
      std::string_view user = k.substr(1);
      KvRecord rec;
      if (!DecodeKv(it->value(), &rec)) continue;
      if (rec.expire_ms > 0 && rec.expire_ms <= now) continue;
      if (!GlobMatch(pattern, user)) continue;
      items->push_back(ScanItem{std::string(user), rec.expire_ms > 0 ? rec.expire_ms - now : -1, rec.value.size()});
    }
    return "0";
  }

  static bool ParseScanArgs(Args& a, size_t from, std::string* pattern, size_t* count, std::string* out) {
    *pattern = "*";
    *count = 10;
    for (size_t i = from; i < a.size(); ++i) {
      std::string opt = Upper(a[i]);
      if (opt == "MATCH" && i + 1 < a.size()) {
        *pattern = a[++i];
      } else if (opt == "COUNT" && i + 1 < a.size()) {
        int64_t n = 0;
        if (!ParseInt64(a[++i], &n) || n <= 0) {
          resp::AppendError(out, "ERR COUNT inválido");
          return false;
        }
        *count = static_cast<size_t>(std::min<int64_t>(n, 100000));
      } else if (opt == "TYPE" && i + 1 < a.size()) {
        ++i;
      } else {
        resp::AppendError(out, "ERR erro de sintaxe");
        return false;
      }
    }
    return true;
  }

  static void Scan(Server& s, Connection& c, Args& a, std::string* out) {
    std::string pattern;
    size_t count;
    if (!ParseScanArgs(a, 2, &pattern, &count, out)) return;
    std::vector<ScanItem> items;
    std::string next = ScanKeys(s, c, a[1], pattern, count, &items);
    resp::AppendArray(out, 2);
    resp::AppendBulk(out, next);
    resp::AppendArray(out, items.size());
    for (const auto& item : items) resp::AppendBulk(out, item.key);
  }

  static void ScanJson(Server& s, Connection& c, Args& a, std::string* out) {
    int64_t count = 50;
    if (a.size() > 3 && (!ParseInt64(a[3], &count) || count <= 0)) {
      resp::AppendError(out, "ERR COUNT inválido");
      return;
    }
    std::vector<ScanItem> items;
    std::string next = ScanKeys(s, c, a[1], a.size() > 2 ? a[2] : "*", static_cast<size_t>(std::min<int64_t>(count, 10000)), &items);
    std::string json = "{\"cursor\":" + JsonString(next) + ",\"keys\":[";
    for (size_t i = 0; i < items.size(); ++i) {
      if (i > 0) json += ",";
      json += "{\"key\":" + JsonString(items[i].key) + ",\"type\":\"string\",\"ttl_ms\":" +
              std::to_string(items[i].ttl_ms) + ",\"size\":" + std::to_string(items[i].size) + "}";
    }
    json += "]}";
    resp::AppendBulk(out, json);
  }

  static void Keys(Server& s, Connection& c, Args& a, std::string* out) {
    std::vector<ScanItem> items;
    ScanKeys(s, c, "0", a[1], SIZE_MAX / 20, &items);
    resp::AppendArray(out, items.size());
    for (const auto& item : items) resp::AppendBulk(out, item.key);
  }

  static void DbSize(Server& s, Connection& c, Args&, std::string* out) {
    std::vector<ScanItem> items;
    ScanKeys(s, c, "0", "*", SIZE_MAX / 20, &items);
    resp::AppendInteger(out, static_cast<int64_t>(items.size()));
  }

  static void FlushDb(Server& s, Connection&, Args&, std::string* out) {
    auto it = s.db_->NewIterator({});
    WriteBatch batch;
    for (it->Seek(std::string(1, '\x01')); it->Valid(); it->Next()) {
      char p = it->key()[0];
      if (p != '\x01' && p != '\x06') break;
      batch.Delete(it->key());
      if (batch.ApproximateSize() > (4u << 20)) {
        s.db_->Write({}, &batch);
        batch.Clear();
      }
    }
    for (it->Seek(std::string(1, '\x06')); it->Valid() && it->key()[0] == '\x06'; it->Next()) batch.Delete(it->key());
    if (Commit(s, &batch, out)) resp::AppendSimple(out, "OK");
  }

  static void Multi(Server&, Connection& c, Args&, std::string* out) {
    if (c.session.in_multi) {
      resp::AppendError(out, "ERR MULTI não pode ser aninhado");
      return;
    }
    c.session.in_multi = true;
    c.session.multi_error = false;
    c.session.queued.clear();
    resp::AppendSimple(out, "OK");
  }

  static void Discard(Server&, Connection& c, Args&, std::string* out) {
    if (!c.session.in_multi) {
      resp::AppendError(out, "ERR DISCARD sem MULTI");
      return;
    }
    c.session.in_multi = false;
    c.session.queued.clear();
    resp::AppendSimple(out, "OK");
  }

  static void Info(Server& s, Connection&, Args& a, std::string* out);
  static void Stats(Server& s, Connection&, Args&, std::string* out);
  static void Client(Server& s, Connection& c, Args& a, std::string* out);
  static void Config(Server& s, Connection&, Args& a, std::string* out);
  static void Slowlog(Server& s, Connection&, Args& a, std::string* out);

  static void Save(Server& s, Connection&, Args&, std::string* out) {
    Status st = s.db_->Flush();
    if (st.ok()) st = s.db_->SyncWal();
    if (!st.ok()) {
      resp::AppendError(out, "ERR " + st.ToString());
      return;
    }
    s.last_save_s_.store(NowMs() / 1000);
    resp::AppendSimple(out, "OK");
  }

  static void LastSave(Server& s, Connection&, Args&, std::string* out) { resp::AppendInteger(out, s.last_save_s_.load()); }

  static void Compact(Server& s, Connection&, Args&, std::string* out) {
    Status st = s.db_->CompactAll();
    st.ok() ? resp::AppendSimple(out, "OK") : resp::AppendError(out, "ERR " + st.ToString());
  }

  static void FlushMem(Server& s, Connection&, Args&, std::string* out) {
    Status st = s.db_->Flush();
    st.ok() ? resp::AppendSimple(out, "OK") : resp::AppendError(out, "ERR " + st.ToString());
  }

  static void Checkpoint(Server& s, Connection&, Args& a, std::string* out) {
    std::string name = a.size() > 1 ? a[1] : "checkpoint-" + std::to_string(NowMs() / 1000);
    for (char ch : name) {
      if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')) {
        resp::AppendError(out, "ERR use só letras, números, - e _ no nome do checkpoint");
        return;
      }
    }
    std::filesystem::path base = CheckpointBase(s);
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    std::string target = (base / name).string();
    Status st = s.db_->Checkpoint(target);
    if (!st.ok()) {
      resp::AppendError(out, "ERR " + st.ToString());
      return;
    }
    s.last_save_s_.store(NowMs() / 1000);
    resp::AppendBulk(out, target);
  }

  static void Snapshot(Server& s, Connection& c, Args& a, std::string* out) {
    std::string op = Upper(a[1]);
    if (op == "BEGIN") {
      c.session.snapshot = s.db_->GetSnapshot();
      resp::AppendInteger(out, static_cast<int64_t>(c.session.snapshot->sequence()));
    } else if (op == "END") {
      c.session.snapshot.reset();
      resp::AppendSimple(out, "OK");
    } else {
      resp::AppendError(out, "ERR use ARCA.SNAPSHOT BEGIN ou END");
    }
  }

  static void Shutdown(Server& s, Connection&, Args& a, std::string* out) {
    bool save = !(a.size() > 1 && Upper(a[1]) == "NOSAVE");
    s.RequestShutdown(save);
    resp::AppendSimple(out, "OK");
  }

  static void SqlCommand(Server& s, Connection& c, Args& a, std::string* out) {
    std::string text;
    for (size_t i = 1; i < a.size(); ++i) {
      if (i > 1) text.push_back(' ');
      text += a[i];
    }
    sql::Result r;
    Status st = s.sql_->Execute(text, &r, c.session.snapshot.get());
    if (!st.ok()) {
      resp::AppendError(out, "ERR " + st.message());
      return;
    }
    bool json = Upper(a[0]) == "SQL.JSON";
    if (r.has_rows) {
      s.reads_.fetch_add(1, std::memory_order_relaxed);
    } else {
      s.writes_.fetch_add(1, std::memory_order_relaxed);
    }
    if (json) {
      std::string j;
      if (r.has_rows) {
        j = "{\"kind\":\"rows\",\"columns\":[";
        for (size_t i = 0; i < r.columns.size(); ++i) j += (i ? "," : "") + JsonString(r.columns[i]);
        j += "],\"types\":[";
        for (size_t i = 0; i < r.types.size(); ++i) j += (i ? "," : "") + JsonString(r.types[i]);
        j += "],\"rows\":[";
        for (size_t i = 0; i < r.rows.size(); ++i) {
          j += i ? ",[" : "[";
          for (size_t k = 0; k < r.rows[i].size(); ++k) j += (k ? "," : "") + r.rows[i][k].ToJson();
          j += "]";
        }
        j += "],\"plan\":" + JsonString(r.plan) + ",\"ms\":" + JsonNumber(r.ms) + "}";
      } else {
        j = "{\"kind\":\"command\",\"tag\":" + JsonString(r.tag) + ",\"affected\":" + std::to_string(r.affected) +
            ",\"ms\":" + JsonNumber(r.ms) + "}";
      }
      resp::AppendBulk(out, j);
      return;
    }
    if (!r.has_rows) {
      resp::AppendSimple(out, r.tag);
      return;
    }
    resp::AppendArray(out, 2);
    resp::AppendArray(out, r.columns.size());
    for (const auto& col : r.columns) resp::AppendBulk(out, col);
    resp::AppendArray(out, r.rows.size());
    for (const auto& row : r.rows) {
      resp::AppendArray(out, row.size());
      for (const auto& v : row) {
        if (v.is_null()) {
          resp::AppendNull(out);
        } else if (v.type == sql::Type::kInt) {
          resp::AppendInteger(out, v.i);
        } else {
          resp::AppendBulk(out, v.ToString());
        }
      }
    }
  }

  static void SqlTables(Server& s, Connection&, Args&, std::string* out) {
    std::string j = "[";
    bool first = true;
    for (const auto& t : s.sql_->Tables()) {
      if (!first) j += ",";
      first = false;
      j += "{\"name\":" + JsonString(t.def.name) + ",\"columns\":[";
      for (size_t i = 0; i < t.def.columns.size(); ++i) {
        const auto& col = t.def.columns[i];
        j += (i ? ",{" : "{");
        j += "\"name\":" + JsonString(col.name) + ",\"type\":\"" + sql::TypeName(col.type) +
             "\",\"primary_key\":" + (col.primary_key ? "true" : "false") +
             ",\"not_null\":" + (col.not_null ? "true" : "false") +
             ",\"default\":" + (col.has_default ? col.default_value.ToJson() : "null") +
             ",\"auto_increment\":" + (col.auto_increment ? "true" : "false") + "}";
      }
      j += "],\"indexes\":[";
      for (size_t i = 0; i < t.def.indexes.size(); ++i) {
        j += (i ? ",{" : "{");
        j += "\"name\":" + JsonString(t.def.indexes[i].name) + ",\"column\":" +
             JsonString(t.def.columns[t.def.indexes[i].column].name) + "}";
      }
      j += "],\"rows\":" + std::to_string(t.rows) + "}";
    }
    j += "]";
    resp::AppendBulk(out, j);
  }


  static std::filesystem::path CheckpointBase(Server& s) {
    std::filesystem::path parent = std::filesystem::path(s.config_.dir).parent_path();
    return parent.empty() ? std::filesystem::path("checkpoints") : parent / "checkpoints";
  }

  static bool ParseExpireOption(Args& a, size_t i, int64_t* expire_ms, bool* persist, bool* given, std::string* out) {
    *given = false;
    *persist = false;
    *expire_ms = 0;
    while (i < a.size()) {
      std::string opt = Upper(a[i]);
      if (*given) {
        resp::AppendError(out, "ERR erro de sintaxe");
        return false;
      }
      if (opt == "PERSIST") {
        *persist = true;
        *given = true;
        ++i;
        continue;
      }
      if ((opt == "EX" || opt == "PX" || opt == "EXAT" || opt == "PXAT") && i + 1 < a.size()) {
        int64_t n = 0;
        if (!ParseInt64(a[i + 1], &n) || n <= 0) {
          resp::AppendError(out, "ERR tempo de expiração inválido");
          return false;
        }
        if (opt == "EX") *expire_ms = NowMs() + n * 1000;
        if (opt == "PX") *expire_ms = NowMs() + n;
        if (opt == "EXAT") *expire_ms = n * 1000;
        if (opt == "PXAT") *expire_ms = n;
        *given = true;
        i += 2;
        continue;
      }
      resp::AppendError(out, "ERR erro de sintaxe");
      return false;
    }
    return true;
  }

  static void GetEx(Server& s, Connection& c, Args& a, std::string* out) {
    int64_t expire_ms = 0;
    bool persist = false;
    bool given = false;
    if (!ParseExpireOption(a, 2, &expire_ms, &persist, &given, out)) return;
    auto locks = s.LockKeys({a[1]});
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    CountRead(s, rec.exists);
    if (!rec.exists) {
      resp::AppendNull(out);
      return;
    }
    if (given && !(persist && rec.expire_ms == 0)) {
      WriteBatch batch;
      if (!persist && expire_ms <= NowMs()) {
        batch.Delete(KvKey(a[1]));
      } else {
        WriteKv(&batch, a[1], rec.value, persist ? 0 : expire_ms);
      }
      if (!Commit(s, &batch, out)) return;
    }
    resp::AppendBulk(out, rec.value);
  }

  static void GetRange(Server& s, Connection& c, Args& a, std::string* out) {
    int64_t start = 0;
    int64_t end = 0;
    if (!ParseInt64(a[2], &start) || !ParseInt64(a[3], &end)) {
      resp::AppendError(out, "ERR o valor não é um inteiro ou está fora do intervalo");
      return;
    }
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    CountRead(s, rec.exists);
    int64_t len = static_cast<int64_t>(rec.value.size());
    if (start < 0 && end < 0 && start > end) {
      resp::AppendBulk(out, "");
      return;
    }
    if (start < 0) start += len;
    if (end < 0) end += len;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (end >= len) end = len - 1;
    if (len == 0 || start > end) {
      resp::AppendBulk(out, "");
      return;
    }
    resp::AppendBulk(out, std::string_view(rec.value).substr(static_cast<size_t>(start), static_cast<size_t>(end - start + 1)));
  }

  static void SetRange(Server& s, Connection& c, Args& a, std::string* out) {
    int64_t offset = 0;
    if (!ParseInt64(a[2], &offset) || offset < 0) {
      resp::AppendError(out, "ERR o deslocamento está fora do intervalo");
      return;
    }
    if (offset + static_cast<int64_t>(a[3].size()) > (512LL << 20)) {
      resp::AppendError(out, "ERR o valor passaria do limite de 512 MB");
      return;
    }
    auto locks = s.LockKeys({a[1]});
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    if (a[3].empty()) {
      resp::AppendInteger(out, static_cast<int64_t>(rec.value.size()));
      return;
    }
    std::string value = rec.value;
    size_t need = static_cast<size_t>(offset) + a[3].size();
    if (value.size() < need) value.resize(need, '\0');
    value.replace(static_cast<size_t>(offset), a[3].size(), a[3]);
    WriteBatch batch;
    WriteKv(&batch, a[1], value, rec.exists ? rec.expire_ms : 0);
    if (Commit(s, &batch, out)) resp::AppendInteger(out, static_cast<int64_t>(value.size()));
  }

  static void Copy(Server& s, Connection& c, Args& a, std::string* out) {
    bool replace = false;
    for (size_t i = 3; i < a.size(); ++i) {
      std::string opt = Upper(a[i]);
      if (opt == "REPLACE") {
        replace = true;
      } else if (opt == "DB" && i + 1 < a.size()) {
        if (a[++i] != "0") {
          resp::AppendError(out, "ERR a Arca tem um único banco (0)");
          return;
        }
      } else {
        resp::AppendError(out, "ERR erro de sintaxe");
        return;
      }
    }
    if (a[1] == a[2]) {
      resp::AppendError(out, "ERR origem e destino são a mesma chave");
      return;
    }
    auto locks = s.LockKeys({a[1], a[2]});
    KvRecord src;
    if (!CheckRead(s, ReadKv(s, c, a[1], &src), out)) return;
    if (!src.exists) {
      resp::AppendInteger(out, 0);
      return;
    }
    if (!replace) {
      KvRecord dst;
      if (!CheckRead(s, ReadKv(s, c, a[2], &dst), out)) return;
      if (dst.exists) {
        resp::AppendInteger(out, 0);
        return;
      }
    }
    WriteBatch batch;
    WriteKv(&batch, a[2], src.value, src.expire_ms);
    if (Commit(s, &batch, out)) resp::AppendInteger(out, 1);
  }

  static void Touch(Server& s, Connection& c, Args& a, std::string* out) {
    int64_t n = 0;
    for (size_t i = 1; i < a.size(); ++i) {
      KvRecord rec;
      if (!CheckRead(s, ReadKv(s, c, a[i], &rec), out)) return;
      n += rec.exists ? 1 : 0;
    }
    resp::AppendInteger(out, n);
  }

  static void RandomKey(Server& s, Connection& c, Args&, std::string* out) {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    ReadOptions ro;
    ro.snapshot = c.session.snapshot.get();
    ro.fill_cache = false;
    auto it = s.db_->NewIterator(ro);
    int64_t now = NowMs();
    std::string start = KvKey(std::string(1, static_cast<char>(rng() & 0xff)));
    for (int pass = 0; pass < 2; ++pass) {
      size_t examined = 0;
      for (it->Seek(pass == 0 ? start : KvKey("")); it->Valid() && examined < 100000; it->Next(), ++examined) {
        std::string_view k = it->key();
        if (k.empty() || k[0] != '\x01') break;
        KvRecord rec;
        if (!DecodeKv(it->value(), &rec)) continue;
        if (rec.expire_ms > 0 && rec.expire_ms <= now) continue;
        resp::AppendBulk(out, k.substr(1));
        return;
      }
    }
    resp::AppendNull(out);
  }

  static void ExpireTime(Server& s, Connection& c, Args& a, std::string* out) {
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[1], &rec), out)) return;
    if (!rec.exists) {
      resp::AppendInteger(out, -2);
    } else if (rec.expire_ms == 0) {
      resp::AppendInteger(out, -1);
    } else {
      resp::AppendInteger(out, Upper(a[0]) == "PEXPIRETIME" ? rec.expire_ms : rec.expire_ms / 1000);
    }
  }

  static void MSetNx(Server& s, Connection& c, Args& a, std::string* out) {
    if (a.size() % 2 != 1) {
      resp::AppendError(out, "ERR número de argumentos inválido para 'msetnx'");
      return;
    }
    std::vector<std::string> keys;
    for (size_t i = 1; i < a.size(); i += 2) keys.push_back(a[i]);
    auto locks = s.LockKeys(keys);
    for (const auto& k : keys) {
      KvRecord rec;
      if (!CheckRead(s, ReadKv(s, c, k, &rec), out)) return;
      if (rec.exists) {
        resp::AppendInteger(out, 0);
        return;
      }
    }
    WriteBatch batch;
    for (size_t i = 1; i < a.size(); i += 2) WriteKv(&batch, a[i], a[i + 1], 0);
    if (Commit(s, &batch, out)) resp::AppendInteger(out, 1);
  }

  static void Memory(Server& s, Connection& c, Args& a, std::string* out) {
    std::string sub = Upper(a[1]);
    if (sub != "USAGE" || a.size() < 3) {
      resp::AppendError(out, "ERR use MEMORY USAGE chave");
      return;
    }
    KvRecord rec;
    if (!CheckRead(s, ReadKv(s, c, a[2], &rec), out)) return;
    if (!rec.exists) {
      resp::AppendNull(out);
      return;
    }
    // chave interna (prefixo + chave + 8 bytes de sequência/tipo) + valor codificado + entrada da skip list/bloco
    int64_t bytes = static_cast<int64_t>(1 + a[2].size() + 8 + 1 + (rec.expire_ms > 0 ? 8 : 0) + rec.value.size() + 24);
    if (rec.expire_ms > 0) bytes += static_cast<int64_t>(1 + 8 + a[2].size() + 8);
    resp::AppendInteger(out, bytes);
  }

  static void Checkpoints(Server& s, Connection&, Args& a, std::string* out) {
    namespace fs = std::filesystem;
    fs::path base = CheckpointBase(s);
    std::error_code ec;
    if (a.size() > 1) {
      if (Upper(a[1]) != "DELETE" || a.size() != 3) {
        resp::AppendError(out, "ERR use ARCA.CHECKPOINTS ou ARCA.CHECKPOINTS DELETE nome");
        return;
      }
      const std::string& name = a[2];
      bool valid = !name.empty() && name != "." && name != "..";
      for (char ch : name) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.')) valid = false;
      }
      if (!valid) {
        resp::AppendError(out, "ERR nome de checkpoint inválido");
        return;
      }
      fs::path target = base / name;
      if (!fs::is_directory(target, ec) || !fs::exists(target / "CURRENT", ec)) {
        resp::AppendError(out, "ERR checkpoint não encontrado");
        return;
      }
      fs::remove_all(target, ec);
      if (ec) {
        resp::AppendError(out, "ERR não foi possível apagar o checkpoint: " + ec.message());
        return;
      }
      resp::AppendSimple(out, "OK");
      return;
    }
    struct Item {
      std::string name;
      int64_t created_ms = 0;
      uint64_t bytes = 0;
      uint64_t files = 0;
    };
    std::vector<Item> items;
    if (fs::is_directory(base, ec)) {
      for (const auto& entry : fs::directory_iterator(base, ec)) {
        std::error_code e2;
        if (!entry.is_directory(e2)) continue;
        Item item;
        item.name = entry.path().filename().string();
        auto ftime = fs::last_write_time(entry.path(), e2);
        if (!e2) {
          auto sys = std::chrono::time_point_cast<std::chrono::milliseconds>(
              ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
          item.created_ms = sys.time_since_epoch().count();
        }
        for (const auto& f : fs::recursive_directory_iterator(entry.path(), e2)) {
          std::error_code e3;
          if (!f.is_regular_file(e3)) continue;
          ++item.files;
          item.bytes += static_cast<uint64_t>(f.file_size(e3));
        }
        items.push_back(std::move(item));
      }
    }
    std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) {
      return x.created_ms != y.created_ms ? x.created_ms > y.created_ms : x.name < y.name;
    });
    std::string j = "[";
    for (size_t i = 0; i < items.size(); ++i) {
      j += (i ? ",{" : "{");
      j += "\"name\":" + JsonString(items[i].name) + ",\"created_ms\":" + std::to_string(items[i].created_ms) +
           ",\"bytes\":" + std::to_string(items[i].bytes) + ",\"files\":" + std::to_string(items[i].files) + "}";
    }
    j += "]";
    resp::AppendBulk(out, j);
  }

  static void Verify(Server& s, Connection&, Args&, std::string* out) {
    VerifyResult r;
    Status st = s.db_->VerifyChecksums(&r);
    if (!st.ok()) {
      resp::AppendError(out, "ERR " + st.ToString());
      return;
    }
    std::string j = "{\"files\":" + std::to_string(r.files) + ",\"bytes\":" + std::to_string(r.bytes) +
                    ",\"ok\":" + std::to_string(r.ok) + ",\"corrupt\":[";
    for (size_t i = 0; i < r.corrupt.size(); ++i) {
      j += (i ? ",{" : "{");
      j += "\"file\":" + JsonString(r.corrupt[i].first) + ",\"error\":" + JsonString(r.corrupt[i].second) + "}";
    }
    j += "],\"ms\":" + JsonNumber(r.ms) + "}";
    resp::AppendBulk(out, j);
  }

  static void Exec(Server& s, Connection& c, Args& a, std::string* out);
};

namespace {

const std::unordered_map<std::string, Server::CommandSpec>& CommandTable() {
  static const std::unordered_map<std::string, Server::CommandSpec> table = [] {
    std::vector<Server::CommandSpec> specs = {
        {"PING", -1, kNoAuth, Commands::Ping},
        {"ECHO", 2, 0, Commands::Echo},
        {"AUTH", -2, kNoAuth | kNoMulti, Commands::Auth},
        {"HELLO", -1, kNoAuth | kNoMulti, Commands::Hello},
        {"QUIT", -1, kNoAuth, Commands::Quit},
        {"SELECT", 2, 0, Commands::Select},
        {"COMMAND", -1, 0, Commands::Command},
        {"TIME", 1, 0, Commands::Time},
        {"GET", 2, kRead, Commands::Get},
        {"SET", -3, kWrite, Commands::Set},
        {"SETNX", 3, kWrite, Commands::SetNx},
        {"SETEX", 4, kWrite, Commands::SetEx},
        {"PSETEX", 4, kWrite, Commands::SetEx},
        {"GETSET", 3, kWrite, Commands::GetSet},
        {"GETDEL", 2, kWrite, Commands::GetDel},
        {"MGET", -2, kRead, Commands::MGet},
        {"MSET", -3, kWrite, Commands::MSet},
        {"DEL", -2, kWrite, Commands::Del},
        {"UNLINK", -2, kWrite, Commands::Del},
        {"EXISTS", -2, kRead, Commands::Exists},
        {"INCR", 2, kWrite, Commands::IncrBy},
        {"DECR", 2, kWrite, Commands::IncrBy},
        {"INCRBY", 3, kWrite, Commands::IncrBy},
        {"DECRBY", 3, kWrite, Commands::IncrBy},
        {"INCRBYFLOAT", 3, kWrite, Commands::IncrByFloat},
        {"APPEND", 3, kWrite, Commands::Append},
        {"STRLEN", 2, kRead, Commands::StrLen},
        {"EXPIRE", 3, kWrite, Commands::Expire},
        {"PEXPIRE", 3, kWrite, Commands::Expire},
        {"EXPIREAT", 3, kWrite, Commands::Expire},
        {"PEXPIREAT", 3, kWrite, Commands::Expire},
        {"TTL", 2, kRead, Commands::Ttl},
        {"PTTL", 2, kRead, Commands::Ttl},
        {"PERSIST", 2, kWrite, Commands::Persist},
        {"TYPE", 2, kRead, Commands::Type},
        {"RENAME", 3, kWrite, Commands::Rename},
        {"RENAMENX", 3, kWrite, Commands::Rename},
        {"SCAN", -2, kRead, Commands::Scan},
        {"KEYS", 2, kRead | kHeavy, Commands::Keys},
        {"DBSIZE", 1, kRead | kHeavy, Commands::DbSize},
        {"FLUSHDB", -1, kWrite | kHeavy, Commands::FlushDb},
        {"FLUSHALL", -1, kWrite | kHeavy, Commands::FlushDb},
        {"MULTI", 1, kNoMulti, Commands::Multi},
        {"EXEC", 1, 0, Commands::Exec},
        {"DISCARD", 1, 0, Commands::Discard},
        {"INFO", -1, 0, Commands::Info},
        {"CLIENT", -2, kNoMulti, Commands::Client},
        {"CONFIG", -2, kNoMulti, Commands::Config},
        {"SLOWLOG", -2, 0, Commands::Slowlog},
        {"SAVE", 1, kHeavy | kNoMulti, Commands::Save},
        {"BGSAVE", -1, kHeavy | kNoMulti, Commands::Checkpoint},
        {"LASTSAVE", 1, 0, Commands::LastSave},
        {"SHUTDOWN", -1, kNoMulti, Commands::Shutdown},
        {"SQL", -2, kHeavy, Commands::SqlCommand},
        {"SQL.JSON", -2, kHeavy, Commands::SqlCommand},
        {"SQL.TABLES", 1, kHeavy, Commands::SqlTables},
        {"ARCA.STATS", 1, 0, Commands::Stats},
        {"ARCA.SCANJSON", -2, kRead, Commands::ScanJson},
        {"ARCA.FLUSH", 1, kHeavy | kNoMulti, Commands::FlushMem},
        {"ARCA.COMPACT", 1, kHeavy | kNoMulti, Commands::Compact},
        {"ARCA.CHECKPOINT", -1, kHeavy | kNoMulti, Commands::Checkpoint},
        {"ARCA.SNAPSHOT", 2, kNoMulti, Commands::Snapshot},
        {"ARCA.CHECKPOINTS", -1, kHeavy | kNoMulti, Commands::Checkpoints},
        {"ARCA.VERIFY", 1, kHeavy | kNoMulti, Commands::Verify},
        {"GETEX", -2, kWrite, Commands::GetEx},
        {"GETRANGE", 4, kRead, Commands::GetRange},
        {"SUBSTR", 4, kRead, Commands::GetRange},
        {"SETRANGE", 4, kWrite, Commands::SetRange},
        {"COPY", -3, kWrite, Commands::Copy},
        {"TOUCH", -2, kRead, Commands::Touch},
        {"RANDOMKEY", 1, kRead, Commands::RandomKey},
        {"EXPIRETIME", 2, kRead, Commands::ExpireTime},
        {"PEXPIRETIME", 2, kRead, Commands::ExpireTime},
        {"MSETNX", -3, kWrite, Commands::MSetNx},
        {"MEMORY", -2, 0, Commands::Memory},
    };
    std::unordered_map<std::string, Server::CommandSpec> m;
    for (const auto& spec : specs) m.emplace(spec.name, spec);
    return m;
  }();
  return table;
}

const Server::CommandSpec* FindCommand(const std::string& upper) {
  const auto& table = CommandTable();
  auto it = table.find(upper);
  return it == table.end() ? nullptr : &it->second;
}

bool ArityOk(const Server::CommandSpec& spec, size_t argc) {
  return spec.arity >= 0 ? argc == static_cast<size_t>(spec.arity) : argc >= static_cast<size_t>(-spec.arity);
}

}

void Commands::Command(Server&, Connection&, Args& a, std::string* out) {
  const auto& table = CommandTable();
  std::string sub = a.size() > 1 ? Upper(a[1]) : "";
  if (sub == "COUNT") {
    resp::AppendInteger(out, static_cast<int64_t>(table.size()));
    return;
  }
  if (sub == "LIST") {
    std::vector<std::string> names;
    for (const auto& [name, spec] : table) {
      std::string lower = name;
      for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      names.push_back(lower);
    }
    std::sort(names.begin(), names.end());
    resp::AppendArray(out, names.size());
    for (const auto& n : names) resp::AppendBulk(out, n);
    return;
  }
  resp::AppendArray(out, 0);
}

void Server::RunCommand(const CommandSpec& spec, Connection& conn, Args& args, std::string* out) {
  auto start = std::chrono::steady_clock::now();
  {
    std::shared_lock<std::shared_mutex> exec(exec_mu_);
    spec.fn(*this, conn, args, out);
  }
  int64_t micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
  commands_.fetch_add(1, std::memory_order_relaxed);
  int64_t threshold;
  int max_len;
  {
    std::lock_guard<std::mutex> lock(config_mu_);
    threshold = config_.slowlog_log_slower_than;
    max_len = config_.slowlog_max_len;
  }
  if (threshold >= 0 && micros >= threshold && max_len > 0) {
    SlowlogEntry e;
    e.at_s = NowMs() / 1000;
    e.micros = micros;
    for (size_t i = 0; i < args.size() && i < 32; ++i) {
      e.args.push_back(args[i].size() > 128 ? args[i].substr(0, 128) + "... (" + std::to_string(args[i].size()) + " bytes)" : args[i]);
    }
    e.addr = conn.addr;
    {
      std::lock_guard<std::mutex> lock(conn.info_mu);
      e.name = conn.name;
    }
    std::lock_guard<std::mutex> lock(slowlog_mu_);
    e.id = slowlog_next_id_++;
    slowlog_.push_front(std::move(e));
    while (static_cast<int>(slowlog_.size()) > max_len) slowlog_.pop_back();
  }
}

Server::Dispatch Server::Execute(const std::shared_ptr<Connection>& conn, Args& args, std::string* out) {
  std::string name = Upper(args[0]);
  {
    std::lock_guard<std::mutex> lock(conn->info_mu);
    conn->last_cmd = name;
  }
  const CommandSpec* spec = FindCommand(name);
  Session& session = conn->session;
  if (!session.authenticated && (spec == nullptr || !(spec->flags & kNoAuth))) {
    resp::AppendError(out, "NOAUTH autenticação necessária");
    return Dispatch::kDone;
  }
  if (spec == nullptr) {
    if (session.in_multi) session.multi_error = true;
    resp::AppendError(out, "ERR comando desconhecido '" + args[0] + "'");
    return Dispatch::kDone;
  }
  if (!ArityOk(*spec, args.size())) {
    if (session.in_multi) session.multi_error = true;
    resp::AppendError(out, "ERR número de argumentos inválido para '" + Upper(args[0]) + "'");
    return Dispatch::kDone;
  }
  if (session.in_multi && name != "EXEC" && name != "DISCARD" && name != "MULTI" && name != "QUIT") {
    if (spec->flags & kNoMulti) {
      session.multi_error = true;
      resp::AppendError(out, "ERR o comando " + name + " não pode ser usado dentro de MULTI");
      return Dispatch::kDone;
    }
    session.queued.push_back(args);
    resp::AppendSimple(out, "QUEUED");
    return Dispatch::kDone;
  }
  if ((spec->flags & kHeavy) && workers_) {
    auto self = conn;
    auto shared_args = std::make_shared<Args>(std::move(args));
    workers_->Submit([this, self, shared_args, spec] {
      std::string reply;
      RunCommand(*spec, *self, *shared_args, &reply);
      loops_[self->loop]->Complete(self, std::move(reply));
    });
    return Dispatch::kAsync;
  }
  RunCommand(*spec, *conn, args, out);
  return Dispatch::kDone;
}

void Commands::Exec(Server& s, Connection& c, Args&, std::string* out) {
  if (!c.session.in_multi) {
    resp::AppendError(out, "ERR EXEC sem MULTI");
    return;
  }
  c.session.in_multi = false;
  auto queued = std::move(c.session.queued);
  c.session.queued.clear();
  if (c.session.multi_error) {
    c.session.multi_error = false;
    resp::AppendError(out, "EXECABORT transação descartada por erros anteriores");
    return;
  }
  resp::AppendArray(out, queued.size());
  s.exec_mu_.unlock_shared();
  {
    std::unique_lock<std::shared_mutex> exclusive(s.exec_mu_);
    for (auto& args : queued) {
      const Server::CommandSpec* spec = FindCommand(Upper(args[0]));
      spec->fn(s, c, args, out);
      s.commands_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  s.exec_mu_.lock_shared();
}

void Commands::Info(Server& s, Connection&, Args& a, std::string* out) {
  std::string section = a.size() > 1 ? Upper(a[1]) : "ALL";
  auto st = s.db_->GetStats();
  int64_t uptime = (NowMs() - s.start_ms_) / 1000;
  std::string text;
  auto add = [&](const std::string& name, const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string up = Upper(name);
    if (section != "ALL" && section != "EVERYTHING" && section != "DEFAULT" && section != up) return;
    text += "# " + name + "\r\n";
    for (const auto& [k, v] : fields) text += k + ":" + v + "\r\n";
    text += "\r\n";
  };
  add("Server", {{"arca_version", kVersion},
                 {"redis_version", "7.0.0"},
                 {"tcp_port", std::to_string(s.config_.port)},
                 {"uptime_in_seconds", std::to_string(uptime)},
                 {"uptime_in_days", std::to_string(uptime / 86400)},
                 {"io_threads", std::to_string(s.config_.io_threads)},
                 {"worker_threads", std::to_string(s.config_.worker_threads)}});
  add("Clients", {{"connected_clients", std::to_string(s.connected_.load())},
                  {"maxclients", std::to_string(s.config_.maxclients)}});
  add("Memory", {{"used_memory", std::to_string(st.memtable_bytes + st.immutable_bytes + st.block_cache_bytes)},
                 {"memtable_bytes", std::to_string(st.memtable_bytes)},
                 {"immutable_memtable_bytes", std::to_string(st.immutable_bytes)},
                 {"block_cache_bytes", std::to_string(st.block_cache_bytes)},
                 {"block_cache_capacity", std::to_string(st.block_cache_capacity)}});
  add("Persistence", {{"appendfsync", s.db_->GetSyncMode() == SyncMode::kAlways ? "always"
                                      : s.db_->GetSyncMode() == SyncMode::kNever ? "no"
                                                                                  : "everysec"},
                      {"last_save_time", std::to_string(s.last_save_s_.load())},
                      {"wal_number", std::to_string(st.wal_number)},
                      {"wal_bytes", std::to_string(st.wal_bytes)},
                      {"flushes", std::to_string(st.flushes)}});
  add("Stats", {{"total_connections_received", std::to_string(s.total_connections_.load())},
                {"total_commands_processed", std::to_string(s.commands_.load())},
                {"instantaneous_ops_per_sec", std::to_string(static_cast<int64_t>(s.ops_per_sec_.load()))},
                {"total_net_input_bytes", std::to_string(s.net_in_.load())},
                {"total_net_output_bytes", std::to_string(s.net_out_.load())},
                {"rejected_connections", std::to_string(s.rejected_.load())},
                {"expired_keys", std::to_string(s.expired_.load())},
                {"keyspace_hits", std::to_string(s.hits_.load())},
                {"keyspace_misses", std::to_string(s.misses_.load())}});
  add("Storage", {{"last_sequence", std::to_string(st.last_sequence)},
                  {"sst_files", std::to_string(st.sst_files)},
                  {"sst_bytes", std::to_string(st.sst_bytes)},
                  {"level0_files", std::to_string(st.levels.empty() ? 0 : st.levels[0].files.size())},
                  {"compactions", std::to_string(st.compactions)},
                  {"compaction_bytes_read", std::to_string(st.compaction_bytes_read)},
                  {"compaction_bytes_written", std::to_string(st.compaction_bytes_written)},
                  {"write_amplification", JsonNumber(st.write_amplification)},
                  {"write_stalls", std::to_string(st.write_stalls)},
                  {"bloom_useful", std::to_string(st.bloom_useful)},
                  {"block_cache_hits", std::to_string(st.block_cache_hits)},
                  {"block_cache_misses", std::to_string(st.block_cache_misses)}});
  add("Keyspace", {{"db0", "keys=" + std::to_string(st.estimated_keys) + ",expires=0,tables=" +
                               std::to_string(s.sql_->TableCount())}});
  resp::AppendBulk(out, text);
}

void Commands::Stats(Server& s, Connection&, Args&, std::string* out) {
  auto st = s.db_->GetStats();
  auto names = s.sql_->TableNames();
  int64_t uptime = (NowMs() - s.start_ms_) / 1000;
  const char* fsync = s.db_->GetSyncMode() == SyncMode::kAlways ? "always"
                      : s.db_->GetSyncMode() == SyncMode::kNever ? "no"
                                                                  : "everysec";
  std::string j = "{\"server\":{\"version\":\"" + std::string(kVersion) + "\",\"port\":" + std::to_string(s.config_.port) +
                  ",\"uptime_s\":" + std::to_string(uptime) + ",\"io_threads\":" + std::to_string(s.config_.io_threads) +
                  ",\"worker_threads\":" + std::to_string(s.config_.worker_threads) + ",\"appendfsync\":\"" + fsync +
                  "\",\"dir\":" + JsonString(s.config_.dir) + "},";
  j += "\"clients\":{\"connected\":" + std::to_string(s.connected_.load()) +
       ",\"total\":" + std::to_string(s.total_connections_.load()) +
       ",\"rejected\":" + std::to_string(s.rejected_.load()) + "},";
  j += "\"commands\":{\"total\":" + std::to_string(s.commands_.load()) +
       ",\"ops_per_sec\":" + JsonNumber(s.ops_per_sec_.load()) + ",\"reads\":" + std::to_string(s.reads_.load()) +
       ",\"writes\":" + std::to_string(s.writes_.load()) + ",\"keyspace_hits\":" + std::to_string(s.hits_.load()) +
       ",\"keyspace_misses\":" + std::to_string(s.misses_.load()) +
       ",\"expired_keys\":" + std::to_string(s.expired_.load()) +
       ",\"net_input_bytes\":" + std::to_string(s.net_in_.load()) +
       ",\"net_output_bytes\":" + std::to_string(s.net_out_.load()) + "},";
  j += "\"engine\":{\"last_sequence\":" + std::to_string(st.last_sequence) +
       ",\"memtable_bytes\":" + std::to_string(st.memtable_bytes) +
       ",\"memtable_entries\":" + std::to_string(st.memtable_entries) +
       ",\"immutable_bytes\":" + std::to_string(st.immutable_bytes) +
       ",\"wal_number\":" + std::to_string(st.wal_number) + ",\"wal_bytes\":" + std::to_string(st.wal_bytes) +
       ",\"sst_files\":" + std::to_string(st.sst_files) + ",\"sst_bytes\":" + std::to_string(st.sst_bytes) +
       ",\"flushes\":" + std::to_string(st.flushes) + ",\"compactions\":" + std::to_string(st.compactions) +
       ",\"compaction_bytes_read\":" + std::to_string(st.compaction_bytes_read) +
       ",\"compaction_bytes_written\":" + std::to_string(st.compaction_bytes_written) +
       ",\"user_bytes_written\":" + std::to_string(st.user_bytes_written) +
       ",\"write_amplification\":" + JsonNumber(st.write_amplification) +
       ",\"write_stalls\":" + std::to_string(st.write_stalls) + ",\"bloom_checks\":" + std::to_string(st.bloom_checks) +
       ",\"bloom_useful\":" + std::to_string(st.bloom_useful) +
       ",\"block_cache_bytes\":" + std::to_string(st.block_cache_bytes) +
       ",\"block_cache_capacity\":" + std::to_string(st.block_cache_capacity) +
       ",\"block_cache_hits\":" + std::to_string(st.block_cache_hits) +
       ",\"block_cache_misses\":" + std::to_string(st.block_cache_misses) +
       ",\"compacting\":" + (st.compacting ? "true" : "false") + ",\"snapshots\":" + std::to_string(st.snapshots) +
       ",\"estimated_keys\":" + std::to_string(st.estimated_keys) +
       ",\"background_error\":" + JsonString(st.background_error) + "},";
  j += "\"levels\":[";
  for (size_t i = 0; i < st.levels.size(); ++i) {
    const auto& l = st.levels[i];
    j += (i ? ",{" : "{");
    j += "\"level\":" + std::to_string(l.level) + ",\"files\":" + std::to_string(l.files.size()) +
         ",\"bytes\":" + std::to_string(l.bytes) + ",\"max_bytes\":" + std::to_string(l.max_bytes) +
         ",\"score\":" + JsonNumber(l.score) + ",\"tables\":[";
    for (size_t k = 0; k < l.files.size(); ++k) {
      const auto& f = l.files[k];
      j += (k ? ",{" : "{");
      j += "\"number\":" + std::to_string(f.number) + ",\"bytes\":" + std::to_string(f.bytes) +
           ",\"smallest\":" + JsonString(sql::Engine::DescribeKey(f.smallest, names)) +
           ",\"largest\":" + JsonString(sql::Engine::DescribeKey(f.largest, names)) +
           ",\"entries\":" + std::to_string(f.entries) + ",\"deletions\":" + std::to_string(f.deletions) + "}";
    }
    j += "]}";
  }
  j += "],\"recent_compactions\":[";
  for (size_t i = 0; i < st.recent_compactions.size(); ++i) {
    const auto& c = st.recent_compactions[i];
    j += (i ? ",{" : "{");
    j += "\"from_level\":" + std::to_string(c.from_level) + ",\"to_level\":" + std::to_string(c.to_level) +
         ",\"inputs\":" + std::to_string(c.inputs) + ",\"outputs\":" + std::to_string(c.outputs) +
         ",\"bytes_read\":" + std::to_string(c.bytes_read) + ",\"bytes_written\":" + std::to_string(c.bytes_written) +
         ",\"ms\":" + JsonNumber(c.ms) + ",\"at\":" + std::to_string(c.at_ms) + ",\"reason\":" + JsonString(c.reason) +
         "}";
  }
  j += "],\"keyspace\":{\"tables\":" + std::to_string(s.sql_->TableCount()) + "}}";
  resp::AppendBulk(out, j);
}

void Commands::Client(Server& s, Connection& c, Args& a, std::string* out) {
  std::string sub = Upper(a[1]);
  if (sub == "ID") {
    resp::AppendInteger(out, static_cast<int64_t>(c.id));
  } else if (sub == "SETNAME" && a.size() == 3) {
    std::lock_guard<std::mutex> lock(c.info_mu);
    c.name = a[2];
    resp::AppendSimple(out, "OK");
  } else if (sub == "GETNAME") {
    std::lock_guard<std::mutex> lock(c.info_mu);
    c.name.empty() ? resp::AppendNull(out) : resp::AppendBulk(out, c.name);
  } else if (sub == "LIST") {
    std::vector<std::shared_ptr<Connection>> conns;
    {
      std::lock_guard<std::mutex> lock(s.registry_mu_);
      for (const auto& [id, w] : s.registry_) {
        if (auto p = w.lock()) conns.push_back(p);
      }
    }
    int64_t now = NowMs();
    std::string text;
    for (const auto& p : conns) {
      std::string name;
      std::string cmd;
      {
        std::lock_guard<std::mutex> lock(p->info_mu);
        name = p->name;
        cmd = p->last_cmd;
      }
      for (auto& ch : cmd) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      text += "id=" + std::to_string(p->id) + " addr=" + p->addr + " name=" + name +
              " age=" + std::to_string((now - p->created_ms) / 1000) +
              " idle=" + std::to_string((now - p->last_ms.load()) / 1000) + " db=0 cmd=" + cmd + "\n";
    }
    resp::AppendBulk(out, text);
  } else if (sub == "SETINFO") {
    resp::AppendSimple(out, "OK");
  } else {
    resp::AppendError(out, "ERR subcomando de CLIENT desconhecido");
  }
}

void Commands::Config(Server& s, Connection&, Args& a, std::string* out) {
  std::string sub = Upper(a[1]);
  if (sub == "GET" && a.size() >= 3) {
    std::vector<std::pair<std::string, std::string>> all;
    {
      std::lock_guard<std::mutex> lock(s.config_mu_);
      all = s.config_.All();
    }
    std::vector<std::pair<std::string, std::string>> matched;
    for (const auto& [k, v] : all) {
      for (size_t i = 2; i < a.size(); ++i) {
        if (GlobMatch(a[i], k)) {
          matched.emplace_back(k, v);
          break;
        }
      }
    }
    resp::AppendArray(out, matched.size() * 2);
    for (const auto& [k, v] : matched) {
      resp::AppendBulk(out, k);
      resp::AppendBulk(out, v);
    }
  } else if (sub == "SET" && a.size() == 4) {
    std::string name = a[2];
    for (auto& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    static const std::vector<std::string> runtime = {"appendfsync", "requirepass", "slowlog-log-slower-than",
                                                     "slowlog-max-len", "maxclients", "timeout", "loglevel"};
    if (std::find(runtime.begin(), runtime.end(), name) == runtime.end()) {
      resp::AppendError(out, "ERR o parâmetro " + name + " só pode ser alterado no arquivo de configuração");
      return;
    }
    Status st;
    {
      std::lock_guard<std::mutex> lock(s.config_mu_);
      st = s.config_.Set(name, a[3]);
    }
    if (!st.ok()) {
      resp::AppendError(out, "ERR " + st.message());
      return;
    }
    if (name == "appendfsync") {
      s.db_->SetSyncMode(a[3] == "always" ? SyncMode::kAlways : a[3] == "no" ? SyncMode::kNever : SyncMode::kEverySecond);
    }
    resp::AppendSimple(out, "OK");
  } else if (sub == "RESETSTAT") {
    s.hits_.store(0);
    s.misses_.store(0);
    resp::AppendSimple(out, "OK");
  } else {
    resp::AppendError(out, "ERR use CONFIG GET padrão ou CONFIG SET nome valor");
  }
}

void Commands::Slowlog(Server& s, Connection&, Args& a, std::string* out) {
  std::string sub = Upper(a[1]);
  std::lock_guard<std::mutex> lock(s.slowlog_mu_);
  if (sub == "GET") {
    int64_t n = 10;
    if (a.size() > 2 && !ParseInt64(a[2], &n)) n = 10;
    size_t count = n < 0 ? s.slowlog_.size() : std::min<size_t>(s.slowlog_.size(), static_cast<size_t>(n));
    resp::AppendArray(out, count);
    for (size_t i = 0; i < count; ++i) {
      const auto& e = s.slowlog_[i];
      resp::AppendArray(out, 6);
      resp::AppendInteger(out, e.id);
      resp::AppendInteger(out, e.at_s);
      resp::AppendInteger(out, e.micros);
      resp::AppendArray(out, e.args.size());
      for (const auto& arg : e.args) resp::AppendBulk(out, arg);
      resp::AppendBulk(out, e.addr);
      resp::AppendBulk(out, e.name);
    }
  } else if (sub == "LEN") {
    resp::AppendInteger(out, static_cast<int64_t>(s.slowlog_.size()));
  } else if (sub == "RESET") {
    s.slowlog_.clear();
    resp::AppendSimple(out, "OK");
  } else {
    resp::AppendError(out, "ERR use SLOWLOG GET, LEN ou RESET");
  }
}

}

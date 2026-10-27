#include <chrono>
#include <thread>

#include "net/resp.hpp"
#include "server/server.hpp"
#include "test.hpp"

using namespace arca;

namespace {

struct Running {
  std::string dir = test::TempDir("servidor");
  int port;
  std::unique_ptr<server::Server> srv;

  explicit Running(const std::string& password = "") {
    server::Config c;
    c.dir = dir;
    port = 20000 + static_cast<int>(std::chrono::steady_clock::now().time_since_epoch().count() % 20000);
    c.port = port;
    c.requirepass = password;
    c.appendfsync = "no";
    c.loglevel = "warning";
    srv = std::make_unique<server::Server>(c);
    Status s = srv->Start();
    if (!s.ok()) throw test::Failure{s.ToString()};
  }

  ~Running() {
    srv.reset();
    DestroyDB(dir);
  }

  resp::Value Call(resp::Client& c, std::vector<std::string> args) {
    resp::Value v;
    Status s = c.Call(args, &v);
    if (!s.ok()) throw test::Failure{s.ToString()};
    return v;
  }
};

}

TEST(ServidorComandosBasicos) {
  Running r;
  resp::Client c;
  CHECK_OK(c.Connect("127.0.0.1", r.port));
  CHECK_EQ(r.Call(c, {"PING"}).str, std::string("PONG"));
  CHECK_EQ(r.Call(c, {"SET", "k", "v"}).str, std::string("OK"));
  CHECK_EQ(r.Call(c, {"GET", "k"}).str, std::string("v"));
  CHECK_EQ(r.Call(c, {"INCRBY", "n", "5"}).integer, int64_t(5));
  CHECK(r.Call(c, {"SET", "k", "w", "NX"}).type == resp::Value::Type::kNull);
  CHECK_EQ(r.Call(c, {"DEL", "k", "inexistente"}).integer, int64_t(1));
  CHECK(r.Call(c, {"GET", "k"}).type == resp::Value::Type::kNull);
  CHECK(r.Call(c, {"NAOEXISTE"}).type == resp::Value::Type::kError);
}

TEST(ServidorExpiracaoETransacao) {
  Running r;
  resp::Client c;
  CHECK_OK(c.Connect("127.0.0.1", r.port));
  r.Call(c, {"SET", "temp", "1", "PX", "150"});
  CHECK(r.Call(c, {"PTTL", "temp"}).integer > 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  CHECK(r.Call(c, {"GET", "temp"}).type == resp::Value::Type::kNull);
  r.Call(c, {"MULTI"});
  CHECK_EQ(r.Call(c, {"INCR", "x"}).str, std::string("QUEUED"));
  r.Call(c, {"INCR", "x"});
  auto exec = r.Call(c, {"EXEC"});
  CHECK_EQ(exec.elements.size(), size_t(2));
  CHECK_EQ(exec.elements[1].integer, int64_t(2));
}

TEST(ServidorSqlEPipeline) {
  Running r;
  resp::Client c;
  CHECK_OK(c.Connect("127.0.0.1", r.port));
  r.Call(c, {"SQL", "CREATE TABLE t (id INT PRIMARY KEY, v TEXT)"});
  r.Call(c, {"SQL", "INSERT INTO t VALUES (1, 'um'), (2, 'dois')"});
  auto json = r.Call(c, {"SQL.JSON", "SELECT * FROM t ORDER BY id DESC"});
  CHECK(json.str.find("\"rows\":[[2,\"dois\"],[1,\"um\"]]") != std::string::npos);
  std::string payload;
  for (int i = 0; i < 200; ++i) payload += resp::EncodeCommand({"SET", "p" + std::to_string(i), std::to_string(i)});
  CHECK_OK(c.SendRaw(payload));
  for (int i = 0; i < 200; ++i) {
    resp::Value v;
    CHECK_OK(c.Read(&v));
    CHECK_EQ(v.str, std::string("OK"));
  }
  CHECK_EQ(r.Call(c, {"GET", "p199"}).str, std::string("199"));
}

TEST(ServidorExigeSenha) {
  Running r("segredo");
  resp::Client c;
  CHECK_OK(c.Connect("127.0.0.1", r.port));
  CHECK(r.Call(c, {"GET", "x"}).type == resp::Value::Type::kError);
  CHECK(r.Call(c, {"AUTH", "errada"}).type == resp::Value::Type::kError);
  CHECK_EQ(r.Call(c, {"AUTH", "segredo"}).str, std::string("OK"));
  CHECK(r.Call(c, {"GET", "x"}).type == resp::Value::Type::kNull);
}

TEST(ServidorComandosDeStrings120) {
  Running r;
  resp::Client c;
  CHECK_OK(c.Connect("127.0.0.1", r.port));
  using T = resp::Value::Type;
  r.Call(c, {"SET", "s", "Hello World"});
  CHECK_EQ(r.Call(c, {"GETRANGE", "s", "0", "4"}).str, std::string("Hello"));
  CHECK_EQ(r.Call(c, {"GETRANGE", "s", "-5", "-1"}).str, std::string("World"));
  CHECK_EQ(r.Call(c, {"GETRANGE", "s", "5", "1"}).str, std::string(""));
  CHECK_EQ(r.Call(c, {"GETRANGE", "nada", "0", "-1"}).str, std::string(""));
  CHECK(r.Call(c, {"GETRANGE", "s", "a", "1"}).type == T::kError);
  CHECK_EQ(r.Call(c, {"SETRANGE", "s", "6", "Redis"}).integer, int64_t(11));
  CHECK_EQ(r.Call(c, {"GET", "s"}).str, std::string("Hello Redis"));
  CHECK_EQ(r.Call(c, {"SETRANGE", "pad", "3", "x"}).integer, int64_t(4));
  CHECK_EQ(r.Call(c, {"GET", "pad"}).str, std::string("\0\0\0x", 4));
  CHECK_EQ(r.Call(c, {"SETRANGE", "vazia", "0", ""}).integer, int64_t(0));
  CHECK_EQ(r.Call(c, {"EXISTS", "vazia"}).integer, int64_t(0));
  CHECK(r.Call(c, {"SETRANGE", "s", "-1", "x"}).type == T::kError);

  CHECK_EQ(r.Call(c, {"MSETNX", "m1", "a", "m2", "b"}).integer, int64_t(1));
  CHECK_EQ(r.Call(c, {"MSETNX", "m2", "z", "m3", "c"}).integer, int64_t(0));
  CHECK(r.Call(c, {"GET", "m3"}).type == T::kNull);
  CHECK_EQ(r.Call(c, {"GET", "m2"}).str, std::string("b"));
  CHECK(r.Call(c, {"MSETNX", "m4", "a", "m5"}).type == T::kError);

  CHECK_EQ(r.Call(c, {"TOUCH", "m1", "m2", "nada"}).integer, int64_t(2));
  CHECK(r.Call(c, {"MEMORY", "USAGE", "m1"}).integer > 0);
  CHECK(r.Call(c, {"MEMORY", "USAGE", "nada"}).type == T::kNull);
  CHECK(r.Call(c, {"MEMORY", "DOCTOR"}).type == T::kError);
  CHECK(r.Call(c, {"COMMAND", "COUNT"}).integer > 60);
}

TEST(ServidorExpiracaoECopia120) {
  Running r;
  resp::Client c;
  CHECK_OK(c.Connect("127.0.0.1", r.port));
  using T = resp::Value::Type;
  r.Call(c, {"SET", "k", "v"});
  CHECK_EQ(r.Call(c, {"EXPIRETIME", "k"}).integer, int64_t(-1));
  CHECK_EQ(r.Call(c, {"EXPIRETIME", "nada"}).integer, int64_t(-2));
  CHECK_EQ(r.Call(c, {"GETEX", "k", "EX", "100"}).str, std::string("v"));
  int64_t pt = r.Call(c, {"PEXPIRETIME", "k"}).integer;
  int64_t et = r.Call(c, {"EXPIRETIME", "k"}).integer;
  CHECK(pt > 0);
  CHECK_EQ(et, pt / 1000);
  CHECK(r.Call(c, {"TTL", "k"}).integer > 90);
  CHECK_EQ(r.Call(c, {"GETEX", "k", "PERSIST"}).str, std::string("v"));
  CHECK_EQ(r.Call(c, {"TTL", "k"}).integer, int64_t(-1));
  CHECK_EQ(r.Call(c, {"GETEX", "k"}).str, std::string("v"));
  CHECK(r.Call(c, {"GETEX", "nada", "EX", "5"}).type == T::kNull);
  CHECK(r.Call(c, {"GETEX", "k", "EX", "0"}).type == T::kError);
  CHECK(r.Call(c, {"GETEX", "k", "EX", "5", "PERSIST"}).type == T::kError);
  CHECK(r.Call(c, {"GETEX", "k", "XYZ"}).type == T::kError);
  CHECK_EQ(r.Call(c, {"GETEX", "k", "PXAT", "1"}).str, std::string("v"));
  CHECK(r.Call(c, {"GET", "k"}).type == T::kNull);

  r.Call(c, {"SET", "orig", "1", "EX", "500"});
  r.Call(c, {"SET", "dest", "2"});
  CHECK_EQ(r.Call(c, {"COPY", "orig", "dest"}).integer, int64_t(0));
  CHECK_EQ(r.Call(c, {"GET", "dest"}).str, std::string("2"));
  CHECK_EQ(r.Call(c, {"COPY", "orig", "dest", "REPLACE"}).integer, int64_t(1));
  CHECK_EQ(r.Call(c, {"GET", "dest"}).str, std::string("1"));
  CHECK(r.Call(c, {"TTL", "dest"}).integer > 400);
  CHECK_EQ(r.Call(c, {"COPY", "orig", "novo"}).integer, int64_t(1));
  CHECK_EQ(r.Call(c, {"COPY", "nada", "outro"}).integer, int64_t(0));
  CHECK(r.Call(c, {"COPY", "orig", "orig"}).type == T::kError);
  CHECK(r.Call(c, {"COPY", "orig", "x", "DB", "1"}).type == T::kError);
  CHECK(r.Call(c, {"COPY", "orig", "x", "OUTRA"}).type == T::kError);

  auto rk = r.Call(c, {"RANDOMKEY"});
  CHECK(rk.type == T::kBulk);
  CHECK(r.Call(c, {"EXISTS", rk.str}).integer == 1);
  r.Call(c, {"FLUSHDB"});
  CHECK(r.Call(c, {"RANDOMKEY"}).type == T::kNull);
}

TEST(ServidorCheckpointsEVerificacao) {
  Running r;
  resp::Client c;
  CHECK_OK(c.Connect("127.0.0.1", r.port));
  using T = resp::Value::Type;
  for (int i = 0; i < 500; ++i) r.Call(c, {"SET", "v" + std::to_string(i), std::string(64, 'x')});
  CHECK_EQ(r.Call(c, {"ARCA.FLUSH"}).str, std::string("OK"));
  auto verify = r.Call(c, {"ARCA.VERIFY"});
  CHECK(verify.type == T::kBulk);
  CHECK(verify.str.find("\"corrupt\":[]") != std::string::npos);
  CHECK(verify.str.find("\"files\":0") == std::string::npos);

  std::string name = "teste-" + std::to_string(r.port) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
  CHECK(r.Call(c, {"ARCA.CHECKPOINT", name}).type == T::kBulk);
  auto list = r.Call(c, {"ARCA.CHECKPOINTS"});
  CHECK(list.type == T::kBulk);
  CHECK(list.str.find("\"name\":\"" + name + "\"") != std::string::npos);
  CHECK(list.str.find("\"files\":") != std::string::npos);
  CHECK(r.Call(c, {"ARCA.CHECKPOINTS", "DELETE", "../dados"}).type == T::kError);
  CHECK(r.Call(c, {"ARCA.CHECKPOINTS", "DELETE", ".."}).type == T::kError);
  CHECK(r.Call(c, {"ARCA.CHECKPOINTS", "DELETE", "a/b"}).type == T::kError);
  CHECK(r.Call(c, {"ARCA.CHECKPOINTS", "DELETE", "nao-existe-" + name}).type == T::kError);
  CHECK(r.Call(c, {"ARCA.CHECKPOINTS", "OUTRO", name}).type == T::kError);
  CHECK_EQ(r.Call(c, {"ARCA.CHECKPOINTS", "DELETE", name}).str, std::string("OK"));
  CHECK(r.Call(c, {"ARCA.CHECKPOINTS"}).str.find("\"name\":\"" + name + "\"") == std::string::npos);
  CHECK(r.Call(c, {"MULTI"}).str == "OK");
  CHECK(r.Call(c, {"ARCA.VERIFY"}).type == T::kError);
  r.Call(c, {"DISCARD"});
}

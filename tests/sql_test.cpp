#include "arca/db.hpp"
#include "sql/sql.hpp"
#include "test.hpp"

using namespace arca;

namespace {

struct Fixture {
  std::string dir = test::TempDir("sql");
  std::unique_ptr<DB> db;
  std::unique_ptr<sql::Engine> engine;

  Fixture() {
    Options o;
    o.sync_mode = SyncMode::kNever;
    Status s = DB::Open(o, dir, &db);
    if (!s.ok()) throw test::Failure{s.ToString()};
    engine = std::make_unique<sql::Engine>(db.get());
    s = engine->Load();
    if (!s.ok()) throw test::Failure{s.ToString()};
  }

  ~Fixture() {
    engine.reset();
    db.reset();
    DestroyDB(dir);
  }

  sql::Result Run(const std::string& q) {
    sql::Result r;
    Status s = engine->Execute(q, &r);
    if (!s.ok()) throw test::Failure{q + " -> " + s.ToString()};
    return r;
  }

  Status Try(const std::string& q) {
    sql::Result r;
    return engine->Execute(q, &r);
  }
};

}

TEST(SqlCriaInsereEConsulta) {
  Fixture f;
  f.Run("CREATE TABLE pedidos (id INT PRIMARY KEY AUTOINCREMENT, cliente TEXT NOT NULL, total REAL, status TEXT DEFAULT 'novo')");
  f.Run("INSERT INTO pedidos (cliente, total) VALUES ('ana', 10), ('bia', 25.5), ('ana', 4.5)");
  auto r = f.Run("SELECT cliente, COUNT(*) AS n, SUM(total) AS soma FROM pedidos GROUP BY cliente ORDER BY soma DESC");
  CHECK_EQ(r.rows.size(), size_t(2));
  CHECK_EQ(r.rows[0][0].s, std::string("bia"));
  CHECK_EQ(r.rows[1][1].i, int64_t(2));
  auto one = f.Run("SELECT * FROM pedidos WHERE id = 2");
  CHECK_EQ(one.rows.size(), size_t(1));
  CHECK_EQ(one.rows[0][3].s, std::string("novo"));
  CHECK(one.plan.find("chave primária") != std::string::npos);
}

TEST(SqlIndiceAtualizaEApaga) {
  Fixture f;
  f.Run("CREATE TABLE c (id INT PRIMARY KEY, cidade TEXT)");
  f.Run("INSERT INTO c VALUES (1, 'Rio'), (2, 'Niterói'), (3, 'Rio')");
  f.Run("CREATE INDEX idx_cidade ON c (cidade)");
  auto r = f.Run("SELECT id FROM c WHERE cidade = 'Rio' ORDER BY id");
  CHECK_EQ(r.rows.size(), size_t(2));
  CHECK(r.plan.find("idx_cidade") != std::string::npos);
  CHECK_EQ(f.Run("UPDATE c SET cidade = 'Niterói' WHERE id = 1").affected, int64_t(1));
  CHECK_EQ(f.Run("SELECT id FROM c WHERE cidade = 'Rio'").rows.size(), size_t(1));
  CHECK_EQ(f.Run("DELETE FROM c WHERE cidade = 'Niterói'").affected, int64_t(2));
  CHECK_EQ(f.Run("SELECT COUNT(*) FROM c").rows[0][0].i, int64_t(1));
}

TEST(SqlRejeitaErros) {
  Fixture f;
  f.Run("CREATE TABLE t (id INT PRIMARY KEY, nome TEXT NOT NULL)");
  CHECK(!f.Try("INSERT INTO t VALUES (1, NULL)").ok());
  f.Run("INSERT INTO t VALUES (1, 'a')");
  CHECK(!f.Try("INSERT INTO t VALUES (1, 'b')").ok());
  CHECK(!f.Try("SELECT inexistente FROM t").ok());
  CHECK(!f.Try("SELEC * FROM t").ok());
  CHECK(f.Try("CREATE TABLE IF NOT EXISTS t (id INT PRIMARY KEY)").ok());
}

TEST(SqlPersisteDepoisDeReabrir) {
  std::string dir = test::TempDir("sql-reabrir");
  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::Open(Options(), dir, &db));
    sql::Engine e(db.get());
    CHECK_OK(e.Load());
    sql::Result r;
    CHECK_OK(e.Execute("CREATE TABLE t (id INT PRIMARY KEY AUTOINCREMENT, v TEXT)", &r));
    CHECK_OK(e.Execute("INSERT INTO t (v) VALUES ('x'), ('y')", &r));
  }
  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::Open(Options(), dir, &db));
    sql::Engine e(db.get());
    CHECK_OK(e.Load());
    sql::Result r;
    CHECK_OK(e.Execute("INSERT INTO t (v) VALUES ('z')", &r));
    sql::Result q;
    CHECK_OK(e.Execute("SELECT MAX(id) FROM t", &q));
    CHECK_EQ(q.rows[0][0].i, int64_t(3));
  }
  DestroyDB(dir);
}

TEST(SqlFuncoesDeTexto130) {
  std::string dir = test::TempDir("sql-funcoes");
  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::Open(Options(), dir, &db));
    sql::Engine e(db.get());
    CHECK_OK(e.Load());
    sql::Result r;
    CHECK_OK(e.Execute("CREATE TABLE p (id INT PRIMARY KEY, nome TEXT, preco REAL, obs TEXT)", &r));
    CHECK_OK(e.Execute("INSERT INTO p VALUES (1, '  Caderno  ', 12.7, NULL), (2, 'Lápis-preto', 2.2, 'promo')", &r));
    sql::Result q;
    CHECK_OK(e.Execute("SELECT TRIM(nome), LTRIM(nome), RTRIM(nome), FLOOR(preco), CEIL(preco), IFNULL(obs, 'sem') FROM p WHERE id = 1", &q));
    CHECK_EQ(q.rows[0][0].s, std::string("Caderno"));
    CHECK_EQ(q.rows[0][1].s, std::string("Caderno  "));
    CHECK_EQ(q.rows[0][2].s, std::string("  Caderno"));
    CHECK_EQ(q.rows[0][3].i, int64_t(12));
    CHECK_EQ(q.rows[0][4].i, int64_t(13));
    CHECK_EQ(q.rows[0][5].s, std::string("sem"));
    sql::Result t;
    CHECK_OK(e.Execute("SELECT SUBSTR(nome, 1, 5), SUBSTR(nome, -5), REPLACE(nome, '-', ' '), CONCAT(nome, '/', obs), INSTR(nome, 'preto'), NULLIF(obs, 'promo'), TYPEOF(preco) FROM p WHERE id = 2", &t));
    CHECK_EQ(t.rows[0][0].s, std::string("Lápi"));
    CHECK_EQ(t.rows[0][1].s, std::string("preto"));
    CHECK_EQ(t.rows[0][2].s, std::string("Lápis preto"));
    CHECK_EQ(t.rows[0][3].s, std::string("Lápis-preto/promo"));
    CHECK_EQ(t.rows[0][4].i, int64_t(8));
    CHECK(t.rows[0][5].is_null());
    CHECK_EQ(t.rows[0][6].s, std::string("real"));
    sql::Result w;
    CHECK_OK(e.Execute("SELECT id FROM p WHERE TRIM(nome) = 'Caderno'", &w));
    CHECK_EQ(w.rows.size(), size_t(1));
    sql::Result u;
    CHECK_OK(e.Execute("UPDATE p SET nome = REPLACE(TRIM(nome), 'Caderno', 'Agenda') WHERE INSTR(nome, 'Cad') > 0", &u));
    sql::Result after;
    CHECK_OK(e.Execute("SELECT nome FROM p WHERE id = 1", &after));
    CHECK_EQ(after.rows[0][0].s, std::string("Agenda"));
    sql::Result bad;
    CHECK(!e.Execute("SELECT FOO(nome) FROM p", &bad).ok());
  }
  DestroyDB(dir);
}

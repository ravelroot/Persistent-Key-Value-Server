#include <atomic>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <thread>

#include "arca/db.hpp"
#include "db/dbformat.hpp"
#include "db/skiplist.hpp"
#include "table/bloom.hpp"
#include "test.hpp"
#include "util/coding.hpp"
#include "util/hash.hpp"

using namespace arca;

namespace {

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "chave%08d", i);
  return buf;
}

Options SmallOptions() {
  Options o;
  o.write_buffer_size = 64 * 1024;
  o.max_file_size = 32 * 1024;
  o.level1_max_bytes = 128 * 1024;
  o.block_cache_capacity = 1 << 20;
  o.sync_mode = SyncMode::kNever;
  return o;
}

std::unique_ptr<DB> OpenDB(const std::string& dir, const Options& o = SmallOptions()) {
  std::unique_ptr<DB> db;
  Status s = DB::Open(o, dir, &db);
  if (!s.ok()) throw test::Failure{"abrir: " + s.ToString()};
  return db;
}

}

TEST(Crc32cValoresConhecidos) {
  CHECK_EQ(crc32c::Value("123456789", 9), 0xe3069283u);
  uint32_t c = crc32c::Value("arca", 4);
  CHECK_EQ(crc32c::Unmask(crc32c::Mask(c)), c);
}

TEST(VarintIdaEVolta) {
  std::string s;
  std::vector<uint64_t> values = {0, 1, 127, 128, 300, 1ull << 32, ~0ull};
  for (auto v : values) PutVarint64(&s, v);
  std::string_view in(s);
  for (auto v : values) {
    uint64_t out = 0;
    CHECK(GetVarint64(&in, &out));
    CHECK_EQ(out, v);
  }
  CHECK(in.empty());
}

TEST(SkipListOrdenada) {
  Arena arena;
  struct Cmp {
    int operator()(uint64_t a, uint64_t b) const { return a < b ? -1 : (a > b ? 1 : 0); }
  };
  SkipList<uint64_t, Cmp> list(Cmp{}, &arena);
  std::mt19937 rng(7);
  std::set<uint64_t> model;
  for (int i = 0; i < 2000; ++i) {
    uint64_t v = rng() % 5000;
    if (model.insert(v).second) list.Insert(v);
  }
  SkipList<uint64_t, Cmp>::Iterator it(&list);
  it.SeekToFirst();
  for (uint64_t v : model) {
    CHECK(it.Valid());
    CHECK_EQ(it.key(), v);
    it.Next();
  }
  CHECK(!it.Valid());
}

TEST(BloomSemFalsoNegativo) {
  BloomFilter bloom(10);
  std::vector<std::string> keys;
  for (int i = 0; i < 1000; ++i) keys.push_back(Key(i));
  std::string filter;
  bloom.Create(keys, &filter);
  for (const auto& k : keys) CHECK(BloomFilter::KeyMayMatch(k, filter));
  int false_positives = 0;
  for (int i = 1000; i < 11000; ++i) false_positives += BloomFilter::KeyMayMatch(Key(i), filter) ? 1 : 0;
  CHECK(false_positives < 300);
}

TEST(EscritaLeituraERemocao) {
  auto dir = test::TempDir("basico");
  {
    auto db = OpenDB(dir);
    CHECK_OK(db->Put({}, "a", "1"));
    CHECK_OK(db->Put({}, "b", "2"));
    std::string v;
    CHECK_OK(db->Get({}, "a", &v));
    CHECK_EQ(v, "1");
    CHECK_OK(db->Delete({}, "a"));
    CHECK(db->Get({}, "a", &v).IsNotFound());
    CHECK_OK(db->Get({}, "b", &v));
    CHECK_EQ(v, "2");
  }
  DestroyDB(dir);
}

TEST(ModeloAleatorioComCompactacao) {
  auto dir = test::TempDir("modelo");
  std::map<std::string, std::string> model;
  std::mt19937 rng(42);
  {
    auto db = OpenDB(dir);
    for (int i = 0; i < 30000; ++i) {
      std::string k = Key(static_cast<int>(rng() % 4000));
      if (rng() % 5 == 0) {
        CHECK_OK(db->Delete({}, k));
        model.erase(k);
      } else {
        std::string v = "valor-" + std::to_string(i) + std::string(rng() % 64, 'x');
        CHECK_OK(db->Put({}, k, v));
        model[k] = v;
      }
    }
    auto check = [&](DB* d) {
      for (int i = 0; i < 4000; ++i) {
        std::string k = Key(i);
        std::string v;
        Status s = d->Get({}, k, &v);
        auto it = model.find(k);
        if (it == model.end()) {
          CHECK(s.IsNotFound());
        } else {
          CHECK_OK(s);
          CHECK_EQ(v, it->second);
        }
      }
      auto iter = d->NewIterator({});
      auto mit = model.begin();
      for (iter->SeekToFirst(); iter->Valid(); iter->Next(), ++mit) {
        CHECK(mit != model.end());
        CHECK_EQ(std::string(iter->key()), mit->first);
        CHECK_EQ(std::string(iter->value()), mit->second);
      }
      CHECK(mit == model.end());
      CHECK_OK(iter->status());
    };
    check(db.get());
    CHECK_OK(db->CompactAll());
    check(db.get());
    auto stats = db->GetStats();
    CHECK(stats.sst_files > 0);
    CHECK(stats.compactions > 0);
  }
  {
    auto db = OpenDB(dir);
    for (auto& [k, v] : model) {
      std::string got;
      CHECK_OK(db->Get({}, k, &got));
      CHECK_EQ(got, v);
    }
  }
  DestroyDB(dir);
}

TEST(RecuperacaoPeloWal) {
  auto dir = test::TempDir("wal");
  {
    auto db = OpenDB(dir);
    for (int i = 0; i < 500; ++i) CHECK_OK(db->Put({}, Key(i), "v" + std::to_string(i)));
    CHECK_OK(db->Delete({}, Key(10)));
  }
  {
    auto db = OpenDB(dir);
    std::string v;
    CHECK_OK(db->Get({}, Key(499), &v));
    CHECK_EQ(v, "v499");
    CHECK(db->Get({}, Key(10), &v).IsNotFound());
  }
  DestroyDB(dir);
}

TEST(RecuperacaoComCaudaCorrompida) {
  auto dir = test::TempDir("cauda");
  {
    auto db = OpenDB(dir);
    for (int i = 0; i < 100; ++i) CHECK_OK(db->Put({}, Key(i), "v"));
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".wal" && entry.file_size() > 0) {
      std::filesystem::resize_file(entry.path(), entry.file_size() - 3);
    }
  }
  {
    auto db = OpenDB(dir);
    std::string v;
    CHECK_OK(db->Get({}, Key(0), &v));
    CHECK_OK(db->Get({}, Key(98), &v));
  }
  DestroyDB(dir);
}

TEST(SnapshotEnxergaVersaoAntiga) {
  auto dir = test::TempDir("snapshot");
  {
    auto db = OpenDB(dir);
    CHECK_OK(db->Put({}, "k", "antigo"));
    auto snap = db->GetSnapshot();
    CHECK_OK(db->Put({}, "k", "novo"));
    for (int i = 0; i < 5000; ++i) CHECK_OK(db->Put({}, Key(i), std::string(40, 'z')));
    CHECK_OK(db->CompactAll());
    ReadOptions ro;
    ro.snapshot = snap.get();
    std::string v;
    CHECK_OK(db->Get(ro, "k", &v));
    CHECK_EQ(v, "antigo");
    CHECK_OK(db->Get({}, "k", &v));
    CHECK_EQ(v, "novo");
  }
  DestroyDB(dir);
}

TEST(EscritasConcorrentes) {
  auto dir = test::TempDir("concorrencia");
  {
    auto db = OpenDB(dir);
    std::vector<std::thread> threads;
    std::atomic<int> errors{0};
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&, t] {
        for (int i = 0; i < 3000; ++i) {
          if (!db->Put({}, "t" + std::to_string(t) + "-" + Key(i), std::to_string(i)).ok()) ++errors;
          std::string v;
          db->Get({}, "t" + std::to_string(t) + "-" + Key(i / 2), &v);
        }
      });
    }
    for (auto& th : threads) th.join();
    CHECK_EQ(errors.load(), 0);
    for (int t = 0; t < 4; ++t) {
      for (int i = 0; i < 3000; i += 97) {
        std::string v;
        CHECK_OK(db->Get({}, "t" + std::to_string(t) + "-" + Key(i), &v));
        CHECK_EQ(v, std::to_string(i));
      }
    }
  }
  DestroyDB(dir);
}

TEST(CheckpointAbreComoBanco) {
  auto dir = test::TempDir("origem");
  auto copy = test::TempDir("copia");
  {
    auto db = OpenDB(dir);
    for (int i = 0; i < 2000; ++i) CHECK_OK(db->Put({}, Key(i), "v" + std::to_string(i)));
    CHECK_OK(db->Checkpoint(copy));
    CHECK_OK(db->Put({}, Key(1), "depois"));
  }
  {
    auto db = OpenDB(copy);
    std::string v;
    CHECK_OK(db->Get({}, Key(1), &v));
    CHECK_EQ(v, "v1");
    CHECK_OK(db->Get({}, Key(1999), &v));
  }
  DestroyDB(dir);
  DestroyDB(copy);
}

TEST(VerificacaoDeChecksums) {
  auto dir = test::TempDir("verifica");
  {
    auto db = OpenDB(dir);
    for (int i = 0; i < 3000; ++i) CHECK_OK(db->Put({}, Key(i), std::string(100, 'a' + (i % 26))));
    CHECK_OK(db->Flush());
    VerifyResult r;
    CHECK_OK(db->VerifyChecksums(&r));
    CHECK(r.files > 0);
    CHECK_EQ(r.ok, r.files);
    CHECK(r.bytes > 0);
    CHECK(r.corrupt.empty());

    std::string victim;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
      if (e.path().extension() == ".sst") {
        victim = e.path().string();
        break;
      }
    }
    CHECK(!victim.empty());
    {
      std::FILE* f = std::fopen(victim.c_str(), "r+b");
      CHECK(f != nullptr);
      std::fseek(f, 100, SEEK_SET);
      int ch = std::fgetc(f);
      std::fseek(f, 100, SEEK_SET);
      std::fputc(ch ^ 0x5a, f);
      std::fclose(f);
    }
    CHECK_OK(db->VerifyChecksums(&r));
    CHECK_EQ(r.corrupt.size(), size_t(1));
    CHECK_EQ(r.ok + 1, r.files);
  }
  DestroyDB(dir);
}

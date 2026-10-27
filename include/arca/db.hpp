#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arca/iterator.hpp"
#include "arca/options.hpp"
#include "arca/status.hpp"
#include "arca/write_batch.hpp"

namespace arca {

inline constexpr const char* kVersion = "1.2.0";

class Snapshot {
 public:
  virtual ~Snapshot() = default;
  virtual uint64_t sequence() const = 0;
};

struct TableFileInfo {
  uint64_t number = 0;
  uint64_t bytes = 0;
  uint64_t entries = 0;
  uint64_t deletions = 0;
  std::string smallest;
  std::string largest;
};

struct LevelInfo {
  int level = 0;
  uint64_t bytes = 0;
  uint64_t max_bytes = 0;
  double score = 0;
  std::vector<TableFileInfo> files;
};

struct CompactionRecord {
  int from_level = 0;
  int to_level = 0;
  int inputs = 0;
  int outputs = 0;
  uint64_t bytes_read = 0;
  uint64_t bytes_written = 0;
  double ms = 0;
  int64_t at_ms = 0;
  std::string reason;
};

struct EngineStats {
  uint64_t last_sequence = 0;
  uint64_t memtable_bytes = 0;
  uint64_t memtable_entries = 0;
  uint64_t immutable_bytes = 0;
  uint64_t wal_number = 0;
  uint64_t wal_bytes = 0;
  uint64_t sst_files = 0;
  uint64_t sst_bytes = 0;
  uint64_t flushes = 0;
  uint64_t compactions = 0;
  uint64_t compaction_bytes_read = 0;
  uint64_t compaction_bytes_written = 0;
  uint64_t user_bytes_written = 0;
  uint64_t flush_bytes = 0;
  uint64_t write_stalls = 0;
  uint64_t bloom_checks = 0;
  uint64_t bloom_useful = 0;
  uint64_t block_cache_bytes = 0;
  uint64_t block_cache_capacity = 0;
  uint64_t block_cache_hits = 0;
  uint64_t block_cache_misses = 0;
  uint64_t snapshots = 0;
  uint64_t estimated_keys = 0;
  double write_amplification = 0;
  bool compacting = false;
  std::string background_error;
  std::vector<LevelInfo> levels;
  std::vector<CompactionRecord> recent_compactions;
};

struct VerifyResult {
  uint64_t files = 0;
  uint64_t bytes = 0;
  uint64_t ok = 0;
  std::vector<std::pair<std::string, std::string>> corrupt;
  double ms = 0;
};

class DB {
 public:
  static Status Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db);

  DB() = default;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  virtual ~DB() = default;

  virtual Status Put(const WriteOptions& options, std::string_view key, std::string_view value) = 0;
  virtual Status Delete(const WriteOptions& options, std::string_view key) = 0;
  virtual Status Write(const WriteOptions& options, WriteBatch* batch) = 0;
  virtual Status Get(const ReadOptions& options, std::string_view key, std::string* value) = 0;
  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) = 0;
  virtual std::shared_ptr<const Snapshot> GetSnapshot() = 0;
  virtual Status Flush() = 0;
  virtual Status CompactAll() = 0;
  virtual Status Checkpoint(const std::string& directory) = 0;
  virtual Status SyncWal() = 0;
  virtual void SetSyncMode(SyncMode mode) = 0;
  virtual SyncMode GetSyncMode() const = 0;
  virtual EngineStats GetStats() = 0;
  virtual Status VerifyChecksums(VerifyResult* result) = 0;
};

Status DestroyDB(const std::string& path);

}

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>

#include "arca/db.hpp"
#include "db/dbformat.hpp"
#include "db/log.hpp"
#include "db/memtable.hpp"
#include "db/table_cache.hpp"
#include "db/version_set.hpp"
#include "table/cache.hpp"
#include "util/file.hpp"

namespace arca {

class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, std::string dbname);
  ~DBImpl() override;

  Status Init();

  Status Put(const WriteOptions& options, std::string_view key, std::string_view value) override;
  Status Delete(const WriteOptions& options, std::string_view key) override;
  Status Write(const WriteOptions& options, WriteBatch* batch) override;
  Status Get(const ReadOptions& options, std::string_view key, std::string* value) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;
  std::shared_ptr<const Snapshot> GetSnapshot() override;
  Status Flush() override;
  Status CompactAll() override;
  Status Checkpoint(const std::string& directory) override;
  Status SyncWal() override;
  void SetSyncMode(SyncMode mode) override { sync_mode_.store(static_cast<int>(mode)); }
  SyncMode GetSyncMode() const override { return static_cast<SyncMode>(sync_mode_.load()); }
  EngineStats GetStats() override;
  Status VerifyChecksums(VerifyResult* result) override;

 private:
  struct Writer;
  struct CompactionState;

  struct SuperVersion {
    std::shared_ptr<MemTable> mem;
    std::shared_ptr<MemTable> imm;
    std::shared_ptr<const Version> current;
  };

  using Lock = std::unique_lock<std::mutex>;

  Status NewDB();
  Status Recover(Lock& lock);
  Status RecoverLogFile(uint64_t log_number, VersionEdit* edit, uint64_t* max_sequence, std::shared_ptr<MemTable>* mem,
                        Lock& lock);
  Status MakeRoomForWrite(Lock& lock, bool force);
  WriteBatch* BuildBatchGroup(Writer** last_writer);
  void BackgroundMain();
  void SyncMain();
  void BackgroundCompaction(Lock& lock);
  void CompactMemTable(Lock& lock);
  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit, Lock& lock);
  Status BuildTable(Iterator* iter, FileMetaData* meta);
  Status DoCompactionWork(CompactionState* compact, Lock& lock);
  Status OpenCompactionOutputFile(CompactionState* compact, Lock& lock);
  Status FinishCompactionOutputFile(CompactionState* compact);
  Status InstallCompactionResults(CompactionState* compact, Lock& lock);
  void RemoveObsoleteFiles(Lock& lock);
  void InstallSuperVersion();
  std::shared_ptr<const SuperVersion> GetSuperVersion() const;
  void ReleaseSnapshot(uint64_t sequence);
  void RecordCompaction(CompactionRecord record);
  void Log(const std::string& message) const;

  const Options options_;
  const std::string dbname_;
  const InternalKeyComparator icmp_;
  EngineCounters counters_;
  std::unique_ptr<FileLock> db_lock_;
  std::unique_ptr<BlockCache> block_cache_;
  std::unique_ptr<TableCache> table_cache_;
  std::unique_ptr<VersionSet> versions_;

  std::mutex mu_;
  std::condition_variable bg_work_cv_;
  std::condition_variable bg_done_cv_;
  std::atomic<bool> shutting_down_{false};
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;
  std::atomic<bool> has_imm_{false};

  std::mutex log_mu_;
  std::unique_ptr<WritableFile> logfile_;
  std::unique_ptr<wal::Writer> log_;
  uint64_t logfile_number_ = 0;
  std::atomic<bool> wal_dirty_{false};

  std::deque<Writer*> writers_;
  WriteBatch tmp_batch_;
  std::multiset<uint64_t> snapshots_;
  std::set<uint64_t> pending_outputs_;
  Status bg_error_;
  bool compacting_ = false;
  int file_deletions_disabled_ = 0;

  struct ManualCompaction {
    bool requested = false;
    bool done = true;
    int level = 0;
    int target = 0;
    Status status;
  } manual_;

  mutable std::shared_mutex sv_mu_;
  std::shared_ptr<const SuperVersion> sv_;

  std::atomic<int> sync_mode_;
  std::condition_variable sync_cv_;
  std::mutex sync_mu_;
  std::thread bg_thread_;
  std::thread sync_thread_;
  std::deque<CompactionRecord> recent_compactions_;
};

}

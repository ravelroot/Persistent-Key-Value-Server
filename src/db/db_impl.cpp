#include "db/db_impl.hpp"

#include <algorithm>
#include <chrono>

#include "db/filename.hpp"
#include "db/write_batch_internal.hpp"
#include "table/iterators.hpp"
#include "table/table.hpp"

namespace arca {

namespace {

int64_t UnixMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

class SnapshotImpl final : public Snapshot {
 public:
  explicit SnapshotImpl(uint64_t seq) : seq_(seq) {}
  uint64_t sequence() const override { return seq_; }

 private:
  uint64_t seq_;
};

class DBIter final : public Iterator {
 public:
  DBIter(std::unique_ptr<Iterator> iter, uint64_t sequence, std::shared_ptr<const void> pin)
      : iter_(std::move(iter)), sequence_(sequence), pin_(std::move(pin)) {}

  bool Valid() const override { return valid_; }
  std::string_view key() const override { return ExtractUserKey(iter_->key()); }
  std::string_view value() const override { return iter_->value(); }
  Status status() const override { return iter_->status(); }

  void Next() override {
    std::string_view current = ExtractUserKey(iter_->key());
    skip_.assign(current.data(), current.size());
    iter_->Next();
    FindNextUserEntry(true);
  }

  void Seek(std::string_view target) override {
    std::string ikey;
    AppendInternalKey(&ikey, target, sequence_, kValueTypeForSeek);
    iter_->Seek(ikey);
    FindNextUserEntry(false);
  }

  void SeekToFirst() override {
    iter_->SeekToFirst();
    FindNextUserEntry(false);
  }

 private:
  void FindNextUserEntry(bool skipping) {
    while (iter_->Valid()) {
      ParsedInternalKey ikey;
      if (ParseInternalKey(iter_->key(), &ikey) && ikey.sequence <= sequence_) {
        if (ikey.type == kTypeDeletion) {
          skip_.assign(ikey.user_key.data(), ikey.user_key.size());
          skipping = true;
        } else if (!skipping || ikey.user_key > skip_) {
          valid_ = true;
          return;
        }
      }
      iter_->Next();
    }
    valid_ = false;
  }

  std::unique_ptr<Iterator> iter_;
  uint64_t sequence_;
  std::shared_ptr<const void> pin_;
  std::string skip_;
  bool valid_ = false;
};

}

struct DBImpl::Writer {
  explicit Writer(WriteBatch* b, bool s) : batch(b), sync(s) {}
  WriteBatch* batch;
  bool sync;
  bool done = false;
  Status status;
  std::condition_variable cv;
};

struct DBImpl::CompactionState {
  struct Output {
    uint64_t number = 0;
    uint64_t file_size = 0;
    uint64_t entries = 0;
    uint64_t deletions = 0;
    InternalKey smallest;
    InternalKey largest;
  };

  explicit CompactionState(std::unique_ptr<Compaction> c) : compaction(std::move(c)) {}

  std::unique_ptr<Compaction> compaction;
  uint64_t smallest_snapshot = 0;
  std::vector<Output> outputs;
  std::unique_ptr<WritableFile> outfile;
  std::unique_ptr<TableBuilder> builder;
  uint64_t total_bytes = 0;

  Output* current_output() { return &outputs.back(); }
};

Status DB::Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db) {
  auto impl = std::make_unique<DBImpl>(options, path);
  Status s = impl->Init();
  if (!s.ok()) return s;
  *db = std::move(impl);
  return Status::OK();
}

Status DestroyDB(const std::string& path) {
  std::vector<std::string> children;
  Status s = GetChildren(path, &children);
  if (!s.ok()) return s.IsNotFound() ? Status::OK() : s;
  for (const auto& name : children) {
    uint64_t number;
    FileType type;
    if (ParseFileName(name, &number, &type)) RemoveFile(JoinPath(path, name));
  }
  RemoveFile(path);
  return Status::OK();
}

DBImpl::DBImpl(const Options& options, std::string dbname)
    : options_(options), dbname_(std::move(dbname)), sync_mode_(static_cast<int>(options.sync_mode)) {
  block_cache_ = std::make_unique<BlockCache>(options_.block_cache_capacity);
  table_cache_ = std::make_unique<TableCache>(dbname_, options_, block_cache_.get(), &counters_);
  versions_ = std::make_unique<VersionSet>(dbname_, &options_, table_cache_.get(), &icmp_);
}

DBImpl::~DBImpl() {
  {
    Lock lock(mu_);
    shutting_down_.store(true);
    bg_work_cv_.notify_all();
    bg_done_cv_.notify_all();
  }
  {
    std::lock_guard<std::mutex> g(sync_mu_);
    sync_cv_.notify_all();
  }
  if (bg_thread_.joinable()) bg_thread_.join();
  if (sync_thread_.joinable()) sync_thread_.join();
  std::lock_guard<std::mutex> g(log_mu_);
  if (logfile_) {
    logfile_->Sync();
    logfile_->Close();
  }
}

void DBImpl::Log(const std::string& message) const {
  if (options_.info_log) options_.info_log(message);
}

Status DBImpl::NewDB() {
  VersionEdit edit;
  edit.SetLogNumber(0);
  edit.SetNextFile(2);
  edit.SetLastSequence(0);
  std::string manifest = ManifestFileName(dbname_, 1);
  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Open(manifest, true, &file);
  if (!s.ok()) return s;
  {
    wal::Writer log(file.get());
    std::string record;
    edit.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) s = file->Sync();
    if (s.ok()) s = file->Close();
  }
  if (s.ok()) {
    s = SetCurrentFile(dbname_, 1);
  } else {
    RemoveFile(manifest);
  }
  return s;
}

Status DBImpl::Init() {
  Status s = CreateDirs(dbname_);
  if (!s.ok()) return s;
  s = FileLock::Acquire(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) return s;
  Lock lock(mu_);
  s = Recover(lock);
  if (!s.ok()) return s;
  bg_thread_ = std::thread([this] { BackgroundMain(); });
  sync_thread_ = std::thread([this] { SyncMain(); });
  bg_work_cv_.notify_one();
  return Status::OK();
}

Status DBImpl::Recover(Lock& lock) {
  if (!FileExists(CurrentFileName(dbname_))) {
    if (!options_.create_if_missing) return Status::InvalidArgument(dbname_ + ": banco não existe");
    Status s = NewDB();
    if (!s.ok()) return s;
  } else if (options_.error_if_exists) {
    return Status::InvalidArgument(dbname_ + ": banco já existe");
  }
  Status s = versions_->Recover();
  if (!s.ok()) return s;

  std::vector<std::string> children;
  s = GetChildren(dbname_, &children);
  if (!s.ok()) return s;
  std::set<uint64_t> expected;
  versions_->AddLiveFiles(&expected);
  std::vector<uint64_t> logs;
  for (const auto& name : children) {
    uint64_t number;
    FileType type;
    if (!ParseFileName(name, &number, &type)) continue;
    expected.erase(number);
    if (type == FileType::kLog && number >= versions_->LogNumber()) logs.push_back(number);
  }
  if (!expected.empty()) {
    return Status::Corruption(std::to_string(expected.size()) + " arquivo(s) SSTable ausentes, por exemplo " +
                              TableFileName(dbname_, *expected.begin()));
  }
  std::sort(logs.begin(), logs.end());
  VersionEdit edit;
  uint64_t max_sequence = 0;
  std::shared_ptr<MemTable> mem;
  for (uint64_t log : logs) {
    versions_->MarkFileNumberUsed(log);
    s = RecoverLogFile(log, &edit, &max_sequence, &mem, lock);
    if (!s.ok()) return s;
  }
  if (mem && mem->Entries() > 0) {
    s = WriteLevel0Table(mem.get(), &edit, lock);
    if (!s.ok()) return s;
  }
  if (versions_->LastSequence() < max_sequence) versions_->SetLastSequence(max_sequence);

  logfile_number_ = versions_->NewFileNumber();
  s = WritableFile::Open(LogFileName(dbname_, logfile_number_), true, &logfile_);
  if (!s.ok()) return s;
  log_ = std::make_unique<wal::Writer>(logfile_.get());
  edit.SetLogNumber(logfile_number_);
  s = versions_->LogAndApply(&edit, lock);
  if (!s.ok()) return s;
  mem_ = std::make_shared<MemTable>();
  InstallSuperVersion();
  RemoveObsoleteFiles(lock);
  if (!logs.empty()) {
    Log("recuperação concluída: " + std::to_string(logs.size()) + " log(s) reaplicado(s), sequência " +
        std::to_string(versions_->LastSequence()));
  }
  return Status::OK();
}

Status DBImpl::RecoverLogFile(uint64_t log_number, VersionEdit* edit, uint64_t* max_sequence,
                              std::shared_ptr<MemTable>* mem, Lock& lock) {
  class Reporter final : public wal::Reader::Reporter {
   public:
    Reporter(const DBImpl* db, Status* status, bool paranoid, std::string name)
        : db_(db), status_(status), paranoid_(paranoid), name_(std::move(name)) {}
    void Corruption(size_t bytes, const Status& s) override {
      db_->Log(name_ + ": descartando " + std::to_string(bytes) + " bytes (" + s.ToString() + ")");
      if (paranoid_ && status_->ok()) *status_ = s;
    }

   private:
    const DBImpl* db_;
    Status* status_;
    bool paranoid_;
    std::string name_;
  };

  std::string fname = LogFileName(dbname_, log_number);
  std::unique_ptr<SequentialFile> file;
  Status s = SequentialFile::Open(fname, &file);
  if (!s.ok()) return s;
  Status status;
  Reporter reporter(this, &status, options_.paranoid_checks, fname);
  wal::Reader reader(file.get(), &reporter);
  std::string scratch;
  std::string_view record;
  WriteBatch batch;
  while (reader.ReadRecord(&record, &scratch) && status.ok()) {
    if (record.size() < WriteBatchInternal::kHeader) {
      reporter.Corruption(record.size(), Status::Corruption("registro de log pequeno demais"));
      continue;
    }
    WriteBatchInternal::SetContents(&batch, record);
    if (!*mem) *mem = std::make_shared<MemTable>();
    status = WriteBatchInternal::InsertInto(&batch, mem->get());
    if (!status.ok()) break;
    uint64_t last = WriteBatchInternal::Sequence(&batch) + WriteBatchInternal::Count(&batch) - 1;
    if (last > *max_sequence) *max_sequence = last;
    if ((*mem)->ApproximateMemoryUsage() > options_.write_buffer_size) {
      status = WriteLevel0Table(mem->get(), edit, lock);
      mem->reset();
      if (!status.ok()) break;
    }
  }
  return status;
}

void DBImpl::InstallSuperVersion() {
  auto sv = std::make_shared<SuperVersion>();
  sv->mem = mem_;
  sv->imm = imm_;
  sv->current = versions_->current();
  std::unique_lock<std::shared_mutex> lock(sv_mu_);
  sv_ = std::move(sv);
}

std::shared_ptr<const DBImpl::SuperVersion> DBImpl::GetSuperVersion() const {
  std::shared_lock<std::shared_mutex> lock(sv_mu_);
  return sv_;
}

Status DBImpl::Put(const WriteOptions& options, std::string_view key, std::string_view value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, &batch);
}

Status DBImpl::Delete(const WriteOptions& options, std::string_view key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, &batch);
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* batch) {
  Writer w(batch, options.sync || GetSyncMode() == SyncMode::kAlways);
  Lock lock(mu_);
  writers_.push_back(&w);
  while (!w.done && &w != writers_.front()) w.cv.wait(lock);
  if (w.done) return w.status;

  Status status = MakeRoomForWrite(lock, batch == nullptr);
  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer = &w;
  if (status.ok() && batch != nullptr) {
    WriteBatch* updates = BuildBatchGroup(&last_writer);
    WriteBatchInternal::SetSequence(updates, last_sequence + 1);
    last_sequence += WriteBatchInternal::Count(updates);
    std::shared_ptr<MemTable> mem = mem_;
    bool sync_error = false;
    lock.unlock();
    {
      std::lock_guard<std::mutex> g(log_mu_);
      status = log_->AddRecord(WriteBatchInternal::Contents(updates));
      if (status.ok() && w.sync) {
        status = logfile_->Sync();
        if (!status.ok()) sync_error = true;
      } else if (status.ok()) {
        wal_dirty_.store(true, std::memory_order_relaxed);
      }
    }
    if (status.ok()) status = WriteBatchInternal::InsertInto(updates, mem.get());
    counters_.user_bytes_written.fetch_add(WriteBatchInternal::ByteSize(updates), std::memory_order_relaxed);
    lock.lock();
    if (sync_error) bg_error_ = status;
    if (updates == &tmp_batch_) tmp_batch_.Clear();
    versions_->SetLastSequence(last_sequence);
  }

  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.notify_one();
    }
    if (ready == last_writer) break;
  }
  if (!writers_.empty()) writers_.front()->cv.notify_one();
  return status;
}

WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  size_t size = WriteBatchInternal::ByteSize(first->batch);
  size_t max_size = 1 << 20;
  if (size <= (128 << 10)) max_size = size + (128 << 10);
  *last_writer = first;
  auto iter = writers_.begin();
  ++iter;
  for (; iter != writers_.end(); ++iter) {
    Writer* w = *iter;
    if (w->sync && !first->sync) break;
    if (w->batch == nullptr) break;
    size += WriteBatchInternal::ByteSize(w->batch);
    if (size > max_size) break;
    if (result == first->batch) {
      result = &tmp_batch_;
      tmp_batch_.Clear();
      WriteBatchInternal::Append(result, first->batch);
    }
    WriteBatchInternal::Append(result, w->batch);
    *last_writer = w;
  }
  return result;
}

Status DBImpl::MakeRoomForWrite(Lock& lock, bool force) {
  bool allow_delay = !force;
  while (true) {
    if (!bg_error_.ok()) return bg_error_;
    if (shutting_down_.load()) return Status::IOError("o banco está sendo fechado");
    const int level0 = versions_->current()->NumFiles(0);
    if (allow_delay && level0 >= options_.level0_slowdown_trigger) {
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      allow_delay = false;
      counters_.write_stalls.fetch_add(1, std::memory_order_relaxed);
      lock.lock();
    } else if (!force && mem_->ApproximateMemoryUsage() <= options_.write_buffer_size) {
      break;
    } else if (imm_) {
      counters_.write_stalls.fetch_add(1, std::memory_order_relaxed);
      bg_done_cv_.wait(lock);
    } else if (level0 >= options_.level0_stop_trigger) {
      counters_.write_stalls.fetch_add(1, std::memory_order_relaxed);
      bg_work_cv_.notify_one();
      bg_done_cv_.wait(lock);
    } else {
      uint64_t new_log_number = versions_->NewFileNumber();
      std::unique_ptr<WritableFile> file;
      Status s = WritableFile::Open(LogFileName(dbname_, new_log_number), true, &file);
      if (!s.ok()) {
        versions_->ReuseFileNumber(new_log_number);
        return s;
      }
      {
        std::lock_guard<std::mutex> g(log_mu_);
        logfile_->Sync();
        logfile_->Close();
        logfile_ = std::move(file);
        log_ = std::make_unique<wal::Writer>(logfile_.get());
        logfile_number_ = new_log_number;
        wal_dirty_.store(false);
      }
      imm_ = mem_;
      has_imm_.store(true, std::memory_order_release);
      mem_ = std::make_shared<MemTable>();
      force = false;
      InstallSuperVersion();
      bg_work_cv_.notify_one();
    }
  }
  return Status::OK();
}

Status DBImpl::Get(const ReadOptions& options, std::string_view key, std::string* value) {
  uint64_t sequence = options.snapshot != nullptr ? options.snapshot->sequence() : versions_->LastSequence();
  auto sv = GetSuperVersion();
  LookupKey lkey(key, sequence);
  Status s;
  if (sv->mem->Get(lkey, value, &s)) return s;
  if (sv->imm && sv->imm->Get(lkey, value, &s)) return s;
  return sv->current->Get(options, lkey, value);
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& options) {
  uint64_t sequence = options.snapshot != nullptr ? options.snapshot->sequence() : versions_->LastSequence();
  auto sv = GetSuperVersion();
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(sv->mem->NewIterator());
  if (sv->imm) children.push_back(sv->imm->NewIterator());
  sv->current->AddIterators(options, &children);
  auto merged = NewMergingIterator(&icmp_, std::move(children));
  return std::make_unique<DBIter>(std::move(merged), sequence, sv);
}

std::shared_ptr<const Snapshot> DBImpl::GetSnapshot() {
  Lock lock(mu_);
  uint64_t seq = versions_->LastSequence();
  snapshots_.insert(seq);
  return std::shared_ptr<const Snapshot>(new SnapshotImpl(seq), [this](const Snapshot* s) {
    ReleaseSnapshot(s->sequence());
    delete s;
  });
}

void DBImpl::ReleaseSnapshot(uint64_t sequence) {
  Lock lock(mu_);
  auto it = snapshots_.find(sequence);
  if (it != snapshots_.end()) snapshots_.erase(it);
}

Status DBImpl::Flush() {
  {
    Lock lock(mu_);
    if (mem_->Entries() == 0 && !imm_) return Status::OK();
  }
  Status s = Write(WriteOptions(), nullptr);
  if (!s.ok()) return s;
  Lock lock(mu_);
  bg_done_cv_.wait(lock, [this] { return !imm_ || !bg_error_.ok() || shutting_down_.load(); });
  return bg_error_;
}

Status DBImpl::CompactAll() {
  Status s = Flush();
  if (!s.ok()) return s;
  Lock lock(mu_);
  bg_done_cv_.wait(lock, [this] { return manual_.done || shutting_down_.load(); });
  int deepest = 0;
  auto current = versions_->current();
  for (int level = 0; level < kNumLevels; ++level) {
    if (current->NumFiles(level) > 0) deepest = level;
  }
  manual_.requested = true;
  manual_.done = false;
  manual_.level = 0;
  manual_.target = std::max(1, deepest);
  manual_.status = Status::OK();
  bg_work_cv_.notify_one();
  bg_done_cv_.wait(lock, [this] { return manual_.done || shutting_down_.load(); });
  if (!bg_error_.ok()) return bg_error_;
  return manual_.status;
}

Status DBImpl::SyncWal() {
  std::lock_guard<std::mutex> g(log_mu_);
  wal_dirty_.store(false);
  return logfile_->Sync();
}

void DBImpl::SyncMain() {
  std::unique_lock<std::mutex> lock(sync_mu_);
  while (!shutting_down_.load()) {
    sync_cv_.wait_for(lock, std::chrono::seconds(1));
    if (shutting_down_.load()) break;
    if (GetSyncMode() == SyncMode::kEverySecond && wal_dirty_.load(std::memory_order_relaxed)) {
      std::lock_guard<std::mutex> g(log_mu_);
      wal_dirty_.store(false);
      Status s = logfile_->Sync();
      if (!s.ok()) Log("falha ao sincronizar o WAL: " + s.ToString());
    }
  }
}

void DBImpl::BackgroundMain() {
  Lock lock(mu_);
  while (true) {
    bg_work_cv_.wait(lock, [this] {
      return shutting_down_.load() ||
             (bg_error_.ok() && (imm_ || manual_.requested || versions_->NeedsCompaction()));
    });
    if (shutting_down_.load()) break;
    compacting_ = true;
    BackgroundCompaction(lock);
    compacting_ = false;
    bg_done_cv_.notify_all();
  }
  if (manual_.requested) {
    manual_.requested = false;
    manual_.done = true;
    manual_.status = Status::IOError("o banco está sendo fechado");
  }
  bg_done_cv_.notify_all();
}

void DBImpl::CompactMemTable(Lock& lock) {
  VersionEdit edit;
  std::shared_ptr<MemTable> imm = imm_;
  Status s = WriteLevel0Table(imm.get(), &edit, lock);
  if (s.ok() && shutting_down_.load()) s = Status::IOError("o banco está sendo fechado");
  if (s.ok()) {
    edit.SetLogNumber(logfile_number_);
    s = versions_->LogAndApply(&edit, lock);
  }
  if (s.ok()) {
    imm_.reset();
    has_imm_.store(false, std::memory_order_release);
    InstallSuperVersion();
    counters_.flushes.fetch_add(1, std::memory_order_relaxed);
    RemoveObsoleteFiles(lock);
  } else if (!shutting_down_.load()) {
    bg_error_ = s;
    Log("falha ao gravar a memtable: " + s.ToString());
  }
  bg_done_cv_.notify_all();
}

Status DBImpl::BuildTable(Iterator* iter, FileMetaData* meta) {
  meta->file_size = 0;
  iter->SeekToFirst();
  std::string fname = TableFileName(dbname_, meta->number);
  if (!iter->Valid()) return iter->status();
  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Open(fname, true, &file);
  if (!s.ok()) return s;
  TableBuilder builder(options_, file.get());
  meta->smallest.DecodeFrom(iter->key());
  std::string last_key;
  for (; iter->Valid(); iter->Next()) {
    last_key.assign(iter->key().data(), iter->key().size());
    builder.Add(iter->key(), iter->value());
  }
  meta->largest.DecodeFrom(last_key);
  s = builder.Finish();
  if (s.ok()) {
    meta->file_size = builder.FileSize();
    meta->entries = builder.NumEntries();
    meta->deletions = builder.NumDeletions();
    s = file->Sync();
  }
  if (s.ok()) s = file->Close();
  if (s.ok()) s = iter->status();
  if (s.ok()) {
    std::shared_ptr<Table> table;
    s = table_cache_->FindTable(meta->number, &table);
  }
  if (!s.ok() || meta->file_size == 0) {
    file.reset();
    RemoveFile(fname);
  }
  return s;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit, Lock& lock) {
  auto start = std::chrono::steady_clock::now();
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);
  auto iter = mem->NewIterator();
  lock.unlock();
  Status s = BuildTable(iter.get(), &meta);
  iter.reset();
  lock.lock();
  pending_outputs_.erase(meta.number);
  if (s.ok() && meta.file_size > 0) {
    edit->AddFile(0, meta);
    counters_.flush_bytes.fetch_add(meta.file_size, std::memory_order_relaxed);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    Log("memtable gravada na SSTable " + std::to_string(meta.number) + " (" + std::to_string(meta.entries) +
        " entradas, " + std::to_string(meta.file_size) + " bytes, " + std::to_string(static_cast<int>(ms)) + " ms)");
  }
  return s;
}

void DBImpl::RecordCompaction(CompactionRecord record) {
  recent_compactions_.push_front(std::move(record));
  while (recent_compactions_.size() > 20) recent_compactions_.pop_back();
}

void DBImpl::BackgroundCompaction(Lock& lock) {
  if (imm_) {
    CompactMemTable(lock);
    return;
  }
  bool manual = manual_.requested;
  std::unique_ptr<Compaction> c;
  if (manual) {
    auto current = versions_->current();
    while (manual_.level < manual_.target && current->NumFiles(manual_.level) == 0) ++manual_.level;
    if (manual_.level < manual_.target) c = versions_->CompactLevel(manual_.level);
    if (!c) {
      manual_.requested = false;
      manual_.done = true;
      return;
    }
  } else {
    c = versions_->PickCompaction();
    if (!c) return;
  }

  Status s;
  auto start = std::chrono::steady_clock::now();
  if (!manual && c->IsTrivialMove()) {
    auto f = c->inputs_[0][0];
    c->edit()->RemoveFile(c->level(), f->number);
    c->edit()->AddFile(c->level() + 1, *f);
    s = versions_->LogAndApply(c->edit(), lock);
    if (s.ok()) {
      InstallSuperVersion();
      CompactionRecord rec;
      rec.from_level = c->level();
      rec.to_level = c->level() + 1;
      rec.inputs = 1;
      rec.outputs = 1;
      rec.at_ms = UnixMillis();
      rec.reason = c->reason_;
      RecordCompaction(rec);
      Log("SSTable " + std::to_string(f->number) + " movida de L" + std::to_string(c->level()) + " para L" +
          std::to_string(c->level() + 1));
    }
  } else {
    CompactionState state(std::move(c));
    s = DoCompactionWork(&state, lock);
    if (!s.ok()) {
      if (state.builder) state.builder.reset();
      if (state.outfile) state.outfile->Close();
    }
    for (const auto& out : state.outputs) pending_outputs_.erase(out.number);
    if (s.ok()) {
      Compaction* cc = state.compaction.get();
      uint64_t bytes_read = 0;
      for (int which = 0; which < 2; ++which) {
        for (const auto& f : cc->inputs_[which]) bytes_read += f->file_size;
      }
      uint64_t bytes_written = 0;
      for (const auto& out : state.outputs) bytes_written += out.file_size;
      counters_.compactions.fetch_add(1, std::memory_order_relaxed);
      counters_.compaction_bytes_read.fetch_add(bytes_read, std::memory_order_relaxed);
      counters_.compaction_bytes_written.fetch_add(bytes_written, std::memory_order_relaxed);
      CompactionRecord rec;
      rec.from_level = cc->level();
      rec.to_level = cc->level() + 1;
      rec.inputs = cc->num_input_files(0) + cc->num_input_files(1);
      rec.outputs = static_cast<int>(state.outputs.size());
      rec.bytes_read = bytes_read;
      rec.bytes_written = bytes_written;
      rec.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      rec.at_ms = UnixMillis();
      rec.reason = cc->reason_;
      Log("compactação L" + std::to_string(rec.from_level) + " -> L" + std::to_string(rec.to_level) + ": " +
          std::to_string(rec.inputs) + " arquivo(s) de entrada, " + std::to_string(rec.outputs) + " de saída, " +
          std::to_string(bytes_read) + " bytes lidos, " + std::to_string(bytes_written) + " gravados");
      RecordCompaction(std::move(rec));
    }
    RemoveObsoleteFiles(lock);
  }

  if (!s.ok() && !shutting_down_.load()) {
    bg_error_ = s;
    Log("falha na compactação: " + s.ToString());
  }
  if (manual) {
    if (!s.ok()) {
      manual_.status = s;
      manual_.requested = false;
      manual_.done = true;
    } else {
      ++manual_.level;
      if (manual_.level >= manual_.target) {
        manual_.requested = false;
        manual_.done = true;
      }
    }
  }
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact, Lock& lock) {
  uint64_t number;
  lock.lock();
  number = versions_->NewFileNumber();
  pending_outputs_.insert(number);
  lock.unlock();
  CompactionState::Output out;
  out.number = number;
  compact->outputs.push_back(out);
  Status s = WritableFile::Open(TableFileName(dbname_, number), true, &compact->outfile);
  if (s.ok()) compact->builder = std::make_unique<TableBuilder>(options_, compact->outfile.get());
  return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact) {
  Status s = compact->builder->Finish();
  auto* out = compact->current_output();
  out->file_size = compact->builder->FileSize();
  out->entries = compact->builder->NumEntries();
  out->deletions = compact->builder->NumDeletions();
  compact->total_bytes += out->file_size;
  compact->builder.reset();
  if (s.ok()) s = compact->outfile->Sync();
  if (s.ok()) s = compact->outfile->Close();
  compact->outfile.reset();
  if (s.ok() && out->entries > 0) {
    std::shared_ptr<Table> table;
    s = table_cache_->FindTable(out->number, &table);
  }
  return s;
}

Status DBImpl::InstallCompactionResults(CompactionState* compact, Lock& lock) {
  Compaction* c = compact->compaction.get();
  c->AddInputDeletions(c->edit());
  for (const auto& out : compact->outputs) {
    if (out.entries == 0) continue;
    FileMetaData meta;
    meta.number = out.number;
    meta.file_size = out.file_size;
    meta.entries = out.entries;
    meta.deletions = out.deletions;
    meta.smallest = out.smallest;
    meta.largest = out.largest;
    c->edit()->AddFile(c->level() + 1, meta);
  }
  Status s = versions_->LogAndApply(c->edit(), lock);
  if (s.ok()) InstallSuperVersion();
  return s;
}

Status DBImpl::DoCompactionWork(CompactionState* compact, Lock& lock) {
  Compaction* c = compact->compaction.get();
  compact->smallest_snapshot = snapshots_.empty() ? versions_->LastSequence() : *snapshots_.begin();
  std::unique_ptr<Iterator> input = versions_->MakeInputIterator(c);
  lock.unlock();
  input->SeekToFirst();
  Status status;
  std::string current_user_key;
  bool has_current_user_key = false;
  uint64_t last_sequence_for_key = kMaxSequenceNumber;
  while (input->Valid() && !shutting_down_.load()) {
    if (has_imm_.load(std::memory_order_acquire)) {
      lock.lock();
      if (imm_) CompactMemTable(lock);
      lock.unlock();
    }
    std::string_view key = input->key();
    if (compact->builder && c->ShouldStopBefore(key)) {
      status = FinishCompactionOutputFile(compact);
      if (!status.ok()) break;
    }
    bool drop = false;
    ParsedInternalKey ikey;
    if (!ParseInternalKey(key, &ikey)) {
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key || ikey.user_key != current_user_key) {
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }
      if (last_sequence_for_key <= compact->smallest_snapshot) {
        drop = true;
      } else if (ikey.type == kTypeDeletion && ikey.sequence <= compact->smallest_snapshot &&
                 c->IsBaseLevelForKey(ikey.user_key)) {
        drop = true;
      }
      last_sequence_for_key = ikey.sequence;
    }
    if (!drop) {
      if (!compact->builder) {
        status = OpenCompactionOutputFile(compact, lock);
        if (!status.ok()) break;
      }
      if (compact->builder->NumEntries() == 0) compact->current_output()->smallest.DecodeFrom(key);
      compact->current_output()->largest.DecodeFrom(key);
      compact->builder->Add(key, input->value());
      if (compact->builder->FileSize() >= c->MaxOutputFileSize()) {
        status = FinishCompactionOutputFile(compact);
        if (!status.ok()) break;
      }
    }
    input->Next();
  }
  if (status.ok() && shutting_down_.load()) status = Status::IOError("o banco está sendo fechado");
  if (status.ok() && compact->builder) status = FinishCompactionOutputFile(compact);
  if (status.ok()) status = input->status();
  input.reset();
  lock.lock();
  if (status.ok()) status = InstallCompactionResults(compact, lock);
  return status;
}

void DBImpl::RemoveObsoleteFiles(Lock& lock) {
  if (file_deletions_disabled_ > 0 || !bg_error_.ok()) return;
  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);
  std::vector<std::string> children;
  if (!GetChildren(dbname_, &children).ok()) return;
  std::vector<std::string> to_delete;
  for (const auto& name : children) {
    uint64_t number;
    FileType type;
    if (!ParseFileName(name, &number, &type)) continue;
    bool keep = true;
    switch (type) {
      case FileType::kLog:
        keep = number >= versions_->LogNumber() || number == logfile_number_;
        break;
      case FileType::kManifest:
        keep = number >= versions_->ManifestFileNumber();
        break;
      case FileType::kTable:
        keep = live.count(number) > 0;
        break;
      case FileType::kTemp:
        keep = live.count(number) > 0;
        break;
      case FileType::kCurrent:
      case FileType::kLock:
        keep = true;
        break;
    }
    if (!keep) {
      if (type == FileType::kTable) table_cache_->Evict(number);
      to_delete.push_back(name);
    }
  }
  lock.unlock();
  for (const auto& name : to_delete) RemoveFile(JoinPath(dbname_, name));
  lock.lock();
}

Status DBImpl::Checkpoint(const std::string& directory) {
  if (FileExists(directory)) return Status::InvalidArgument(directory + ": o destino já existe");
  Status s = Flush();
  if (!s.ok()) return s;
  std::shared_ptr<const Version> version;
  VersionEdit edit;
  uint64_t max_number = 1;
  {
    Lock lock(mu_);
    ++file_deletions_disabled_;
    version = versions_->current();
    versions_->WriteSnapshotEdit(&edit);
    edit.SetLastSequence(versions_->LastSequence());
  }
  s = CreateDirs(directory);
  for (int level = 0; level < kNumLevels && s.ok(); ++level) {
    for (const auto& f : version->files_[level]) {
      max_number = std::max(max_number, f->number);
      s = LinkOrCopyFile(TableFileName(dbname_, f->number), TableFileName(directory, f->number));
      if (!s.ok()) break;
    }
  }
  if (s.ok()) {
    edit.SetLogNumber(0);
    edit.SetNextFile(max_number + 2);
    std::unique_ptr<WritableFile> file;
    uint64_t manifest_number = max_number + 1;
    s = WritableFile::Open(ManifestFileName(directory, manifest_number), true, &file);
    if (s.ok()) {
      wal::Writer log(file.get());
      std::string record;
      edit.EncodeTo(&record);
      s = log.AddRecord(record);
      if (s.ok()) s = file->Sync();
      if (s.ok()) s = file->Close();
    }
    if (s.ok()) s = SetCurrentFile(directory, manifest_number);
  }
  {
    Lock lock(mu_);
    --file_deletions_disabled_;
  }
  if (s.ok()) Log("checkpoint criado em " + directory);
  return s;
}

Status DBImpl::VerifyChecksums(VerifyResult* result) {
  *result = VerifyResult();
  auto start = std::chrono::steady_clock::now();
  std::shared_ptr<const Version> version;
  {
    Lock lock(mu_);
    ++file_deletions_disabled_;
    version = versions_->current();
  }
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& f : version->files_[level]) {
      std::string name = TableFileName(dbname_, f->number);
      ++result->files;
      result->bytes += f->file_size;
      std::unique_ptr<RandomAccessFile> file;
      std::shared_ptr<Table> table;
      Status s = RandomAccessFile::Open(name, &file);
      if (s.ok()) s = Table::Open(options_, std::move(file), nullptr, nullptr, &table);
      if (s.ok()) s = table->VerifyAll();
      if (s.ok()) {
        ++result->ok;
      } else {
        std::string base = name.substr(name.find_last_of("/\\") + 1);
        result->corrupt.emplace_back(base, s.ToString());
      }
    }
  }
  {
    Lock lock(mu_);
    --file_deletions_disabled_;
  }
  result->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return Status::OK();
}

EngineStats DBImpl::GetStats() {
  EngineStats st;
  Lock lock(mu_);
  auto current = versions_->current();
  st.last_sequence = versions_->LastSequence();
  st.memtable_bytes = mem_->ApproximateMemoryUsage();
  st.memtable_entries = mem_->Entries();
  st.immutable_bytes = imm_ ? imm_->ApproximateMemoryUsage() : 0;
  st.wal_number = logfile_number_;
  st.flushes = counters_.flushes.load();
  st.flush_bytes = counters_.flush_bytes.load();
  st.compactions = counters_.compactions.load();
  st.compaction_bytes_read = counters_.compaction_bytes_read.load();
  st.compaction_bytes_written = counters_.compaction_bytes_written.load();
  st.user_bytes_written = counters_.user_bytes_written.load();
  st.write_stalls = counters_.write_stalls.load();
  st.bloom_checks = counters_.bloom_checks.load();
  st.bloom_useful = counters_.bloom_useful.load();
  st.block_cache_capacity = block_cache_->Capacity();
  st.block_cache_hits = block_cache_->Hits();
  st.block_cache_misses = block_cache_->Misses();
  st.snapshots = snapshots_.size();
  st.compacting = compacting_;
  if (!bg_error_.ok()) st.background_error = bg_error_.ToString();
  if (st.user_bytes_written > 0) {
    st.write_amplification =
        static_cast<double>(st.user_bytes_written + st.flush_bytes + st.compaction_bytes_written) /
        static_cast<double>(st.user_bytes_written);
  }
  uint64_t entries = st.memtable_entries + (imm_ ? imm_->Entries() : 0);
  uint64_t deletions = 0;
  for (int level = 0; level < kNumLevels; ++level) {
    LevelInfo info;
    info.level = level;
    info.max_bytes = versions_->MaxBytesForLevel(level);
    info.bytes = current->LevelBytes(level);
    if (level == 0) {
      info.score = static_cast<double>(current->NumFiles(0)) / options_.level0_compaction_trigger;
    } else if (info.max_bytes > 0) {
      info.score = static_cast<double>(info.bytes) / static_cast<double>(info.max_bytes);
    }
    for (const auto& f : current->files_[level]) {
      TableFileInfo tf;
      tf.number = f->number;
      tf.bytes = f->file_size;
      tf.entries = f->entries;
      tf.deletions = f->deletions;
      tf.smallest = std::string(f->smallest.user_key());
      tf.largest = std::string(f->largest.user_key());
      info.files.push_back(std::move(tf));
      entries += f->entries;
      deletions += f->deletions;
      st.sst_bytes += f->file_size;
      ++st.sst_files;
    }
    st.levels.push_back(std::move(info));
  }
  st.estimated_keys = entries > 2 * deletions ? entries - 2 * deletions : 0;
  st.recent_compactions.assign(recent_compactions_.begin(), recent_compactions_.end());
  lock.unlock();
  st.block_cache_bytes = block_cache_->Usage();
  {
    std::lock_guard<std::mutex> g(log_mu_);
    st.wal_bytes = logfile_ ? logfile_->Size() : 0;
  }
  return st;
}

}

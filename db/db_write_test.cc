//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <atomic>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include "db/db_test_util.h"
#include "db/write_batch_internal.h"
#include "db/write_thread.h"
#include "port/port.h"
#include "port/stack_trace.h"
#include "test_util/sync_point.h"
#include "util/random.h"
#include "util/string_util.h"
#include "utilities/fault_injection_env.h"

namespace ROCKSDB_NAMESPACE {

// Test variations of WriteImpl.
class DBWriteTest : public DBTestBase, public testing::WithParamInterface<int> {
 public:
  DBWriteTest() : DBTestBase("db_write_test", /*env_do_fsync=*/true) {}

  Options GetOptions() { return DBTestBase::GetOptions(GetParam()); }

  void Open() { DBTestBase::Reopen(GetOptions()); }
};

// It is invalid to do sync write while disabling WAL.
TEST_P(DBWriteTest, SyncAndDisableWAL) {
  WriteOptions write_options;
  write_options.sync = true;
  write_options.disableWAL = true;
  ASSERT_TRUE(dbfull()->Put(write_options, "foo", "bar").IsInvalidArgument());
  WriteBatch batch;
  ASSERT_OK(batch.Put("foo", "bar"));
  ASSERT_TRUE(dbfull()->Write(write_options, &batch).IsInvalidArgument());
}

TEST_P(DBWriteTest, WriteStallRemoveNoSlowdownWrite) {
  Options options = GetOptions();
  options.level0_stop_writes_trigger = options.level0_slowdown_writes_trigger =
      4;
  std::vector<port::Thread> threads;
  std::atomic<int> thread_num(0);
  port::Mutex mutex;
  port::CondVar cv(&mutex);
  // Guarded by mutex
  int writers = 0;

  Reopen(options);

  std::function<void()> write_slowdown_func = [&]() {
    int a = thread_num.fetch_add(1);
    std::string key = "foo" + std::to_string(a);
    WriteOptions wo;
    wo.no_slowdown = false;
    ASSERT_OK(dbfull()->Put(wo, key, "bar"));
  };
  std::function<void()> write_no_slowdown_func = [&]() {
    int a = thread_num.fetch_add(1);
    std::string key = "foo" + std::to_string(a);
    WriteOptions wo;
    wo.no_slowdown = true;
    Status s = dbfull()->Put(wo, key, "bar");
    ASSERT_TRUE(s.ok() || s.IsIncomplete());
  };
  std::function<void(void*)> unblock_main_thread_func = [&](void*) {
    mutex.Lock();
    ++writers;
    cv.SignalAll();
    mutex.Unlock();
  };

  // Create 3 L0 files and schedule 4th without waiting
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->SetCallBack(
      "WriteThread::JoinBatchGroup:Start", unblock_main_thread_func);
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->LoadDependency(
      {{"DBWriteTest::WriteStallRemoveNoSlowdownWrite:1",
        "DBImpl::BackgroundCallFlush:start"},
       {"DBWriteTest::WriteStallRemoveNoSlowdownWrite:2",
        "DBImplWrite::PipelinedWriteImpl:AfterJoinBatchGroup"},
       // Make compaction start wait for the write stall to be detected and
       // implemented by a write group leader
       {"DBWriteTest::WriteStallRemoveNoSlowdownWrite:3",
        "BackgroundCallCompaction:0"}});
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  // Schedule creation of 4th L0 file without waiting. This will seal the
  // memtable and then wait for a sync point before writing the file. We need
  // to do it this way because SwitchMemtable() needs to enter the
  // write_thread
  FlushOptions fopt;
  fopt.wait = false;
  ASSERT_OK(dbfull()->Flush(fopt));

  // Create a mix of slowdown/no_slowdown write threads
  mutex.Lock();
  // First leader
  threads.emplace_back(write_slowdown_func);
  while (writers != 1) {
    cv.Wait();
  }

  // Second leader. Will stall writes
  // Build a writers list with no slowdown in the middle:
  //  +-------------+
  //  | slowdown    +<----+ newest
  //  +--+----------+
  //     |
  //     v
  //  +--+----------+
  //  | no slowdown |
  //  +--+----------+
  //     |
  //     v
  //  +--+----------+
  //  | slowdown    +
  //  +-------------+
  threads.emplace_back(write_slowdown_func);
  while (writers != 2) {
    cv.Wait();
  }
  threads.emplace_back(write_no_slowdown_func);
  while (writers != 3) {
    cv.Wait();
  }
  threads.emplace_back(write_slowdown_func);
  while (writers != 4) {
    cv.Wait();
  }

  mutex.Unlock();

  TEST_SYNC_POINT("DBWriteTest::WriteStallRemoveNoSlowdownWrite:1");
  ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable(nullptr));
  // This would have triggered a write stall. Unblock the write group leader
  TEST_SYNC_POINT("DBWriteTest::WriteStallRemoveNoSlowdownWrite:2");
  // The leader is going to create missing newer links. When the leader
  // finishes, the next leader is going to delay writes and fail writers with
  // no_slowdown

  TEST_SYNC_POINT("DBWriteTest::WriteStallRemoveNoSlowdownWrite:3");
  for (auto& t : threads) {
    t.join();
  }

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->ClearAllCallBacks();
}

TEST_P(DBWriteTest, WriteThreadHangOnWriteStall) {
  Options options = GetOptions();
  options.level0_stop_writes_trigger = options.level0_slowdown_writes_trigger = 4;
  std::vector<port::Thread> threads;
  std::atomic<int> thread_num(0);
  port::Mutex mutex;
  port::CondVar cv(&mutex);
  // Guarded by mutex
  int writers = 0;

  Reopen(options);

  std::function<void()> write_slowdown_func = [&]() {
    int a = thread_num.fetch_add(1);
    std::string key = "foo" + std::to_string(a);
    WriteOptions wo;
    wo.no_slowdown = false;
    ASSERT_OK(dbfull()->Put(wo, key, "bar"));
  };
  std::function<void()> write_no_slowdown_func = [&]() {
    int a = thread_num.fetch_add(1);
    std::string key = "foo" + std::to_string(a);
    WriteOptions wo;
    wo.no_slowdown = true;
    Status s = dbfull()->Put(wo, key, "bar");
    ASSERT_TRUE(s.ok() || s.IsIncomplete());
  };
  std::function<void(void *)> unblock_main_thread_func = [&](void *) {
    mutex.Lock();
    ++writers;
    cv.SignalAll();
    mutex.Unlock();
  };

  // Create 3 L0 files and schedule 4th without waiting
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));
  ASSERT_OK(Flush());
  ASSERT_OK(Put("foo" + std::to_string(thread_num.fetch_add(1)), "bar"));

  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->SetCallBack(
      "WriteThread::JoinBatchGroup:Start", unblock_main_thread_func);
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->LoadDependency(
      {{"DBWriteTest::WriteThreadHangOnWriteStall:1",
        "DBImpl::BackgroundCallFlush:start"},
       {"DBWriteTest::WriteThreadHangOnWriteStall:2",
        "DBImpl::WriteImpl:BeforeLeaderEnters"},
       // Make compaction start wait for the write stall to be detected and
       // implemented by a write group leader
       {"DBWriteTest::WriteThreadHangOnWriteStall:3",
        "BackgroundCallCompaction:0"}});
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  // Schedule creation of 4th L0 file without waiting. This will seal the
  // memtable and then wait for a sync point before writing the file. We need
  // to do it this way because SwitchMemtable() needs to enter the
  // write_thread
  FlushOptions fopt;
  fopt.wait = false;
  ASSERT_OK(dbfull()->Flush(fopt));

  // Create a mix of slowdown/no_slowdown write threads
  mutex.Lock();
  // First leader
  threads.emplace_back(write_slowdown_func);
  while (writers != 1) {
    cv.Wait();
  }
  // Second leader. Will stall writes
  threads.emplace_back(write_slowdown_func);
  threads.emplace_back(write_no_slowdown_func);
  threads.emplace_back(write_slowdown_func);
  threads.emplace_back(write_no_slowdown_func);
  threads.emplace_back(write_slowdown_func);
  while (writers != 6) {
    cv.Wait();
  }
  mutex.Unlock();

  TEST_SYNC_POINT("DBWriteTest::WriteThreadHangOnWriteStall:1");
  ASSERT_OK(dbfull()->TEST_WaitForFlushMemTable(nullptr));
  // This would have triggered a write stall. Unblock the write group leader
  TEST_SYNC_POINT("DBWriteTest::WriteThreadHangOnWriteStall:2");
  // The leader is going to create missing newer links. When the leader finishes,
  // the next leader is going to delay writes and fail writers with no_slowdown

  TEST_SYNC_POINT("DBWriteTest::WriteThreadHangOnWriteStall:3");
  for (auto& t : threads) {
    t.join();
  }
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();
  ROCKSDB_NAMESPACE::SyncPoint::GetInstance()->ClearAllCallBacks();
}

TEST_P(DBWriteTest, IOErrorOnWALWritePropagateToWriteThreadFollower) {
  constexpr int kNumThreads = 5;
  std::unique_ptr<FaultInjectionTestEnv> mock_env(
      new FaultInjectionTestEnv(env_));
  Options options = GetOptions();
  options.env = mock_env.get();
  Reopen(options);
  std::atomic<int> ready_count{0};
  std::atomic<int> leader_count{0};
  std::vector<port::Thread> threads;
  mock_env->SetFilesystemActive(false);

  // Wait until all threads linked to write threads, to make sure
  // all threads join the same batch group.
  SyncPoint::GetInstance()->SetCallBack(
      "WriteThread::JoinBatchGroup:Wait", [&](void* arg) {
        ready_count++;
        auto* w = reinterpret_cast<WriteThread::Writer*>(arg);
        if (w->state == WriteThread::STATE_GROUP_LEADER) {
          leader_count++;
          while (ready_count < kNumThreads) {
            // busy waiting
          }
        }
      });
  SyncPoint::GetInstance()->EnableProcessing();
  for (int i = 0; i < kNumThreads; i++) {
    threads.push_back(port::Thread(
        [&](int index) {
          // All threads should fail.
          auto res = Put("key" + std::to_string(index), "value");
          if (options.manual_wal_flush) {
            ASSERT_TRUE(res.ok());
            // we should see fs error when we do the flush

            // TSAN reports a false alarm for lock-order-inversion but Open and
            // FlushWAL are not run concurrently. Disabling this until TSAN is
            // fixed.
            // res = dbfull()->FlushWAL(false);
            // ASSERT_FALSE(res.ok());
          } else {
            ASSERT_FALSE(res.ok());
          }
        },
        i));
  }
  for (int i = 0; i < kNumThreads; i++) {
    threads[i].join();
  }
  ASSERT_EQ(1, leader_count);

  // The Failed PUT operations can cause a BG error to be set.
  // Mark it as Checked for the ASSERT_STATUS_CHECKED
  dbfull()->Resume().PermitUncheckedError();

  // Close before mock_env destruct.
  Close();
}

TEST_P(DBWriteTest, ManualWalFlushInEffect) {
  Options options = GetOptions();
  Reopen(options);
  // try the 1st WAL created during open
  ASSERT_TRUE(Put("key" + std::to_string(0), "value").ok());
  ASSERT_TRUE(options.manual_wal_flush != dbfull()->TEST_WALBufferIsEmpty());
  ASSERT_TRUE(dbfull()->FlushWAL(false).ok());
  ASSERT_TRUE(dbfull()->TEST_WALBufferIsEmpty());
  // try the 2nd wal created during SwitchWAL
  ASSERT_OK(dbfull()->TEST_SwitchWAL());
  ASSERT_TRUE(Put("key" + std::to_string(0), "value").ok());
  ASSERT_TRUE(options.manual_wal_flush != dbfull()->TEST_WALBufferIsEmpty());
  ASSERT_TRUE(dbfull()->FlushWAL(false).ok());
  ASSERT_TRUE(dbfull()->TEST_WALBufferIsEmpty());
}

TEST_P(DBWriteTest, IOErrorOnWALWriteTriggersReadOnlyMode) {
  std::unique_ptr<FaultInjectionTestEnv> mock_env(
      new FaultInjectionTestEnv(env_));
  Options options = GetOptions();
  options.env = mock_env.get();
  Reopen(options);
  for (int i = 0; i < 2; i++) {
    // Forcibly fail WAL write for the first Put only. Subsequent Puts should
    // fail due to read-only mode
    mock_env->SetFilesystemActive(i != 0);
    auto res = Put("key" + std::to_string(i), "value");
    // TSAN reports a false alarm for lock-order-inversion but Open and
    // FlushWAL are not run concurrently. Disabling this until TSAN is
    // fixed.
    /*
    if (options.manual_wal_flush && i == 0) {
      // even with manual_wal_flush the 2nd Put should return error because of
      // the read-only mode
      ASSERT_TRUE(res.ok());
      // we should see fs error when we do the flush
      res = dbfull()->FlushWAL(false);
    }
    */
    if (!options.manual_wal_flush) {
      ASSERT_NOK(res);
    } else {
      ASSERT_OK(res);
    }
  }
  // Close before mock_env destruct.
  Close();
}

TEST_P(DBWriteTest, IOErrorOnSwitchMemtable) {
  Random rnd(301);
  std::unique_ptr<FaultInjectionTestEnv> mock_env(
      new FaultInjectionTestEnv(env_));
  Options options = GetOptions();
  options.env = mock_env.get();
  options.writable_file_max_buffer_size = 4 * 1024 * 1024;
  options.write_buffer_size = 3 * 512 * 1024;
  options.wal_bytes_per_sync = 256 * 1024;
  options.manual_wal_flush = true;
  Reopen(options);
  mock_env->SetFilesystemActive(false, Status::IOError("Not active"));
  Status s;
  for (int i = 0; i < 4 * 512; ++i) {
    s = Put(Key(i), rnd.RandomString(1024));
    if (!s.ok()) {
      break;
    }
  }
  ASSERT_EQ(s.severity(), Status::Severity::kFatalError);

  mock_env->SetFilesystemActive(true);
  // Close before mock_env destruct.
  Close();
}

// Test that db->LockWAL() flushes the WAL after locking.
TEST_P(DBWriteTest, LockWalInEffect) {
  Options options = GetOptions();
  Reopen(options);
  // try the 1st WAL created during open
  ASSERT_OK(Put("key" + std::to_string(0), "value"));
  ASSERT_TRUE(options.manual_wal_flush != dbfull()->TEST_WALBufferIsEmpty());
  ASSERT_OK(dbfull()->LockWAL());
  ASSERT_TRUE(dbfull()->TEST_WALBufferIsEmpty(false));
  ASSERT_OK(dbfull()->UnlockWAL());
  // try the 2nd wal created during SwitchWAL
  ASSERT_OK(dbfull()->TEST_SwitchWAL());
  ASSERT_OK(Put("key" + std::to_string(0), "value"));
  ASSERT_TRUE(options.manual_wal_flush != dbfull()->TEST_WALBufferIsEmpty());
  ASSERT_OK(dbfull()->LockWAL());
  ASSERT_TRUE(dbfull()->TEST_WALBufferIsEmpty(false));
  ASSERT_OK(dbfull()->UnlockWAL());
}

TEST_P(DBWriteTest, ConcurrentlyDisabledWAL) {
    Options options = GetOptions();
    options.statistics = ROCKSDB_NAMESPACE::CreateDBStatistics();
    options.statistics->set_stats_level(StatsLevel::kAll);
    Reopen(options);
    std::string wal_key_prefix = "WAL_KEY_";
    std::string no_wal_key_prefix = "K_";
    // 100 KB value each for NO-WAL operation
    std::string no_wal_value(1024 * 100, 'X');
    // 1B value each for WAL operation
    std::string wal_value = "0";
    std::thread threads[10];
    for (int t = 0; t < 10; t++) {
        threads[t] = std::thread([t, wal_key_prefix, wal_value, no_wal_key_prefix, no_wal_value, this] {
            for(int i = 0; i < 10; i++) {
              ROCKSDB_NAMESPACE::WriteOptions write_option_disable;
              write_option_disable.disableWAL = true;
              ROCKSDB_NAMESPACE::WriteOptions write_option_default;
              std::string no_wal_key = no_wal_key_prefix + std::to_string(t) +
                                       "_" + std::to_string(i);
              ASSERT_OK(
                  this->Put(no_wal_key, no_wal_value, write_option_disable));
              std::string wal_key =
                  wal_key_prefix + std::to_string(i) + "_" + std::to_string(i);
              ASSERT_OK(this->Put(wal_key, wal_value, write_option_default));
              ASSERT_OK(dbfull()->SyncWAL());
            }
            return;
        });
    }
    for (auto& t: threads) {
        t.join();
    }
    uint64_t bytes_num = options.statistics->getTickerCount(
        ROCKSDB_NAMESPACE::Tickers::WAL_FILE_BYTES);
    // written WAL size should less than 100KB (even included HEADER & FOOTER overhead)
    ASSERT_LE(bytes_num, 1024 * 100);
}

TEST_P(DBWriteTest, DisableWriteStall) {
  Options options = GetOptions();
  options.disable_write_stall = true;
  options.max_write_buffer_number = 2;
  options.use_options_file = false;
  Reopen(options);
  db_->PauseBackgroundWork();
  ASSERT_OK(Put("k1", "v1"));
  FlushOptions opts;
  opts.wait = false;
  opts.allow_write_stall = true;
  ASSERT_OK(db_->Flush(opts));
  ASSERT_OK(Put("k2", "v2"));
  ASSERT_OK(db_->Flush(opts));

  // no write stall since it's disabled
  ASSERT_OK(Put("k3", "v3"));

  // now enable write stall
  ASSERT_OK(db_->SetOptions({{"disable_write_stall", "false"}}));

  WriteOptions wopts;
  wopts.no_slowdown = true;
  auto st = db_->Put(wopts, "k4", "v4");
  EXPECT_TRUE(st.IsIncomplete());

  // now disable again
  ASSERT_OK(db_->SetOptions({{"disable_write_stall", "true"}}));
  // no write stall since it's disabled
  ASSERT_OK(Put("k4", "v4"));

  // verify that disable write stall will unblock writes
  ASSERT_OK(db_->SetOptions({{"disable_write_stall", "false"}}));

  std::thread t([&]() {
    // writes will be blocked due to write stall
    // but once we disable write stall, the writes are unblocked
    ASSERT_OK(Put("k5", "v5"));
  });
  // sleep to make sure t is blocked on write. Not ideal but it works
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ASSERT_OK(db_->SetOptions({{"disable_write_stall", "true"}}));
  t.join();

  Close();
}

class DummyListener : public ReplicationLogListener {
   public:
    std::string OnReplicationLogRecord(
        ReplicationLogRecord /*record*/) override {
        seq_ += 1;
        return std::to_string(seq_);
    }

   private:
    std::atomic_int seq_{0};
};

// verifies that when `disable_write_stall` is the only cf option we set,
// there won't be manifest updates
TEST_P(DBWriteTest, DisableWriteStallNotWriteManifest) {
  // pipelined write is conflicted with atomic flush
  if (GetParam() == kPipelinedWrite) {
    return ;
  }
  Options options = GetOptions();
  options.disable_write_stall = false;
  // make sure manifest update seq is bumped
  options.replication_log_listener = std::make_shared<DummyListener>();
  options.atomic_flush = true;
  Reopen(options);

  uint64_t manifestUpdateSeq;
  ASSERT_OK(db_->GetManifestUpdateSequence(&manifestUpdateSeq));

  db_->SetOptions({{"disable_write_stall", "true"}});

  uint64_t newManifestUpdateSeq;
  ASSERT_OK(db_->GetManifestUpdateSequence(&newManifestUpdateSeq));

  EXPECT_EQ(manifestUpdateSeq, newManifestUpdateSeq);

  Close();
}

void functionTrampoline(void* arg) {
    (*reinterpret_cast<std::function<void()>*>(arg))();
}

// Test the case that non-trival compaction is triggered before we disable write stall
// and make sure compaction job with old mutable_cf_options won't cause write stall
TEST_P(DBWriteTest, AutoCompactionBeforeDisableWriteStall) {
  const int kNumKeysPerFile = 100;

  Options options;
  options.env = env_;
  options.use_options_file = false;

  // auto flush/compaction enabled so that write stall will be triggered
  options.disable_auto_compactions = false;
  options.disable_auto_flush = false;

  // set write buffer number to trigger write stall
  options.max_write_buffer_number = 2;
  options.disable_write_stall = false;

  // set compaction trigger to trigger non trival auto compaction
  options.num_levels = 3;
  options.level0_file_num_compaction_trigger = 3;

  // large write buffer size so auto flush never triggered
  options.write_buffer_size = 10 << 20;

  options.max_background_jobs = 2;

  options.info_log = info_log_;
  CreateAndReopenWithCF({"pikachu"}, options);

  auto cfd = static_cast_with_check<ColumnFamilyHandleImpl>(handles_[1])->cfd();

  Random rnd(301);

  for (int num = 0; num < options.level0_file_num_compaction_trigger - 1;
       num++) {
    std::vector<std::string> values;
    // Write 100KB (100 values, each 1K)
    for (int i = 0; i < kNumKeysPerFile; i++) {
      values.push_back(rnd.RandomString(990));
      ASSERT_OK(Put(1, Key(i), values[i]));
    }
    ASSERT_OK(dbfull()->Flush({}, handles_[1]));
    ASSERT_EQ(NumTableFilesAtLevel(0, 1), num + 1);
  }

  // We are trying to simulate following case:
  // 1. non trival compaction job scheduled but not starting yet
  // 2. continuous writes trigger flush, which generates too many memtables and
  // stalls writes
  // 3. disable write stall through setOption API
  // 4. compaction job is done. Even though it installs super version with stale
  // `mutable_cf_options`, which still has `disable_write_stall=false`, the
  // writes are not stalled since latest `mutable_cf_options` has
  // `disable_write_stall=true`
  // 5. flush jobs are done
  SyncPoint::GetInstance()->LoadDependency(
      {{"DBImpl::BackgroundCompaction:NonTrivial:BeforeRun",
        "DBWriteTest::CompactionBeforeDisableWriteStall:BeforeDisableWriteStall"},
        {
          "DBWriteTest::CompactionBeforeDisableWriteStall:AfterDisableWriteStall",
          "CompactionJob::Run():Start"
        }});
  SyncPoint::GetInstance()->EnableProcessing();

  // Write one more file to trigger auto compaction
  std::vector<std::string> values;
  for (int i = 0; i < kNumKeysPerFile; i++) {
    values.push_back(rnd.RandomString(990));
    ASSERT_OK(Put(1, Key(i), values[i]));
  }
  ASSERT_OK(dbfull()->Flush({}, handles_[1]));

  TEST_SYNC_POINT("DBWriteTest::CompactionBeforeDisableWriteStall:BeforeDisableWriteStall");
  // writes not stalled yet
  EXPECT_FALSE(cfd->GetSuperVersion()->mutable_cf_options.disable_write_stall);
  EXPECT_FALSE(dbfull()
                   ->GetVersionSet()
                   ->GetColumnFamilySet()
                   ->write_controller()
                   ->IsStopped());

  auto cork = std::make_shared<std::atomic<bool>>();
  cork->store(true);
  std::function<void()> corkFunction = [cork]() {
    while (cork->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  };

  // schedule high priority jobs to block the flush from finishing.
  // We can't `pauseBackgroundWork` here since that would prevent the compaction
  // from finishing as well
  for (int i = 0; i < 2; i++) {
    env_->Schedule(&functionTrampoline, &corkFunction, rocksdb::Env::Priority::HIGH);
  }

  ASSERT_OK(Put(1, "k1", "v1"));
  FlushOptions fopts;
  fopts.wait = false;
  fopts.allow_write_stall = true;
  ASSERT_OK(dbfull()->Flush(fopts, handles_[1]));
  ASSERT_OK(Put(1, "k2", "v2"));
  // write stall condition triggered after this flush
  ASSERT_OK(dbfull()->Flush(fopts, handles_[1]));
  EXPECT_EQ(cfd->imm()->NumNotFlushed(), 2);
  EXPECT_TRUE(dbfull()
                  ->GetVersionSet()
                  ->GetColumnFamilySet()
                  ->write_controller()
                  ->IsStopped());

  ASSERT_OK(db_->SetOptions(
    handles_[1], {{"disable_write_stall", "true"}}));

  TEST_SYNC_POINT("DBWriteTest::CompactionBeforeDisableWriteStall:AfterDisableWriteStall");

  ASSERT_OK(dbfull()->TEST_WaitForScheduledCompaction());
  // compaction job installs super version with stale mutable_cf_options
  EXPECT_FALSE(cfd->GetSuperVersion()->mutable_cf_options.disable_write_stall);
  // but latest mutable_cf_options should be correctly set
  EXPECT_TRUE(cfd->GetLatestMutableCFOptions()->disable_write_stall);
  // and writes are not stalled!
  EXPECT_FALSE(dbfull()->GetVersionSet()->GetColumnFamilySet()->write_controller()->IsStopped());

  WriteOptions wopts;
  wopts.no_slowdown = true;
  EXPECT_OK(db_->Put(wopts, handles_[1], "k3", "v3"));

  cork->store(false);
  // wait for flush to be done
  ASSERT_OK(dbfull()->TEST_WaitForBackgroundWork());

  Close();
}

INSTANTIATE_TEST_CASE_P(DBWriteTestInstance, DBWriteTest,
                        testing::Values(DBTestBase::kDefault,
                                        DBTestBase::kConcurrentWALWrites,
                                        DBTestBase::kPipelinedWrite));

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  RegisterCustomObjects(argc, argv);
  return RUN_ALL_TESTS();
}

#include "TestSupport.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>
#include <filesystem>
#include <string>
#include <vector>

#include "persistence/PersistedRecordReader.h"
#include "persistence/PersistedRecordWriteQueue.h"
#include "persistence/PersistedRecordWriter.h"
#include "workspace/persistence/PersistenceService.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"

namespace microide::tests {
namespace {

using microide::persistence::PersistedRecordReader;
using microide::persistence::PersistedRecordWriteQueue;
using microide::persistence::PersistedRecordWriter;

std::vector<std::byte> Body(std::string_view text) {
  std::vector<std::byte> body;
  body.reserve(text.size());
  for (const char c : text) {
    body.push_back(static_cast<std::byte>(c));
  }
  return body;
}

std::string BodyText(const std::vector<std::byte>& body) {
  std::string out;
  out.reserve(body.size());
  for (const std::byte b : body) {
    out.push_back(static_cast<char>(b));
  }
  return out;
}

std::string ReadRecordText(const std::filesystem::path& path) {
  const auto record = PersistedRecordReader::ReadFile(path);
  return record.has_value() ? BodyText(record->body) : std::string{};
}

void TestEncodedWriteLandsCoalescesAndDropsAFailedEncode() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "session.record";
  PersistedRecordWriteQueue queue;
  queue.QueueEncoded(path, [](std::vector<std::byte>* body) { *body = Body("encoded there"); return true; }, 1u);
  queue.Flush();
  Expect(ReadRecordText(path) == "encoded there", "an encoded body lands: " + ReadRecordText(path));
  queue.Queue(path, Body("plain"), 1u);
  queue.QueueEncoded(path, [](std::vector<std::byte>* body) { *body = Body("latest"); return true; }, 1u);
  queue.Flush();
  Expect(ReadRecordText(path) == "latest", "the newest of the two wins, whichever kind: " + ReadRecordText(path));
  queue.QueueEncoded(path, [](std::vector<std::byte>*) { return false; }, 1u);
  queue.Flush();
  Expect(ReadRecordText(path) == "latest", "a failed encode leaves the record as it was");
}

void TestSettledWaitsForEveryQueuedJobIncludingCoalescedOnes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "session.record";
  PersistedRecordWriteQueue queue;
  Expect(queue.settled(), "an idle queue is settled");
  std::mutex gate;
  gate.lock();
  std::atomic<bool> encoding{false};
  queue.QueueEncoded(path, [&](std::vector<std::byte>* body) {
    encoding = true;
    const std::lock_guard hold(gate);
    *body = Body("first");
    return true;
  }, 1u);
  while (!encoding) {
    std::this_thread::yield();
  }
  queue.Queue(path, Body("second"), 1u);
  queue.Queue(path, Body("third"), 1u);
  Expect(!queue.settled(), "a job mid-encode, and two queued behind it, are not settled");
  gate.unlock();
  queue.Flush();
  Expect(queue.settled(), "after the flush every job, the coalesced one too, has finished");
  Expect(ReadRecordText(path) == "third", "and the newest body is what landed: " + ReadRecordText(path));
  queue.QueueEncoded(path, [](std::vector<std::byte>*) { return false; }, 1u);
  queue.Flush();
  Expect(queue.settled(), "a dropped encode still finishes its job");
}

void TestQueuedWriteLandsAfterFlush() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state.record";
  PersistedRecordWriteQueue queue;

  queue.Queue(path, Body("hello"), 1u);
  queue.Flush();

  Expect(std::filesystem::exists(path), "a flushed write must exist on disk");
  Expect(ReadRecordText(path) == "hello", "the record must hold exactly what was queued");
}

void TestCoalescedWritesKeepTheLatestBody() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state.record";
  PersistedRecordWriteQueue queue;

  for (int i = 0; i < 50; ++i) {
    queue.Queue(path, Body("v" + std::to_string(i)), 1u);
  }
  queue.Flush();

  Expect(ReadRecordText(path) == "v49",
         "coalescing must persist the LAST accepted body, never an earlier one");
  Expect(queue.applied_write_count() <= 50,
         "coalescing must not write more times than it was asked to");
}

void TestDestructorFlushesPendingWrites() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state.record";
  {
    PersistedRecordWriteQueue queue;
    queue.Queue(path, Body("last-write-before-exit"), 1u);
  }
  Expect(std::filesystem::exists(path), "destruction must land a still-queued write");
  Expect(ReadRecordText(path) == "last-write-before-exit",
         "the write the destructor landed must be the queued one");
}

void TestDeleteAndWriteResolveInSubmissionOrder() {
  TemporaryDirectory temp_dir;
  PersistedRecordWriteQueue queue;

  const std::filesystem::path deleted = temp_dir.path() / "deleted.record";
  queue.Queue(deleted, Body("doomed"), 1u);
  queue.QueueDelete(deleted);
  queue.Flush();
  Expect(!std::filesystem::exists(deleted), "a delete queued after a write must win");
  Expect(!std::filesystem::exists(PersistedRecordWriter::BackupPathFor(deleted)),
         "the delete must take the backup too, or the reader resurrects the state");

  const std::filesystem::path revived = temp_dir.path() / "revived.record";
  queue.Queue(revived, Body("first"), 1u);
  queue.Flush();
  queue.QueueDelete(revived);
  queue.Queue(revived, Body("second"), 1u);
  queue.Flush();
  Expect(std::filesystem::exists(revived), "a write queued after a delete must win");
  Expect(ReadRecordText(revived) == "second", "the surviving record must be the later write");
}

void TestMemoNeverSkipsAWriteThatIsNotOnDisk() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state.record";
  PersistedRecordWriteQueue queue;

  queue.Queue(path, Body("same"), 1u);
  queue.Flush();
  const std::size_t after_first = queue.applied_write_count();

  queue.Queue(path, Body("same"), 1u);
  queue.Flush();
  Expect(queue.applied_write_count() == after_first,
         "an identical body over an existing record should skip the rewrite");

  std::error_code error;
  std::filesystem::remove(path, error);
  std::filesystem::remove(PersistedRecordWriter::BackupPathFor(path), error);
  queue.Queue(path, Body("same"), 1u);
  queue.Flush();
  Expect(std::filesystem::exists(path),
         "the memo must never skip a write against a record that no longer exists");
  Expect(ReadRecordText(path) == "same", "the recreated record must hold the queued body");
}

void TestFailedWriteIsRetriedOnTheNextSave() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path blocker = temp_dir.path() / "blocked";
  WriteFile(blocker, "not a directory");
  const std::filesystem::path missing_path = blocker / "state.record";

  PersistedRecordWriteQueue queue;
  queue.Queue(missing_path, Body("payload"), 1u);
  queue.Flush();
  Expect(queue.applied_write_count() == 0, "the blocked write should not report success");
  Expect(!std::filesystem::exists(missing_path), "nothing should have been written");

  std::error_code error;
  std::filesystem::remove(blocker, error);
  Expect(!error, "the blocking file should be removable");
  queue.Queue(missing_path, Body("payload"), 1u);
  queue.Flush();
  Expect(std::filesystem::exists(missing_path) && !std::filesystem::is_directory(missing_path),
         "a save that failed with the record MISSING must be retried");
  Expect(ReadRecordText(missing_path) == "payload", "the retried write must hold the queued body");

  const std::filesystem::path dir = temp_dir.path() / "locked";
  std::filesystem::create_directories(dir);
  const std::filesystem::path path = dir / "state.record";
  queue.Queue(path, Body("A"), 1u);
  queue.Flush();
  Expect(ReadRecordText(path) == "A", "the first body should land");

  std::filesystem::permissions(dir, std::filesystem::perms::owner_read |
                                        std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::replace, error);
  const std::size_t before_blocked = queue.applied_write_count();
  queue.Queue(path, Body("B"), 1u);
  queue.Flush();
  const bool write_was_blocked = queue.applied_write_count() == before_blocked;
  std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, error);

  if (!write_was_blocked) {
    Expect(ReadRecordText(path) == "B",
           "unblocked (running with permission overrides): the write should have landed");
  } else {
    Expect(ReadRecordText(path) == "A", "the failed write must leave the previous body intact");
    queue.Queue(path, Body("B"), 1u);
    queue.Flush();
    Expect(ReadRecordText(path) == "B",
           "a save that failed while the record STILL EXISTED must be retried -- remembering it "
           "as persisted would lose that state for good");
  }
}

void TestManyRecordsAllLandWithTheirOwnBodies() {
  TemporaryDirectory temp_dir;
  PersistedRecordWriteQueue queue;
  constexpr int kRecords = 12;
  constexpr int kRounds = 8;

  for (int round = 0; round < kRounds; ++round) {
    for (int record = 0; record < kRecords; ++record) {
      queue.Queue(temp_dir.path() / ("r" + std::to_string(record) + ".record"),
                  Body("record" + std::to_string(record) + "-round" + std::to_string(round)), 1u);
    }
    if (round % 3 == 0) {
      queue.Flush();
    }
  }
  queue.Flush();

  for (int record = 0; record < kRecords; ++record) {
    const std::filesystem::path path =
        temp_dir.path() / ("r" + std::to_string(record) + ".record");
    Expect(ReadRecordText(path) ==
               "record" + std::to_string(record) + "-round" + std::to_string(kRounds - 1),
           "every record must end at its own final body");
  }
}

void TestServiceLoadSeesAnUnflushedSave() {
  using microide::workspace::PersistedUserConfigState;
  using microide::workspace::PersistenceService;

  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "user.config";
  PersistenceService service;

  const PersistedUserConfigState saved{.ui_scale = 1.5f, .settings = {{"theme", "async"}}};
  Expect(service.SaveUserConfig(path, saved), "save should be accepted");

  PersistedUserConfigState loaded;
  Expect(service.LoadUserConfig(path, &loaded), "load should succeed against a pending write");
  Expect(loaded.settings.size() == 1 && loaded.settings[0].second == "async",
         "a load must never overtake a save that was already accepted");
}

}

void RegisterPersistedRecordWriteQueueTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PersistedRecordWriteQueue/EncodedWriteLandsCoalescesAndDropsAFailedEncode",
          TestEncodedWriteLandsCoalescesAndDropsAFailedEncode);
  AddTest(tests, "PersistedRecordWriteQueue/SettledWaitsForEveryQueuedJobIncludingCoalescedOnes",
          TestSettledWaitsForEveryQueuedJobIncludingCoalescedOnes);
  AddTest(tests, "PersistedRecordWriteQueue/QueuedWriteLandsAfterFlush",
          TestQueuedWriteLandsAfterFlush);
  AddTest(tests, "PersistedRecordWriteQueue/CoalescedWritesKeepTheLatestBody",
          TestCoalescedWritesKeepTheLatestBody);
  AddTest(tests, "PersistedRecordWriteQueue/DestructorFlushesPendingWrites",
          TestDestructorFlushesPendingWrites);
  AddTest(tests, "PersistedRecordWriteQueue/DeleteAndWriteResolveInSubmissionOrder",
          TestDeleteAndWriteResolveInSubmissionOrder);
  AddTest(tests, "PersistedRecordWriteQueue/MemoNeverSkipsAWriteThatIsNotOnDisk",
          TestMemoNeverSkipsAWriteThatIsNotOnDisk);
  AddTest(tests, "PersistedRecordWriteQueue/FailedWriteIsRetriedOnTheNextSave",
          TestFailedWriteIsRetriedOnTheNextSave);
  AddTest(tests, "PersistedRecordWriteQueue/ManyRecordsAllLandWithTheirOwnBodies",
          TestManyRecordsAllLandWithTheirOwnBodies);
  AddTest(tests, "PersistedRecordWriteQueue/ServiceLoadSeesAnUnflushedSave",
          TestServiceLoadSeesAnUnflushedSave);
}

}

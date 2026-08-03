#include "TestSupport.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "persistence/PersistedRecordReader.h"
#include "persistence/PersistedRecordWriteQueue.h"
#include "persistence/PersistedRecordWriter.h"
#include "workspace/PersistenceService.h"
#include "workspace/WorkspacePersistenceFormat.h"

// Persisted workspace state (session, config, recents) is written on a background
// worker so saving never stalls the shell. That trades a synchronous guarantee
// for a queued one, and the whole point of these tests is that the trade never
// costs data: whatever the last accepted save said must be what ends up on disk,
// through coalescing, through deletes, through teardown, and through a failed
// write.
//
// Note what is NOT in scope here: the user's own files. Document saves go through
// util::WriteFileBytesDurable on the calling thread and are unchanged -- see
// TextFileIO. This queue only ever touches the state record store.

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

// Reads the record back, following the reader's own primary/backup fallback.
std::string ReadRecordText(const std::filesystem::path& path) {
  const auto record = PersistedRecordReader::ReadFile(path);
  return record.has_value() ? BodyText(record->body) : std::string{};
}

// The core promise: an accepted save reaches disk.
void TestQueuedWriteLandsAfterFlush() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state.record";
  PersistedRecordWriteQueue queue;

  queue.Queue(path, Body("hello"), 1u);
  queue.Flush();

  Expect(std::filesystem::exists(path), "a flushed write must exist on disk");
  Expect(ReadRecordText(path) == "hello", "the record must hold exactly what was queued");
}

// Coalescing must keep the NEWEST body. Dropping the newest instead of the oldest
// would silently persist stale state -- the exact data loss this design risks.
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

// Teardown must not drop the backlog. SerialWorkQueue's own destructor CANCELS
// queued jobs, so a queue that merely relied on it would lose the final save of
// every session -- the most valuable one.
void TestDestructorFlushesPendingWrites() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state.record";
  {
    PersistedRecordWriteQueue queue;
    queue.Queue(path, Body("last-write-before-exit"), 1u);
    // No Flush(): destruction is what has to save it.
  }
  Expect(std::filesystem::exists(path), "destruction must land a still-queued write");
  Expect(ReadRecordText(path) == "last-write-before-exit",
         "the write the destructor landed must be the queued one");
}

// Save-then-delete and delete-then-save must resolve in submission order. Both
// run on the same key, so this is really a test that the delete does not jump
// ahead of (or fall behind) a write it was ordered against.
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

// The skip-identical-body memo is the one place an optimization could eat data:
// if it claimed a body was on disk when it was not, that state would be lost
// until the bytes happened to change again.
void TestMemoNeverSkipsAWriteThatIsNotOnDisk() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state.record";
  PersistedRecordWriteQueue queue;

  queue.Queue(path, Body("same"), 1u);
  queue.Flush();
  const std::size_t after_first = queue.applied_write_count();

  // Identical body: correctly skipped, since those exact bytes are already there.
  queue.Queue(path, Body("same"), 1u);
  queue.Flush();
  Expect(queue.applied_write_count() == after_first,
         "an identical body over an existing record should skip the rewrite");

  // ...but the file going missing must invalidate that reasoning.
  std::error_code error;
  std::filesystem::remove(path, error);
  std::filesystem::remove(PersistedRecordWriter::BackupPathFor(path), error);
  queue.Queue(path, Body("same"), 1u);
  queue.Flush();
  Expect(std::filesystem::exists(path),
         "the memo must never skip a write against a record that no longer exists");
  Expect(ReadRecordText(path) == "same", "the recreated record must hold the queued body");
}

// A write can fail. The next save of the SAME body must still be attempted -- a
// memo that recorded the failed write as persisted would strand that state
// permanently, because every later save of it would be skipped as "already on
// disk" against a record that never received it.
//
// Two failure shapes, and they are protected by different things:
//   * the record is GONE      -> the existence probe catches it
//   * the record still EXISTS, holding the PREVIOUS body, because the write
//     failed partway -> only the not-remembering catches it, and it is the
//     dangerous one: the file looks fine, so nothing else notices the loss.
void TestFailedWriteIsRetriedOnTheNextSave() {
  TemporaryDirectory temp_dir;
  // A regular FILE where the record's parent directory should be. The writer
  // cannot work around this: create_directories fails outright, so the write
  // fails before it ever stages a temp. (A directory at the record path itself is
  // not a blocker -- the backup rotation renames it aside and the write succeeds.)
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

  // Now the shape the existence probe cannot see. Land body A, then make the
  // directory read-only so staging the temp file fails while the record itself
  // survives holding A, then queue body B.
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

  // Running as root ignores the mode bits, so the write is not actually blocked
  // and there is nothing to assert. Say so rather than passing vacuously.
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

// Interleaved saves across several records must not lose or cross-contaminate
// any of them, including while writes are actively draining.
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
      queue.Flush();  // drain mid-stream so some rounds race a draining queue
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

// The service-level contract: a save followed by a load returns the saved state
// without the caller flushing. Loads flush internally precisely so no caller has
// to know the write is asynchronous.
void TestServiceLoadSeesAnUnflushedSave() {
  using microide::workspace::PersistedUserConfigState;
  using microide::workspace::PersistenceService;

  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "user.config";
  PersistenceService service;

  const PersistedUserConfigState saved{.ui_scale = 1.5f, .settings = {{"theme", "async"}}};
  Expect(service.SaveUserConfig(path, saved), "save should be accepted");
  // Deliberately no FlushPendingWrites() here.

  PersistedUserConfigState loaded;
  Expect(service.LoadUserConfig(path, &loaded), "load should succeed against a pending write");
  Expect(loaded.settings.size() == 1 && loaded.settings[0].second == "async",
         "a load must never overtake a save that was already accepted");
}

}  // namespace

void RegisterPersistedRecordWriteQueueTests(std::vector<TestCase>& tests) {
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

}  // namespace microide::tests

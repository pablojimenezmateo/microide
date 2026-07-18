#include "TestSupport.h"

#include "persistence/PersistedRecord.h"
#include "persistence/PersistedRecordReader.h"
#include "persistence/PersistedRecordWriter.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace microide::tests {
namespace {

using microide::persistence::PersistedRecordReadResult;
using microide::persistence::PersistedRecordReader;
using microide::persistence::PersistedRecordReaderError;
using microide::persistence::PersistedRecordWriter;
using microide::persistence::PersistedRecordWriterError;

std::vector<std::byte> BytesFromText(std::string_view text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (char ch : text) {
    bytes.push_back(std::byte(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

std::string TextFromBytes(std::span<const std::byte> bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (std::byte byte : bytes) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
  }
  return text;
}

PersistedRecordReadResult ReadRecordOrFail(const std::filesystem::path& path,
                                           std::string_view context) {
  PersistedRecordReaderError error = PersistedRecordReaderError::None;
  std::optional<PersistedRecordReadResult> result = PersistedRecordReader::ReadFile(path, &error);
  Expect(result.has_value(), std::string(context) + ": read should succeed");
  Expect(error == PersistedRecordReaderError::None, std::string(context) + ": read error mismatch");
  return *result;
}

void TestPersistedRecordWriterRoundTripsWithAtomicBackupFlow() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state" / "project.state.bin";

  const std::vector<std::byte> body_v1 = BytesFromText("version-one");
  PersistedRecordWriterError write_error = PersistedRecordWriterError::None;
  Expect(PersistedRecordWriter::WriteFile(path, body_v1, 11u, &write_error),
         "first write should succeed");
  Expect(write_error == PersistedRecordWriterError::None, "first write should not report an error");
  Expect(std::filesystem::exists(path), "first write should create the destination file");
  Expect(!std::filesystem::exists(path.string() + ".tmp"),
         "atomic writer should not leave a temp file");
  Expect(!std::filesystem::exists(PersistedRecordWriter::BackupPathFor(path)),
         "first write should not create a backup file");

  const PersistedRecordReadResult first = ReadRecordOrFail(path, "round-trip first read");
  Expect(first.header.version == microide::persistence::kPersistedRecordFormatVersion,
         "first read version mismatch");
  Expect(first.header.capability_flags == 11u, "first read capability flags mismatch");
  Expect(!first.used_backup, "first read should use primary file");
  Expect(TextFromBytes(first.body) == "version-one", "first read body mismatch");

  const std::vector<std::byte> body_v2 = BytesFromText("version-two");
  Expect(PersistedRecordWriter::WriteFile(path, body_v2, 27u, &write_error),
         "second write should succeed");
  Expect(write_error == PersistedRecordWriterError::None, "second write should not report an error");
  const std::filesystem::path backup = PersistedRecordWriter::BackupPathFor(path);
  Expect(std::filesystem::exists(backup),
         "second write should preserve previous content in backup");

  const PersistedRecordReadResult second = ReadRecordOrFail(path, "round-trip second read");
  Expect(second.header.capability_flags == 27u, "second read capability flags mismatch");
  Expect(TextFromBytes(second.body) == "version-two", "second read body mismatch");

  const auto backup_result = ReadRecordOrFail(backup, "round-trip backup read");
  Expect(backup_result.header.capability_flags == 11u,
         "backup should preserve previous capability flags");
  Expect(TextFromBytes(backup_result.body) == "version-one",
         "backup should preserve previous body payload");
}

void TestPersistedRecordReaderFallsBackToBackupOnCorruption() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state" / "user.config.bin";

  PersistedRecordWriterError write_error = PersistedRecordWriterError::None;
  Expect(PersistedRecordWriter::WriteFile(path, BytesFromText("known-good"), 3u, &write_error),
         "initial write should succeed");
  Expect(PersistedRecordWriter::WriteFile(path, BytesFromText("current"), 9u, &write_error),
         "second write should succeed and create backup");
  const std::filesystem::path backup = PersistedRecordWriter::BackupPathFor(path);
  Expect(std::filesystem::exists(backup), "backup file should exist after rewrite");

  WriteFile(path, "corrupt-payload-without-valid-header");

  PersistedRecordReaderError read_error = PersistedRecordReaderError::None;
  const auto recovered = PersistedRecordReader::ReadFile(path, &read_error);
  Expect(recovered.has_value(), "read should recover from backup on primary corruption");
  Expect(read_error == PersistedRecordReaderError::None, "recovery should not report an error");
  Expect(recovered->used_backup, "recovery should report backup usage");
  Expect(recovered->header.capability_flags == 3u,
         "recovery should return the last valid backup metadata");
  Expect(TextFromBytes(recovered->body) == "known-good",
         "recovery should return the last valid backup body");
}

void TestPersistedRecordReaderReportsParseFailureWithoutBackup() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state" / "session.workspace.bin";
  WriteFile(path, "bad-data");

  PersistedRecordReaderError error = PersistedRecordReaderError::None;
  const auto result = PersistedRecordReader::ReadFile(path, &error);
  Expect(!result.has_value(), "read should fail when no valid backup exists");
  Expect(error == PersistedRecordReaderError::ParseFailed,
         "read should report parse failure without backup");
}

void TestPersistedRecordReaderIgnoresTruncatedTempSibling() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state" / "project.state.bin";

  PersistedRecordWriterError write_error = PersistedRecordWriterError::None;
  Expect(PersistedRecordWriter::WriteFile(path, BytesFromText("stable"), 41u, &write_error),
         "baseline write should succeed");

  const std::filesystem::path temp_path = path.string() + ".tmp";
  WriteFile(temp_path, "x");

  PersistedRecordReaderError read_error = PersistedRecordReaderError::None;
  const auto result = PersistedRecordReader::ReadFile(path, &read_error);
  Expect(result.has_value(), "reader should ignore truncated temp siblings");
  Expect(read_error == PersistedRecordReaderError::None, "truncated temp should not produce errors");
  Expect(!result->used_backup, "truncated temp should not force backup usage");
  Expect(TextFromBytes(result->body) == "stable", "primary payload should remain authoritative");
}

void TestPersistedRecordReaderIgnoresUnrenamedTempPayloadAfterSimulatedCrash() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state" / "session.workspace.bin";

  PersistedRecordWriterError write_error = PersistedRecordWriterError::None;
  Expect(PersistedRecordWriter::WriteFile(path, BytesFromText("old"), 7u, &write_error),
         "initial write should succeed");

  std::vector<std::byte> pending_body = BytesFromText("new-uncommitted");
  std::vector<std::byte> pending_file;
  Expect(microide::persistence::BuildPersistedRecordFile(pending_body, 9u, &pending_file),
         "pending temp payload should be encoded");
  WriteFile(path.string() + ".tmp", TextFromBytes(pending_file));

  PersistedRecordReaderError read_error = PersistedRecordReaderError::None;
  const auto result = PersistedRecordReader::ReadFile(path, &read_error);
  Expect(result.has_value(), "reader should read committed primary file");
  Expect(result->header.capability_flags == 7u, "reader should ignore unrenamed temp metadata");
  Expect(TextFromBytes(result->body) == "old", "reader should ignore unrenamed temp payload");
}

void TestPersistedRecordReaderRejectsUnsupportedVersionEvenWithValidCrc() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state" / "chat.conversations.bin";

  std::vector<std::byte> body = BytesFromText("payload");
  std::vector<std::byte> file_bytes;
  Expect(microide::persistence::BuildPersistedRecordFile(body, 4u, &file_bytes),
         "test should build a valid record file");
  Expect(file_bytes.size() > 8, "test file should include a version field");

  file_bytes[4] = std::byte{0x02};
  file_bytes[5] = std::byte{0x00};
  file_bytes[6] = std::byte{0x00};
  file_bytes[7] = std::byte{0x00};

  WriteFile(path, TextFromBytes(file_bytes));

  PersistedRecordReaderError error = PersistedRecordReaderError::None;
  const auto result = PersistedRecordReader::ReadFile(path, &error);
  Expect(!result.has_value(), "read should reject unknown version");
  Expect(error == PersistedRecordReaderError::UnsupportedVersion,
         "read should report unsupported version");
}

void TestPrimitiveReaderSanitizesNonFiniteF32() {
  // A forged file can encode NaN/Inf in an F32 field. std::clamp(NaN, lo, hi)
  // returns NaN, which later reaches static_cast<int>(NaN) in layout math (UB).
  // ReadF32 must neutralize non-finite payloads at the source.
  const std::uint32_t kNonFinite[] = {
      0x7FC00000u,  // quiet NaN
      0x7F800000u,  // +Inf
      0xFF800000u,  // -Inf
  };
  for (const std::uint32_t bits : kNonFinite) {
    std::vector<std::byte> payload;
    microide::persistence::PrimitiveWriter writer(&payload);
    Expect(writer.WriteU32(bits), "raw non-finite F32 bits should encode");
    microide::persistence::PrimitiveReader reader(payload);
    float value = 1.0f;
    Expect(reader.ReadF32(&value), "ReadF32 should still succeed on non-finite input");
    Expect(std::isfinite(value), "ReadF32 must sanitize NaN/Inf to a finite value");
  }

  // A finite value round-trips unchanged.
  std::vector<std::byte> finite_payload;
  microide::persistence::PrimitiveWriter finite_writer(&finite_payload);
  Expect(finite_writer.WriteF32(288.0f), "finite F32 should encode");
  microide::persistence::PrimitiveReader finite_reader(finite_payload);
  float finite_value = 0.0f;
  Expect(finite_reader.ReadF32(&finite_value), "finite F32 should decode");
  Expect(finite_value == 288.0f, "finite F32 must round-trip unchanged");
}

// Clearing persisted state by removing only the primary file lets it resurrect from
// the backup on the next read. Removing BOTH primary and backup (what
// PersistenceService::DeleteState does) prevents the resurrection — the mechanism
// behind the cleared-debug-state fix.
void TestRemovingOnlyPrimaryResurrectsFromBackup() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state" / "debug.bin";

  PersistedRecordWriterError write_error = PersistedRecordWriterError::None;
  Expect(PersistedRecordWriter::WriteFile(path, BytesFromText("v1"), 3u, &write_error),
         "first write succeeds");
  Expect(PersistedRecordWriter::WriteFile(path, BytesFromText("v2"), 3u, &write_error),
         "second write succeeds and rotates a backup");
  const std::filesystem::path backup = PersistedRecordWriter::BackupPathFor(path);
  Expect(std::filesystem::exists(backup), "backup exists after the second write");

  // Buggy clear: remove only the primary. The reader falls back to the backup.
  std::error_code ec;
  std::filesystem::remove(path, ec);
  Expect(PersistedRecordReader::ReadFile(path).has_value(),
         "removing only the primary resurrects state from the backup");

  // Correct clear (DeleteState): remove both. Now the read finds nothing.
  std::filesystem::remove(backup, ec);
  Expect(!PersistedRecordReader::ReadFile(path).has_value(),
         "removing both primary and backup fully clears the state");
}

// Regression: a genuine stat failure on the primary file (here ELOOP from a
// symlink cycle) must surface as ReadFailed, not NotFound. std::filesystem::exists
// returns false *and sets the error code* on such failures; the reader used to
// test !exists() first and report NotFound, letting a transient I/O error look
// like "no persisted state" — which a later save would overwrite with defaults.
void TestPersistedRecordReaderReportsReadFailureOnStatError() {
#if defined(_WIN32)
  return;  // POSIX symlink semantics
#else
  TemporaryDirectory temp_dir;
  const std::filesystem::path loopa = temp_dir.path() / "loopa";
  const std::filesystem::path loopb = temp_dir.path() / "loopb";
  std::error_code ec;
  std::filesystem::create_directory_symlink(loopb, loopa, ec);
  Expect(!ec, "loop symlink 'loopa' should be created");
  std::filesystem::create_directory_symlink(loopa, loopb, ec);
  Expect(!ec, "loop symlink 'loopb' should be created");

  // Reading through the cycle stats with ELOOP for both primary and backup paths.
  PersistedRecordReaderError error = PersistedRecordReaderError::None;
  const auto result = PersistedRecordReader::ReadFile(loopa / "state.bin", &error);
  Expect(!result.has_value(), "reading through a symlink cycle should not succeed");
  Expect(error == PersistedRecordReaderError::ReadFailed,
         "a stat failure must be reported as ReadFailed, not NotFound");
#endif
}

// I16 regression: a readable but stale `.bak` must not silently mask a primary
// UnsupportedVersion. Falling back would let the next save overwrite newer state
// with older data and hide the version mismatch. The reader now refuses the
// fallback for UnsupportedVersion (surfacing the mismatch) unless the caller opts
// into an explicit downgrade-recovery path, and it always surfaces the primary
// failure + backup usage through the result.
void TestPersistedRecordReaderRefusesBackupForUnsupportedVersion() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state" / "project.state.bin";

  // A valid backup (v1) with a valid current-format primary (v2), then overwrite
  // the primary with a CRC-valid but future-version record.
  PersistedRecordWriterError write_error = PersistedRecordWriterError::None;
  Expect(PersistedRecordWriter::WriteFile(path, BytesFromText("known-good"), 3u, &write_error),
         "initial write should succeed");
  Expect(PersistedRecordWriter::WriteFile(path, BytesFromText("current"), 9u, &write_error),
         "second write should succeed and rotate a valid backup");
  const std::filesystem::path backup = PersistedRecordWriter::BackupPathFor(path);
  Expect(std::filesystem::exists(backup), "backup should exist after the rewrite");

  std::vector<std::byte> future_primary;
  Expect(microide::persistence::BuildPersistedRecordFile(BytesFromText("future"), 9u,
                                                         &future_primary),
         "future-version primary should build");
  Expect(future_primary.size() > 8, "primary file should include a version field");
  future_primary[4] = std::byte{0x02};  // bump the format version out of range
  future_primary[5] = std::byte{0x00};
  future_primary[6] = std::byte{0x00};
  future_primary[7] = std::byte{0x00};
  WriteFile(path, TextFromBytes(future_primary));

  // Default: fallback refused, the version mismatch surfaces, no value returned.
  PersistedRecordReaderError error = PersistedRecordReaderError::None;
  const auto refused = PersistedRecordReader::ReadFile(path, &error);
  Expect(!refused.has_value(), "an unsupported-version primary must not fall back by default");
  Expect(error == PersistedRecordReaderError::UnsupportedVersion,
         "a refused fallback should surface the version mismatch reason");

  // Opt-in downgrade recovery: the backup is used, and the result carries both the
  // used_backup flag and the primary failure that triggered recovery.
  PersistedRecordReaderError recovery_error = PersistedRecordReaderError::None;
  const auto recovered =
      PersistedRecordReader::ReadFile(path, &recovery_error, /*allow_backup_for_unsupported_version=*/true);
  Expect(recovered.has_value(), "explicit downgrade recovery should read the backup");
  Expect(recovery_error == PersistedRecordReaderError::None,
         "a successful downgrade recovery reports None as the overall status");
  Expect(recovered->used_backup, "downgrade recovery should report backup usage");
  Expect(recovered->primary_error == PersistedRecordReaderError::UnsupportedVersion,
         "downgrade recovery should surface the primary version mismatch through the result");
  Expect(TextFromBytes(recovered->body) == "known-good",
         "downgrade recovery should return the backup body");
}

// I16 regression: a parse-failure primary with a valid backup still recovers, but
// the result must now surface that recovery happened (used_backup) and why
// (primary_error), so operators do not lose the signal.
void TestPersistedRecordReaderSurfacesBackupUsageOnParseFailure() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state" / "user.config.bin";

  PersistedRecordWriterError write_error = PersistedRecordWriterError::None;
  Expect(PersistedRecordWriter::WriteFile(path, BytesFromText("known-good"), 3u, &write_error),
         "initial write should succeed");
  Expect(PersistedRecordWriter::WriteFile(path, BytesFromText("current"), 9u, &write_error),
         "second write should rotate a valid backup");
  WriteFile(path, "corrupt-payload-without-valid-header");

  PersistedRecordReaderError error = PersistedRecordReaderError::None;
  const auto recovered = PersistedRecordReader::ReadFile(path, &error);
  Expect(recovered.has_value(), "a parse-failure primary should recover from a valid backup");
  Expect(error == PersistedRecordReaderError::None, "recovery reports None as the overall status");
  Expect(recovered->used_backup, "recovery should surface backup usage");
  Expect(recovered->primary_error == PersistedRecordReaderError::ParseFailed,
         "recovery should surface the primary parse failure that triggered fallback");
  Expect(TextFromBytes(recovered->body) == "known-good", "recovery should return the backup body");
}

// Regression (header-first bounded read): a large file with a bad magic must be
// rejected on the fixed 16-byte prefix, before the body is read into memory. We
// can't probe the allocation directly here, but we lock the behavioral contract
// that a large non-record file is rejected as ParseFailed.
void TestPersistedRecordReaderRejectsLargeBadMagicOnHeader() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state" / "session.workspace.bin";
  // 4 MiB of a byte that is NOT the 'M' of the magic → header check fails fast.
  WriteFile(path, std::string(4u * 1024 * 1024, 'Z'));

  PersistedRecordReaderError error = PersistedRecordReaderError::None;
  const auto result = PersistedRecordReader::ReadFile(path, &error);
  Expect(!result.has_value(), "a large bad-magic file must be rejected");
  Expect(error == PersistedRecordReaderError::ParseFailed,
         "a bad magic must be reported as a parse failure via the header path");
}

// Regression: a large file with a VALID magic but unsupported version is rejected
// as UnsupportedVersion from the header prefix (so no backup fallback), without
// reading the whole body.
void TestPersistedRecordReaderRejectsLargeUnsupportedVersionOnHeader() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state" / "session.workspace.bin";

  std::string content;
  content.append("MIDE");                    // valid magic
  content.push_back(static_cast<char>(99));  // version = 99 (LE), unsupported
  content.append(3, '\0');
  content.append(4, '\0');                    // capability flags
  content.append(4, '\0');                    // crc32c placeholder
  content.append(2u * 1024 * 1024, 'x');      // oversized body
  WriteFile(path, content);

  PersistedRecordReaderError error = PersistedRecordReaderError::None;
  const auto result = PersistedRecordReader::ReadFile(path, &error);
  Expect(!result.has_value(), "an unsupported-version file must be rejected");
  Expect(error == PersistedRecordReaderError::UnsupportedVersion,
         "an unsupported version must be reported from the header, blocking backup fallback");
}

}  // namespace

// TD-2026-07-17-073: a persisted path swapped for a directory/FIFO/device must be
// classified as a read failure (not opened, not NotFound). The reader now proves
// the path is a regular file before opening it.
void TestPersistedRecordReaderRejectsNonRegularPath() {
  TemporaryDirectory temp_dir;

  // Directory case (all platforms).
  const std::filesystem::path dir_path = temp_dir.path() / "state_dir";
  std::error_code ec;
  std::filesystem::create_directory(dir_path, ec);
  Expect(!ec, "probe directory should be created");
  PersistedRecordReaderError dir_error = PersistedRecordReaderError::None;
  const auto dir_result = PersistedRecordReader::ReadFile(dir_path, &dir_error);
  Expect(!dir_result.has_value(), "reading a directory path must not yield a record");
  Expect(dir_error == PersistedRecordReaderError::ReadFailed,
         "a directory persisted-state path is a read failure, not NotFound");

#if defined(__unix__) || defined(__APPLE__)
  const std::filesystem::path fifo_path = temp_dir.path() / "state_fifo";
  if (::mkfifo(fifo_path.c_str(), 0600) == 0) {
    PersistedRecordReaderError fifo_error = PersistedRecordReaderError::None;
    const auto fifo_result = PersistedRecordReader::ReadFile(fifo_path, &fifo_error);
    Expect(!fifo_result.has_value(), "reading a FIFO path must not block or yield a record");
    Expect(fifo_error == PersistedRecordReaderError::ReadFailed,
           "a FIFO persisted-state path is a read failure");
    std::filesystem::remove(fifo_path, ec);
  }
#endif
}

// TD-2026-07-17A-127: writing a persisted-state record through a symlinked path must
// update the link's target and preserve the link, not rotate the link node to `.bak`
// and publish a fresh regular file at the link path (which breaks user-managed
// config/session symlinks).
void TestPersistedRecordWriterPreservesSymlinkedTarget() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#endif
  TemporaryDirectory temp_dir;
  const std::filesystem::path target = temp_dir.path() / "real" / "session.bin";
  const std::filesystem::path link_path = temp_dir.path() / "link" / "session.bin";
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  std::filesystem::create_directories(link_path.parent_path(), ec);

  PersistedRecordWriterError write_error = PersistedRecordWriterError::None;
  Expect(PersistedRecordWriter::WriteFile(target, BytesFromText("v1"), 1u, &write_error),
         "seed the real target file");
  std::filesystem::create_symlink(target, link_path, ec);
  Expect(!ec, "symlink creation should succeed");
  Expect(std::filesystem::is_symlink(std::filesystem::symlink_status(link_path)),
         "the link path is a symlink before the write");

  // Write THROUGH the symlink.
  Expect(PersistedRecordWriter::WriteFile(link_path, BytesFromText("v2"), 2u, &write_error),
         "writing through the symlink should succeed");

  Expect(std::filesystem::is_symlink(std::filesystem::symlink_status(link_path)),
         "writing through a symlink must preserve the link, not replace it with a regular file");
  Expect(TextFromBytes(ReadRecordOrFail(link_path, "read via link").body) == "v2",
         "the write lands on the symlink target (readable through the link)");
  Expect(TextFromBytes(ReadRecordOrFail(target, "read target").body) == "v2",
         "the real target holds the new content");
  Expect(std::filesystem::exists(PersistedRecordWriter::BackupPathFor(target)),
         "the backup is rotated beside the real target");
  Expect(!std::filesystem::exists(PersistedRecordWriter::BackupPathFor(link_path)),
         "no backup or regular file is created at the link path");
}

void RegisterPersistedRecordIoTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PersistedRecordIo/WriterPreservesSymlinkedTarget",
          TestPersistedRecordWriterPreservesSymlinkedTarget);
  AddTest(tests, "PersistedRecordIo/ReaderRejectsNonRegularPath",
          TestPersistedRecordReaderRejectsNonRegularPath);
  AddTest(tests, "PersistedRecordIo/ReaderRejectsLargeBadMagicOnHeader",
          TestPersistedRecordReaderRejectsLargeBadMagicOnHeader);
  AddTest(tests, "PersistedRecordIo/ReaderRejectsLargeUnsupportedVersionOnHeader",
          TestPersistedRecordReaderRejectsLargeUnsupportedVersionOnHeader);
  AddTest(tests, "PersistedRecordIo/ReaderRefusesBackupForUnsupportedVersion",
          TestPersistedRecordReaderRefusesBackupForUnsupportedVersion);
  AddTest(tests, "PersistedRecordIo/ReaderSurfacesBackupUsageOnParseFailure",
          TestPersistedRecordReaderSurfacesBackupUsageOnParseFailure);
  AddTest(tests, "PersistedRecordIo/ReaderReportsReadFailureOnStatError",
          TestPersistedRecordReaderReportsReadFailureOnStatError);
  AddTest(tests, "PersistedRecordIo/RemovingOnlyPrimaryResurrectsFromBackup",
          TestRemovingOnlyPrimaryResurrectsFromBackup);
  AddTest(tests, "PersistedRecordIo/PrimitiveReaderSanitizesNonFiniteF32",
          TestPrimitiveReaderSanitizesNonFiniteF32);
  AddTest(tests, "PersistedRecordIo/WriterRoundTripsWithAtomicBackupFlow",
          TestPersistedRecordWriterRoundTripsWithAtomicBackupFlow);
  AddTest(tests, "PersistedRecordIo/ReaderFallsBackToBackupOnCorruption",
          TestPersistedRecordReaderFallsBackToBackupOnCorruption);
  AddTest(tests, "PersistedRecordIo/ReaderReportsParseFailureWithoutBackup",
          TestPersistedRecordReaderReportsParseFailureWithoutBackup);
  AddTest(tests, "PersistedRecordIo/ReaderIgnoresTruncatedTempSibling",
          TestPersistedRecordReaderIgnoresTruncatedTempSibling);
  AddTest(tests, "PersistedRecordIo/ReaderIgnoresUnrenamedTempPayloadAfterSimulatedCrash",
          TestPersistedRecordReaderIgnoresUnrenamedTempPayloadAfterSimulatedCrash);
  AddTest(tests, "PersistedRecordIo/ReaderRejectsUnsupportedVersionEvenWithValidCrc",
          TestPersistedRecordReaderRejectsUnsupportedVersionEvenWithValidCrc);
}

}  // namespace microide::tests

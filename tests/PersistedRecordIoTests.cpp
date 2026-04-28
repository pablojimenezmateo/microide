#include "TestSupport.h"

#include "persistence/PersistedRecord.h"
#include "persistence/PersistedRecordReader.h"
#include "persistence/PersistedRecordWriter.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

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

}  // namespace

void RegisterPersistedRecordIoTests(std::vector<TestCase>& tests) {
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

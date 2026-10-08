#include "TestSupport.h"

#include "project/remote/RemoteManifest.h"
#include "util/ByteCodec.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using project::remote::DecodeManifestRows;
using project::remote::EncodeManifestRows;
using project::remote::IsSafeRelativePath;
using project::remote::ManifestEntryKind;
using project::remote::ManifestRow;

ManifestRow FileRow(std::string path, std::string_view content) {
  ManifestRow row;
  row.path = std::move(path);
  row.size = content.size();
  row.mode = 0644;
  row.mtime_ns = 1'700'000'000'123'456'789;
  row.hash = util::HashContent(content);
  return row;
}

void TestRemoteManifestRoundTrip() {
  std::vector<ManifestRow> rows = {
      FileRow("src/a.cpp", "a"),
      FileRow("src/a.h", "b"),
      FileRow("src/sub/deep/name.txt", "c"),
      FileRow("tools/run.sh", "#!/bin/sh\n"),
  };
  rows[3].mode = 0755;
  ManifestRow link;
  link.path = "tools/latest";
  link.kind = ManifestEntryKind::Symlink;
  link.link_target = "run.sh";
  rows.push_back(link);
  ManifestRow module;
  module.path = "vendor/lib";
  module.kind = ManifestEntryKind::Submodule;
  module.mtime_ns = -5;
  rows.push_back(module);

  std::string bytes;
  EncodeManifestRows(rows, bytes);
  std::vector<ManifestRow> decoded;
  Expect(DecodeManifestRows(bytes, decoded), "a well-formed chunk decodes");
  Expect(decoded == rows, "every row survives the trip, kinds, modes and hashes included");

  // Shared prefixes are stored once: the chunk is far smaller than the paths.
  std::size_t path_bytes = 0;
  for (const ManifestRow& row : rows) {
    path_bytes += row.path.size();
  }
  Expect(bytes.size() < path_bytes + rows.size() * 48, "paths are prefix-compressed");

  // A second chunk appends, and decodes independently of the first.
  Expect(DecodeManifestRows(bytes, decoded) && decoded.size() == rows.size() * 2,
         "chunks append");
}

void TestRemoteManifestRejectsUnsafePaths() {
  for (const char* path : {"", "/etc/passwd", "../x", "a/../b", "a//b", "a/./b", "a/", "./a",
                           "a\\b"}) {
    Expect(!IsSafeRelativePath(path), std::string("unsafe: '") + path + "'");
  }
  Expect(!IsSafeRelativePath(std::string("a\0b", 3)), "an embedded NUL is unsafe");
  Expect(!IsSafeRelativePath(std::string(256, 'x')), "a component over 255 bytes is unsafe");
  for (const char* path : {"a", "a/b", ".hidden/x", "a..b", "..a/b"}) {
    Expect(IsSafeRelativePath(path), std::string("safe: '") + path + "'");
  }

  // A forged chunk whose second row walks out of the root through the prefix.
  std::vector<ManifestRow> rows = {FileRow("ok", "x")};
  std::string bytes;
  EncodeManifestRows(rows, bytes);
  std::string forged;
  util::PutVarint(forged, 1);
  util::PutVarint(forged, 0);
  util::PutBytes(forged, "../../.bashrc");
  forged.push_back('\0');
  util::PutVarint(forged, 1);
  util::PutVarint(forged, 0644);
  util::PutVarint(forged, 0);
  forged.append(32, 'h');
  std::vector<ManifestRow> decoded;
  Expect(DecodeManifestRows(bytes, decoded) && decoded.size() == 1, "the good chunk decodes");
  Expect(!DecodeManifestRows(forged, decoded), "a path escaping the root is refused");
  Expect(decoded.size() == 1, "a refused chunk leaves nothing behind");
}

void TestRemoteManifestRejectsMalformedBytes() {
  std::vector<ManifestRow> rows = {FileRow("a", "1"), FileRow("ab", "2")};
  std::string bytes;
  EncodeManifestRows(rows, bytes);
  std::vector<ManifestRow> decoded;
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    Expect(!DecodeManifestRows(std::string_view(bytes).substr(0, cut), decoded) &&
               decoded.empty(),
           "a truncated chunk is refused whole");
  }
  Expect(!DecodeManifestRows(bytes + "x", decoded), "trailing bytes are refused");
  std::string huge_count;
  util::PutVarint(huge_count, 1ull << 40);
  Expect(!DecodeManifestRows(huge_count, decoded), "a count the payload cannot hold is refused");
}

}  // namespace

void RegisterRemoteManifestTests(std::vector<TestCase>& tests) {
  AddTest(tests, "RemoteManifest/RoundTrip", TestRemoteManifestRoundTrip);
  AddTest(tests, "RemoteManifest/RejectsUnsafePaths", TestRemoteManifestRejectsUnsafePaths);
  AddTest(tests, "RemoteManifest/RejectsMalformedBytes", TestRemoteManifestRejectsMalformedBytes);
}

}  // namespace microide::tests

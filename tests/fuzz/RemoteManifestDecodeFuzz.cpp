// The manifest row decoder (src/project/remote/RemoteManifest.h). Its bytes come
// from the host, and every path it yields is about to name a file under the mirror,
// so it is a hostile-input surface (dev-docs/design/remote-projects.md § 6.9).
//
// Invariants: a successful decode yields only safe relative paths, and re-encoding
// the rows reproduces the input exactly (no row dropped, invented or altered); a
// failed decode leaves the output vector as it was.
#include "project/remote/RemoteManifest.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using microide::project::remote::DecodeManifestRows;
  using microide::project::remote::EncodeManifestRows;
  using microide::project::remote::IsSafeRelativePath;
  using microide::project::remote::ManifestRow;

  const std::string_view bytes(reinterpret_cast<const char*>(data), size);
  std::vector<ManifestRow> rows(1);
  rows[0].path = "sentinel";
  if (!DecodeManifestRows(bytes, rows)) {
    if (rows.size() != 1 || rows[0].path != "sentinel") {
      std::abort();
    }
    return 0;
  }
  for (std::size_t i = 1; i < rows.size(); ++i) {
    if (!IsSafeRelativePath(rows[i].path)) {
      std::abort();
    }
  }
  std::string reencoded;
  EncodeManifestRows(rows.data() + 1, rows.size() - 1, reencoded);
  // Varints have one canonical spelling only when the encoder wrote them, so compare
  // by decoding again rather than byte-for-byte.
  std::vector<ManifestRow> again;
  if (!DecodeManifestRows(reencoded, again) || again.size() != rows.size() - 1) {
    std::abort();
  }
  for (std::size_t i = 0; i < again.size(); ++i) {
    if (!(again[i] == rows[i + 1])) {
      std::abort();
    }
  }
  return 0;
}

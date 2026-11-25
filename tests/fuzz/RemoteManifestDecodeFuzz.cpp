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

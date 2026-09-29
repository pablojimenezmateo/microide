#include "project/FileWriteGate.h"

namespace microide::project {

namespace {

class LocalGate final : public FileWriteGate {
 public:
  Result WriteText(const std::filesystem::path& path, std::string_view text) override {
    Result result;
    result.ok = util::WriteTextFileAtomically(path, text);
    if (result.ok) {
      // Stat only on success: a failed write left the original in place, and its
      // signature is whatever the caller already had.
      result.signature = util::StatFileSignature(path);
    }
    return result;
  }
};

}  // namespace

FileWriteGate& LocalFileWriteGate() {
  static LocalGate gate;
  return gate;
}

}  // namespace microide::project

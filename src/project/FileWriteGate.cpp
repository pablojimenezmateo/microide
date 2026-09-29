#include "project/FileWriteGate.h"

namespace microide::project {

namespace {

class LocalGate final : public FileWriteGate {
 public:
  Result WriteText(const std::filesystem::path& path,
                   std::string_view text,
                   Signature signature) override {
    Result result;
    result.ok = util::WriteTextFileAtomically(path, text);
    if (result.ok && signature == Signature::Capture) {
      // Stat only on success: a failed write left the original in place, and its
      // signature is whatever the caller already had. The bytes are right here, so
      // the signature carries their hash too — which is what lets a later check
      // tell a real external edit from a touch or a byte-identical rewrite.
      result.signature = util::SignatureForKnownContent(path, text);
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

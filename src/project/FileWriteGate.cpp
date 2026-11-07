#include "project/FileWriteGate.h"

#include "project/LocalTreeOps.h"

#include <system_error>

#include <cerrno>
#include <unistd.h>

namespace microide::project {

namespace {

class LocalGate final : public FileWriteGate {
 public:
  Result WriteText(const std::filesystem::path& path,
                   std::string_view text,
                   Signature signature,
                   util::WriteTarget target) override {
    return WriteTextLocally(path, text, signature, target, {});
  }

  TreeResult ApplyTreeOps(std::span<const TreeOp> ops) override {
    return ApplyLocalTreeOps(ops);
  }

  void DisposeStaged(std::span<const std::filesystem::path> staged) override {
    for (const std::filesystem::path& path : staged) {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  }
};

}  // namespace

FileWriteGate::Result WriteTextLocally(const std::filesystem::path& path, std::string_view text,
                                       FileWriteGate::Signature signature, util::WriteTarget target,
                                       const std::filesystem::path& staging) {
  FileWriteGate::Result result;
  result.ok = util::WriteTextFileAtomically(path, text, &result.error, target, staging);
  if (result.ok && signature == FileWriteGate::Signature::Capture) {
    // Stat only on success: a failed write left the original in place, and its
    // signature is whatever the caller already had. The bytes are right here, so
    // the signature carries their hash too — which is what lets a later check
    // tell a real external edit from a touch or a byte-identical rewrite.
    result.signature = util::SignatureForKnownContent(path, text);
  }
  return result;
}

bool FileWriteGate::IsReadOnly(const std::filesystem::path& path) const {
  std::error_code error;
  const std::filesystem::file_status status = std::filesystem::status(path, error);
  if (error || !std::filesystem::is_regular_file(status)) {
    return false;  // nothing there yet: the save creates it
  }
  // Asked of the kernel rather than read off the mode bits: it knows the owner,
  // the groups, ACLs, and that root may write anything.
  return ::access(path.c_str(), W_OK) != 0 && errno == EACCES;
}

bool FileWriteGate::MakeWritable(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::permissions(path, std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add, error);
  return !error && !IsReadOnly(path);
}

FileWriteGate& LocalFileWriteGate() {
  static LocalGate gate;
  return gate;
}

}  // namespace microide::project

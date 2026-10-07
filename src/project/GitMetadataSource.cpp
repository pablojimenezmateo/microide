#include "project/GitMetadataSource.h"

#include "platform/ProcessLauncher.h"

namespace microide::project {

namespace {

class LocalSource final : public GitMetadataSource {
 public:
  GitAvailability Availability(const std::filesystem::path& root) const override {
    return internal::HasGitMarker(root) ? GitAvailability::Repository
                                        : GitAvailability::NotARepository;
  }
  std::optional<std::filesystem::path> ReadableGitDirectory(
      const std::filesystem::path& root) const override {
    return internal::ResolveGitDirectory(root);
  }
};

}  // namespace

const GitMetadataSource& LocalGitMetadataSource() {
  static const LocalSource source;
  return source;
}

const GitMetadataSource& GitMetadataFor(const platform::ProcessLauncher& launcher) {
  // A sideways cast, answered from the vtable: a git call site pays one type check,
  // and a remote launcher opts in by implementing the interface, with nothing to
  // register and nothing to keep in sync.
  if (const auto* source = dynamic_cast<const GitMetadataSource*>(&launcher)) {
    return *source;
  }
  return LocalGitMetadataSource();
}

}  // namespace microide::project

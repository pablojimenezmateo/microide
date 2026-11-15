#include "project/remote/MirrorRoutingWriteGate.h"

#include <algorithm>
#include <mutex>

#include "util/PathMatch.h"

namespace microide::project::remote {

void MirrorRoutingWriteGate::Register(const std::filesystem::path& tree, FileWriteGate& gate) {
  std::unique_lock lock(mutex_);
  const std::filesystem::path normal = tree.lexically_normal();
  const auto known = std::find_if(mirrors_.begin(), mirrors_.end(),
                                  [&](const Mirror& mirror) { return mirror.tree == normal; });
  if (known != mirrors_.end()) {
    known->gate = &gate;
    return;
  }
  mirrors_.push_back(Mirror{.tree = normal, .gate = &gate});
}

void MirrorRoutingWriteGate::Unregister(const std::filesystem::path& tree) {
  std::unique_lock lock(mutex_);
  const std::filesystem::path normal = tree.lexically_normal();
  std::erase_if(mirrors_, [&](const Mirror& mirror) { return mirror.tree == normal; });
}

FileWriteGate& MirrorRoutingWriteGate::GateForLocked(const std::filesystem::path& path) const {
  const Mirror* best = nullptr;
  for (const Mirror& mirror : mirrors_) {
    if (util::NormalizedPathEqualsOrWithin(path, mirror.tree) &&
        (best == nullptr || mirror.tree.native().size() > best->tree.native().size())) {
      best = &mirror;
    }
  }
  return best != nullptr ? *best->gate : LocalFileWriteGate();
}

FileWriteGate::Result MirrorRoutingWriteGate::WriteText(const std::filesystem::path& path,
                                                        std::string_view text, Signature signature,
                                                        util::WriteTarget target) {
  std::shared_lock lock(mutex_);
  return GateForLocked(path).WriteText(path, text, signature, target);
}

FileWriteGate::TreeResult MirrorRoutingWriteGate::ApplyTreeOps(std::span<const TreeOp> ops) {
  std::shared_lock lock(mutex_);
  // A batch is all or nothing, so it goes through one gate: the mirror's when any
  // of its paths is in one (that gate applies the local half and replays to the
  // host what lies in its tree), the local one otherwise.
  for (const TreeOp& op : ops) {
    for (const std::filesystem::path* path : {&op.path, &op.new_path}) {
      if (path->empty()) {
        continue;
      }
      FileWriteGate& gate = GateForLocked(*path);
      if (&gate != &LocalFileWriteGate()) {
        return gate.ApplyTreeOps(ops);
      }
    }
  }
  return LocalFileWriteGate().ApplyTreeOps(ops);
}

void MirrorRoutingWriteGate::DisposeStaged(std::span<const std::filesystem::path> staged) {
  std::shared_lock lock(mutex_);
  if (staged.empty()) {
    return;
  }
  GateForLocked(staged.front()).DisposeStaged(staged);
}

bool MirrorRoutingWriteGate::IsReadOnly(const std::filesystem::path& path) const {
  std::shared_lock lock(mutex_);
  return GateForLocked(path).IsReadOnly(path);
}

bool MirrorRoutingWriteGate::MakeWritable(const std::filesystem::path& path) {
  std::shared_lock lock(mutex_);
  return GateForLocked(path).MakeWritable(path);
}

}  // namespace microide::project::remote

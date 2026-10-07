#include "parity/LoopbackLocality.h"

#include <system_error>
#include <utility>

#include "util/TextFileIO.h"

namespace microide::tests::parity {

LoopbackPathMap::LoopbackPathMap(std::filesystem::path mirror_root,
                                 std::filesystem::path host_root)
    : mirror_root_(mirror_root.lexically_normal()), host_root_(host_root.lexically_normal()) {}

std::optional<std::filesystem::path> LoopbackPathMap::ToHost(
    const std::filesystem::path& mirror_path) const {
  const std::filesystem::path normal = mirror_path.lexically_normal();
  auto root_it = mirror_root_.begin();
  auto path_it = normal.begin();
  for (; root_it != mirror_root_.end(); ++root_it, ++path_it) {
    // A trailing separator normalizes to an empty final component.
    if (root_it->empty()) {
      continue;
    }
    if (path_it == normal.end() || *path_it != *root_it) {
      return std::nullopt;
    }
  }
  std::filesystem::path host = host_root_;
  for (; path_it != normal.end(); ++path_it) {
    if (!path_it->empty()) {
      host /= *path_it;
    }
  }
  return host;
}

void LoopbackProcessLauncher::Record(const std::vector<std::string>& argv) const {
  std::string line;
  for (const std::string& word : argv) {
    if (!line.empty()) {
      line += ' ';
    }
    line += word;
  }
  std::lock_guard lock(mutex_);
  spawns_.push_back(std::move(line));
}

std::vector<std::string> LoopbackProcessLauncher::ResolveArgv(
    std::vector<std::string> argv) const {
  Record(argv);
  return argv;
}

std::filesystem::path LoopbackProcessLauncher::ResolveWorkingDirectory(
    std::filesystem::path cwd) const {
  if (cwd.empty()) {
    return cwd;
  }
  // A directory outside the project is left as it is: it is not the mirror's, and
  // a real remote launcher would refuse it rather than guess.
  return map_.ToHost(cwd).value_or(std::move(cwd));
}

platform::SubprocessResult LoopbackProcessLauncher::Run(std::vector<std::string> argv,
                                                        platform::SubprocessOptions options) const {
  Record(argv);
  options.cwd = ResolveWorkingDirectory(std::move(options.cwd));
  return platform::LocalProcessLauncher().Run(std::move(argv), std::move(options));
}

project::GitAvailability LoopbackProcessLauncher::Availability(
    const std::filesystem::path& root) const {
  const auto host = map_.ToHost(root);
  return host.has_value() ? project::LocalGitMetadataSource().Availability(*host)
                          : project::GitAvailability::NotARepository;
}

std::optional<std::filesystem::path> LoopbackProcessLauncher::ReadableGitDirectory(
    const std::filesystem::path& root) const {
  const auto host = map_.ToHost(root);
  return host.has_value() ? project::LocalGitMetadataSource().ReadableGitDirectory(*host)
                          : std::nullopt;
}

std::vector<std::string> LoopbackProcessLauncher::spawns() const {
  std::lock_guard lock(mutex_);
  return spawns_;
}

std::size_t LoopbackProcessLauncher::spawn_count() const {
  std::lock_guard lock(mutex_);
  return spawns_.size();
}

project::FileWriteGate::Result LoopbackWriteGate::WriteText(const std::filesystem::path& path,
                                                            std::string_view text,
                                                            Signature signature) {
  Result result = project::LocalFileWriteGate().WriteText(path, text, signature);
  if (!result.ok) {
    return result;
  }
  ++writes_;
  if (const auto host = map_.ToHost(path); host.has_value()) {
    std::error_code ec;
    std::filesystem::create_directories(host->parent_path(), ec);
    result.ok = util::WriteTextFileAtomically(*host, text);
  }
  return result;
}

project::FileWriteGate::TreeResult LoopbackWriteGate::ApplyTreeOps(std::span<const TreeOp> ops) {
  TreeResult result = project::LocalFileWriteGate().ApplyTreeOps(ops);
  if (!result.ok) {
    return result;
  }
  ++writes_;
  std::vector<TreeOp> host_ops;
  host_ops.reserve(ops.size());
  for (const TreeOp& op : ops) {
    TreeOp host_op = op;
    const auto host_path = map_.ToHost(op.path);
    if (!host_path.has_value()) {
      continue;  // not the project's tree
    }
    host_op.path = *host_path;
    if (!op.new_path.empty()) {
      host_op.new_path = map_.ToHost(op.new_path).value_or(op.new_path);
    }
    // The host has no desktop trash the user can open; what the user sees is that
    // the path is gone, which is a recursive delete.
    if (host_op.kind == TreeOp::Kind::Trash) {
      host_op.kind = TreeOp::Kind::Delete;
      host_op.recursive = true;
    }
    host_ops.push_back(std::move(host_op));
  }
  if (!host_ops.empty()) {
    const TreeResult host_result = project::LocalFileWriteGate().ApplyTreeOps(host_ops);
    project::LocalFileWriteGate().DisposeStaged(host_result.staged_directories);
    if (!host_result.ok) {
      result.ok = false;
      result.error_message = "loopback host replica: " + host_result.error_message;
    }
  }
  return result;
}

void LoopbackWriteGate::DisposeStaged(std::span<const std::filesystem::path> staged) {
  project::LocalFileWriteGate().DisposeStaged(staged);
}

}  // namespace microide::tests::parity

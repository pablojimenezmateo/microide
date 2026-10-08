#include "project/remote/RemoteProject.h"

#include <span>
#include <utility>

#include "persistence/PersistedRecordReader.h"
#include "persistence/PersistedRecordWriter.h"
#include "util/ByteCodec.h"

namespace microide::project::remote {
namespace {

constexpr std::uint64_t kRecordVersion = 1;

std::string TreeName(std::string_view host_root) {
  const std::filesystem::path root{std::string(host_root)};
  std::string name = root.filename().string();
  if (name.empty()) {
    name = root.parent_path().filename().string();
  }
  return name.empty() ? std::string("root") : name;
}

std::filesystem::path RecordPath(const std::filesystem::path& tree) {
  return tree.parent_path() / "meta" / "remote";
}

}  // namespace

std::filesystem::path RemoteProject::DefaultTree(std::string_view host,
                                                 std::string_view host_root) {
  return MirrorStore(MirrorStore::DefaultDirectory(host, host_root), TreeName(host_root)).tree();
}

std::optional<RemoteProjectRecord> RemoteProject::ReadRecord(const std::filesystem::path& tree) {
  if (tree.empty() || !tree.has_parent_path()) {
    return std::nullopt;
  }
  const auto read = persistence::PersistedRecordReader::ReadFile(RecordPath(tree));
  if (!read.has_value()) {
    return std::nullopt;
  }
  const std::string_view body(reinterpret_cast<const char*>(read->body.data()), read->body.size());
  util::ByteReader in(body);
  const std::uint64_t version = in.Varint();
  RemoteProjectRecord record;
  record.host = std::string(in.Bytes(1024));
  record.host_root = std::string(in.Bytes(4096));
  if (in.failed() || version != kRecordVersion || record.host.empty() ||
      record.host_root.empty() || record.host_root.front() != '/') {
    return std::nullopt;
  }
  return record;
}

RemoteProject::RemoteProject(Config config, Listener listener)
    : config_(std::move(config)), listener_(std::move(listener)) {
  const std::string host = config_.session.target.Display();
  const std::string& root = config_.session.workspace_root;
  std::filesystem::path directory = config_.mirror_directory.empty()
                                        ? MirrorStore::DefaultDirectory(host, root)
                                        : config_.mirror_directory;
  store_ = std::make_unique<MirrorStore>(std::move(directory), TreeName(root));
}

RemoteProject::~RemoteProject() {
  // The session first: its worker reports into the engine.
  session_.reset();
  if (engine_) {
    engine_->Flush();
  }
  gate_.reset();
  engine_.reset();
}

RemoteProjectRecord RemoteProject::record() const {
  return RemoteProjectRecord{config_.session.target.Display(), config_.session.workspace_root};
}

bool RemoteProject::Open(std::string* error) {
  if (config_.session.workspace_root.empty() || config_.session.workspace_root.front() != '/') {
    *error = "a remote project's root must be an absolute path on the host";
    return false;
  }
  if (!store_->Open(error)) {
    return false;
  }
  std::string body;
  util::PutVarint(body, kRecordVersion);
  const RemoteProjectRecord mine = record();
  util::PutBytes(body, mine.host);
  util::PutBytes(body, mine.host_root);
  if (!persistence::PersistedRecordWriter::WriteFile(
          RecordPath(store_->tree()),
          std::span<const std::byte>(reinterpret_cast<const std::byte*>(body.data()), body.size()),
          0)) {
    *error = "cannot write " + RecordPath(store_->tree()).string();
    return false;
  }
  session_ = std::make_unique<RemoteHostSession>(
      config_.session, [this](const RemoteHostSession::Status& status) { OnSessionStatus(status); });
  workspace_ = std::make_unique<RemoteWorkspace>(session_->connection());
  engine_ = std::make_unique<MirrorSyncEngine>(*workspace_, *store_, config_.engine,
                                               MirrorSyncEngine::Callbacks{
                                                   .changed =
                                                       [this]() {
                                                         if (listener_) {
                                                           listener_();
                                                         }
                                                       },
                                                   .materialized = {},
                                               });
  gate_ = std::make_unique<MirrorWriteGate>(*store_, *engine_);
  launcher_ = std::make_shared<RemoteProcessLauncher>(
      session_->connection(), RemotePathMap(store_->tree(), config_.session.workspace_root),
      RemoteProcessLauncher::Options{.description = "ssh " + mine.host,
                                     .host_file_cache = host_file_cache()});
  session_->Connect();
  return true;
}

ProjectLocality RemoteProject::locality() const {
  return ProjectLocality{.launcher = launcher_.get(), .write_gate = gate_.get()};
}

void RemoteProject::OnSessionStatus(const RemoteHostSession::Status& status) {
  if (status.state == RemoteHostSession::State::Ready && engine_ && workspace_) {
    // A new connection: resubscribe first, then sync — a change between the two is
    // in the manifest, and nothing is lost before it.
    MirrorSyncEngine* engine = engine_.get();
    std::string error;
    (void)workspace_->SubscribeWatch(
        [engine](RemoteWorkspace::WatchDelta delta) { engine->ApplyWatchDelta(std::move(delta)); },
        nullptr, &error);
    engine_->RequestSync();
  }
  if (listener_) {
    listener_();
  }
}

}  // namespace microide::project::remote

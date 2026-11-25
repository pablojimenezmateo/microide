#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "project/remote/RemoteFrame.h"
#include "project/remote/RemoteManifest.h"
#include "project/remote/RemoteProtocol.h"

namespace microide::project::remote {

class RemoteConnection;
class RemoteServerClient;

class RemoteWorkspace {
 public:
  static constexpr std::size_t kMaxManifestRows = static_cast<std::size_t>(kMaxManifestFilesCeiling);

  struct Manifest {
    std::uint64_t id = 0;
    std::vector<ManifestRow> rows;
    bool git = false;
  };
  using ManifestDone = std::function<void(std::optional<Manifest> manifest, std::string error)>;

  explicit RemoteWorkspace(std::shared_ptr<RemoteConnection> connection)
      : connection_(std::move(connection)) {}

  std::uint64_t FetchManifest(ManifestDone done);
  std::optional<Manifest> FetchManifestSync(std::string* error,
                                            std::chrono::milliseconds timeout = std::chrono::seconds(120));

  struct WatchDelta {
    std::uint64_t manifest_id = 0;
    std::vector<std::string> deleted;
    std::vector<ManifestRow> rows;
    bool repository_moved = false;
  };
  bool SubscribeGitStatus(std::function<void()> changed, std::string* error);
  bool SubscribeWatch(std::function<void(WatchDelta delta)> on_delta, bool* native,
                      std::string* error);

  struct FetchedObject {
    std::string path;
    std::optional<util::ContentHash> hash;
    bool missing = false;
    std::string error;
    std::string content;
    std::filesystem::path staged_file;
    std::optional<util::ContentHash> delta_base;
  };
  static constexpr std::uint64_t kMaxStreamedObjectBytes = 16ull * 1024 * 1024 * 1024;
  static constexpr std::uint64_t kMaxBufferedObjectBytes = 64ull * 1024 * 1024;
  using FetchDone =
      std::function<void(std::optional<std::vector<FetchedObject>> objects, std::string error)>;
  std::uint64_t FetchObjects(std::vector<std::string> paths, Lane lane, FetchDone done,
                             std::uint64_t max_bytes = 0,
                             std::vector<std::optional<util::ContentHash>> bases = {});
  std::uint64_t FetchObjectToFile(std::string path, std::filesystem::path staging, Lane lane,
                                  std::function<void(FetchedObject object, std::string error)> done);
  std::uint64_t ReadHostFile(std::string host_path, FetchDone done);
  std::optional<std::vector<FetchedObject>> FetchObjectsSync(
      std::vector<std::string> paths, Lane lane, std::string* error,
      std::chrono::milliseconds timeout = std::chrono::seconds(120));

  struct WriteResult {
    enum class Status {
      Ok,
      Conflict,
      Error,
    };
    Status status = Status::Error;
    std::optional<util::ContentHash> hash;
    std::string error;
    bool unreachable = false;
    bool may_have_applied = false;
    bool blocked = false;
    std::uint64_t manifest_id = 0;
  };
  using WriteDone = std::function<void(WriteResult result)>;
  std::uint64_t WriteFile(std::string path, std::string_view content, const Precondition& expect,
                          std::optional<std::uint32_t> mode, Lane lane, WriteDone done);
  using UploadDone = std::function<void(std::optional<std::string> host_path, std::string error)>;
  void UploadFile(std::string name, std::string_view content, UploadDone done);
  using StatePutDone = std::function<void(std::string error)>;
  using StateGetDone = std::function<void(std::optional<std::string> text, std::string error)>;
  void PutState(std::string key, std::string text, StatePutDone done);
  using EditDone = std::function<void(std::optional<bool> delivered)>;
  void FinishTerminalEdit(std::string_view epoch, std::uint64_t token, std::optional<std::string> text,
                          EditDone done);
  void GetState(std::string key, StateGetDone done);
  WriteResult WriteFileSync(std::string path, std::string_view content, const Precondition& expect,
                            std::optional<std::uint32_t> mode = std::nullopt,
                            std::chrono::milliseconds timeout = std::chrono::seconds(120));
  enum class TreeOp { MakeDirectory, Rename, Delete };
  std::uint64_t ApplyTreeOp(TreeOp op, std::string path, std::string to, const Precondition& expect,
                            WriteDone done);
  WriteResult ApplyTreeOpSync(TreeOp op, std::string path, std::string to,
                              const Precondition& expect,
                              std::chrono::milliseconds timeout = std::chrono::seconds(120));

 private:
  std::shared_ptr<RemoteServerClient> Client() const;
  std::uint64_t StreamObjects(std::string_view method, util::JsonValue params,
                              std::vector<std::string> names, Lane lane, FetchDone done,
                              std::uint64_t object_limit,
                              std::vector<std::optional<util::ContentHash>> bases = {});

  std::shared_ptr<RemoteConnection> connection_;
};

}

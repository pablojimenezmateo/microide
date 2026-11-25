#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util/TextFileIO.h"

namespace microide::project {

class FileWriteGate {
 public:
  struct Result {
    bool ok = false;
    util::FileSignature signature;
    std::error_code error;
  };

  enum class Signature { Skip, Capture };

  virtual ~FileWriteGate() = default;

  [[nodiscard]] virtual Result WriteText(const std::filesystem::path& path,
                           std::string_view text,
                           Signature signature = Signature::Skip,
                           util::WriteTarget target = util::WriteTarget::Replace) = 0;

  struct TreeOp {
    enum class Kind : std::uint8_t {
      CreateFile,
      CreateDirectory,
      Rename,
      Delete,
      Trash,
      Copy,
    };
    Kind kind = Kind::CreateFile;
    std::filesystem::path path;
    std::filesystem::path new_path;
    bool overwrite = false;
    bool recursive = false;
  };

  struct TreeResult {
    bool ok = false;
    std::size_t failed_index = 0;
    std::string error_message;
    std::filesystem::path resulting_path;
    std::vector<std::filesystem::path> staged_directories;
  };

  [[nodiscard]] virtual TreeResult ApplyTreeOps(std::span<const TreeOp> ops) = 0;

  virtual void DisposeStaged(std::span<const std::filesystem::path> staged) = 0;

  [[nodiscard]] virtual bool IsReadOnly(const std::filesystem::path& path) const;
  [[nodiscard]] virtual bool MakeWritable(const std::filesystem::path& path);

  [[nodiscard]] TreeResult ApplyTreeOp(const TreeOp& op) {
    return ApplyTreeOps(std::span<const TreeOp>(&op, 1));
  }
};

FileWriteGate& LocalFileWriteGate();

FileWriteGate::Result WriteTextLocally(const std::filesystem::path& path, std::string_view text,
                                       FileWriteGate::Signature signature, util::WriteTarget target,
                                       const std::filesystem::path& staging);

}

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace microide::workspace {

struct DiffSurfacePaint;

struct CompareRenderProjectInputs {
  const DiffSurfacePaint* paint = nullptr;
};


struct CompareInput {
  std::string content;
  std::string label;
  std::filesystem::path path;
  bool editable = false;
};

std::optional<CompareInput> ReadFileCompareInput(const std::filesystem::path& path, bool editable);

}

#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <utility>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "render/PluginDisplayList.h"
#include "util/TransparentStringHash.h"

namespace microide::editor {

enum class SurfacePreviewSlot : std::uint8_t { None, Bottom, Side };

struct RasterHandle {
  std::uint64_t content_hash = 0;
  int width = 0;
  int height = 0;

  bool operator==(const RasterHandle&) const = default;
};

struct SurfaceHitRegion {
  SDL_FRect rect{0.0f, 0.0f, 0.0f, 0.0f};
  std::string command;

  bool operator==(const SurfaceHitRegion& o) const {
    return rect.x == o.rect.x && rect.y == o.rect.y && rect.w == o.rect.w &&
           rect.h == o.rect.h && command == o.command;
  }
};

struct SurfaceAnchor {
  std::filesystem::path path;
  std::uint32_t line = 0;

  bool operator==(const SurfaceAnchor&) const = default;
};

struct SurfaceContent {
  std::variant<std::monostate, render::PluginDisplayList, RasterHandle> body;
  float intrinsic_width = 0.0f;
  float intrinsic_height = 0.0f;
  std::string title;
  std::optional<SurfaceAnchor> anchor;
  std::vector<SurfaceHitRegion> hit_regions;
  SurfacePreviewSlot preview = SurfacePreviewSlot::None;

  bool has_body() const { return body.index() != 0; }
  bool operator==(const SurfaceContent&) const = default;
};

bool ApplyRasterSizeTo(SurfaceContent& content, std::uint64_t content_hash, int width,
                       int height);

struct AnchoredSurface {
  std::uint32_t line = 0;
  std::string owner;
  std::string surface_id;
  const SurfaceContent* content = nullptr;
};

struct SurfaceRef {
  std::string owner;
  std::string surface_id;
  const SurfaceContent* content = nullptr;
};

class PluginSurfaceStore {
 public:
  bool ReplaceForOwnerSurface(std::string_view owner, std::string_view surface_id,
                              SurfaceContent content);
  bool ClearOwnerSurface(std::string_view owner, std::string_view surface_id);
  bool ClearOwner(std::string_view owner);
  bool SetPreviewSlot(std::string_view owner, std::string_view surface_id,
                      SurfacePreviewSlot slot);

  const SurfaceContent* Find(std::string_view owner, std::string_view surface_id) const;

  const std::vector<SurfaceRef>& PreviewSurfaces() const;

  std::span<const AnchoredSurface> AnchoredSurfacesForPath(
      const std::filesystem::path& path) const;
  std::span<const AnchoredSurface> AnchoredSurfacesForPathKey(std::string_view path_key) const;

  bool RetargetPathPrefix(const std::filesystem::path& old_prefix,
                          const std::filesystem::path& new_prefix);
  bool ClearPathPrefix(const std::filesystem::path& path_prefix);

  bool ApplyRasterSize(std::uint64_t content_hash, int width, int height);

  bool has_anchored() const { return !anchored_by_path_.empty(); }
  std::uint64_t revision() const { return revision_; }
  bool empty() const { return by_owner_.empty(); }

 private:
  static std::string PathKey(const std::filesystem::path& path);
  void RebuildAnchorIndex();

  static constexpr std::size_t kMaxSurfacesPerOwner = 8192;

  using SurfaceMap = std::unordered_map<std::string, SurfaceContent,
                                        util::TransparentStringHash, std::equal_to<>>;

  std::unordered_map<std::string, SurfaceMap, util::TransparentStringHash, std::equal_to<>>
      by_owner_;
  std::unordered_map<std::string, std::vector<AnchoredSurface>, util::TransparentStringHash,
                     std::equal_to<>>
      anchored_by_path_;
  std::set<std::pair<std::string, std::string>> dismissed_previews_;
  std::uint64_t revision_ = 0;
  mutable std::vector<SurfaceRef> preview_cache_;
  mutable std::uint64_t preview_cache_revision_ = ~0ull;
};

}

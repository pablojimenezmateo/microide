#include "workspace/WorkspaceFileIconRegistry.h"

#include <array>

#include "editor/GutterIconRegistry.h"
#include "plugin/PluginHost.h"
#include "util/StringUtil.h"

namespace microide::workspace {

namespace {

using editor::GutterIconShape;

// Built-in extension → icon defaults. Colours are deliberately muted so the
// file-tree reads as type-coded accents, not a rainbow. Keyed by lower-cased
// extension without the leading dot.
struct BuiltinRule {
  std::string_view ext;
  GutterIconShape shape;
  SDL_Color color;
};

constexpr SDL_Color kBlue{0x4f, 0x9c, 0xff, 0xff};
constexpr SDL_Color kGreen{0x6f, 0xc1, 0x6f, 0xff};
constexpr SDL_Color kAmber{0xe2, 0xb4, 0x55, 0xff};
constexpr SDL_Color kGray{0x9a, 0xa3, 0xb0, 0xff};
constexpr SDL_Color kPurple{0xb1, 0x86, 0xe0, 0xff};
constexpr SDL_Color kRed{0xe0, 0x6c, 0x6c, 0xff};

constexpr std::array<BuiltinRule, 34> kBuiltins{{
    {"c", GutterIconShape::Dot, kBlue},
    {"h", GutterIconShape::Dot, kBlue},
    {"cc", GutterIconShape::Dot, kBlue},
    {"cpp", GutterIconShape::Dot, kBlue},
    {"cxx", GutterIconShape::Dot, kBlue},
    {"hpp", GutterIconShape::Dot, kBlue},
    {"hh", GutterIconShape::Dot, kBlue},
    {"py", GutterIconShape::Dot, kGreen},
    {"lua", GutterIconShape::Dot, kBlue},
    {"rs", GutterIconShape::Dot, kAmber},
    {"go", GutterIconShape::Dot, kBlue},
    {"js", GutterIconShape::Dot, kAmber},
    {"jsx", GutterIconShape::Dot, kAmber},
    {"ts", GutterIconShape::Dot, kBlue},
    {"tsx", GutterIconShape::Dot, kBlue},
    {"json", GutterIconShape::Diamond, kAmber},
    {"yaml", GutterIconShape::Diamond, kAmber},
    {"yml", GutterIconShape::Diamond, kAmber},
    {"toml", GutterIconShape::Diamond, kAmber},
    {"ini", GutterIconShape::Diamond, kGray},
    {"xml", GutterIconShape::Diamond, kGreen},
    {"md", GutterIconShape::Triangle, kGray},
    {"markdown", GutterIconShape::Triangle, kGray},
    {"rst", GutterIconShape::Triangle, kGray},
    {"txt", GutterIconShape::Triangle, kGray},
    {"png", GutterIconShape::Square, kPurple},
    {"jpg", GutterIconShape::Square, kPurple},
    {"jpeg", GutterIconShape::Square, kPurple},
    {"gif", GutterIconShape::Square, kPurple},
    {"svg", GutterIconShape::Square, kPurple},
    {"webp", GutterIconShape::Square, kPurple},
    {"ico", GutterIconShape::Square, kPurple},
    {"sh", GutterIconShape::Dash, kGreen},
    {"bash", GutterIconShape::Dash, kGreen},
}};

std::optional<WorkspaceFileIconRegistry::Icon> ResolveBuiltinExtension(std::string_view ext) {
  for (const BuiltinRule& rule : kBuiltins) {
    if (rule.ext == ext) {
      return WorkspaceFileIconRegistry::Icon{rule.shape, rule.color};
    }
  }
  return std::nullopt;
}

// Lower-cased extension (without the dot) of a filename, or empty when none. A
// leading dot (dotfile) is not treated as an extension separator.
std::string ExtensionOf(std::string_view filename) {
  const std::size_t dot = filename.rfind('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 >= filename.size()) {
    return {};
  }
  return util::ToLowerAscii(filename.substr(dot + 1));
}

}  // namespace

void WorkspaceFileIconRegistry::Clear() {
  by_name_.clear();
  by_extension_.clear();
}

void WorkspaceFileIconRegistry::Rebuild(const plugin::PluginHost& host) {
  by_name_.clear();
  by_extension_.clear();
  for (const auto& theme : host.ContributedFileIconThemes()) {
    for (const auto& rule : theme.rules) {
      const auto shape = editor::GutterIconRegistry::ResolveShape(rule.icon);
      if (!shape || rule.matcher.empty()) {
        continue;  // unknown shape name → ignore the rule (already lower-cased)
      }
      const Icon icon{*shape, rule.color};
      if (rule.match_filename) {
        by_name_[rule.matcher] = icon;
      } else {
        by_extension_[rule.matcher] = icon;
      }
    }
  }
}

std::optional<WorkspaceFileIconRegistry::Icon> WorkspaceFileIconRegistry::Resolve(
    std::string_view filename) const {
  if (filename.empty()) {
    return std::nullopt;
  }
  const std::string lower_name = util::ToLowerAscii(filename);
  if (const auto it = by_name_.find(lower_name); it != by_name_.end()) {
    return it->second;
  }
  const std::string ext = ExtensionOf(lower_name);
  if (!ext.empty()) {
    if (const auto it = by_extension_.find(ext); it != by_extension_.end()) {
      return it->second;
    }
    if (auto builtin = ResolveBuiltinExtension(ext)) {
      return builtin;
    }
  }
  return std::nullopt;
}

}  // namespace microide::workspace

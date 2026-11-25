#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <utility>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "workspace/assist/AssistHost.h"
#include "workspace/assist/AssistProviderMerge.h"

namespace microide::workspace {

struct AssistServiceTestAccess;

class NavigationAssist {
 public:
  struct Port {
    std::function<bool(const std::filesystem::path&)> open_file_in_new_tab;
    std::function<bool(const std::filesystem::path&, std::function<void()> then)> open_file_then;
  };

  explicit NavigationAssist(AssistHost& host) : host_(host) {}
  void Configure(Port port) { port_ = std::move(port); }

  bool GoToDefinition(std::string* error_message = nullptr);
  bool GoToTypeDefinition(std::string* error_message = nullptr);
  bool GoToImplementation(std::string* error_message = nullptr);
  bool GoToDeclaration(std::string* error_message = nullptr);
  bool FindReferences(std::string* error_message = nullptr);

  struct WorkspaceSymbolHit {
    std::string name;
    std::string container;
    std::filesystem::path path;
    std::size_t line = 0;
    int character = 0;
  };
  using WorkspaceSymbolsReady = std::function<void(std::vector<WorkspaceSymbolHit>)>;
  bool QueryWorkspaceSymbols(const std::string& query, WorkspaceSymbolsReady deliver,
                             std::string* error_message = nullptr);
  void OpenWorkspaceSymbol(const std::filesystem::path& path, std::size_t line, int character);

  bool ShowCallHierarchy(bool incoming, std::string* error_message = nullptr);

  friend struct AssistServiceTestAccess;

 private:
  struct Merge {
    assist_merge::TwoSourceState sources;
    lsp_encoding::PositionEncoding lsp_encoding = lsp_encoding::PositionEncoding::Utf8;
    std::vector<plugin::PluginHost::LocationResult> plugin_locations;
    std::vector<LspClient::Location> lsp_locations;
    bool acted = false;
    std::uint64_t generation = 0;
  };

  enum class LocationQuery { Definition, References };
  bool QueryLocations(LocationQuery query, std::string* error_message);
  void ResolveDefinition(const std::shared_ptr<Merge>& merge,
                         const std::filesystem::path& request_path);
  void PublishReferences(const std::shared_ptr<Merge>& merge,
                         const std::filesystem::path& request_path);

  enum class LspNavigationKind { TypeDefinition, Implementation, Declaration };
  bool GoToLspNavigation(LspNavigationKind kind, std::string* error_message);

  void OpenThen(const std::filesystem::path& path, std::function<void()> then);
  void NavigateToPluginLocation(const plugin::PluginHost::LocationResult& location);
  void NavigateToLspLocation(const LspClient::Location& location,
                             lsp_encoding::PositionEncoding encoding);

  struct ReferenceColumn {
    enum class Units { PluginByte1Based, LspCodeUnit0Based };
    std::size_t value = 0;
    Units units = Units::PluginByte1Based;
    lsp_encoding::PositionEncoding encoding = lsp_encoding::PositionEncoding::Utf8;

    static ReferenceColumn PluginByte(std::size_t column) {
      return ReferenceColumn{.value = column, .units = Units::PluginByte1Based};
    }
    static ReferenceColumn LspCharacter(int character, lsp_encoding::PositionEncoding encoding) {
      return ReferenceColumn{.value = static_cast<std::size_t>(std::max(0, character)),
                             .units = Units::LspCodeUnit0Based,
                             .encoding = encoding};
    }
  };
  void EmitReferenceEntry(const char* channel_id, const char* channel_title,
                          const std::filesystem::path& path, std::size_t line,
                          ReferenceColumn column, bool append_separator,
                          std::map<std::filesystem::path, std::vector<std::string>>&
                              file_line_cache) const;

  AssistHost& host_;
  Port port_;
  std::uint64_t navigation_generation_ = 0;
  std::uint64_t references_generation_ = 0;
  std::uint64_t workspace_symbol_generation_ = 0;
  lsp_encoding::PositionEncoding workspace_symbol_encoding_ = lsp_encoding::PositionEncoding::Utf16;
  std::uint64_t call_hierarchy_generation_ = 0;
};

}

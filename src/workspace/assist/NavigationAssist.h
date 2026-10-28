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

// Everything that answers "where is this symbol": go to definition / type
// definition / implementation / declaration, find references, the call
// hierarchy, and workspace symbols.
class NavigationAssist {
 public:
  // The shell callbacks only this feature reads (AssistHost::Operations has the
  // shared ones).
  struct Port {
    std::function<bool(const std::filesystem::path&)> open_file_in_new_tab;
    // Open `path` and then run `then` on the opened tab — now, or later when the
    // file must first come from the host (a remote project's out-of-project file).
    // False when it will not open at all.
    std::function<bool(const std::filesystem::path&, std::function<void()> then)> open_file_then;
  };

  explicit NavigationAssist(AssistHost& host) : host_(host) {}
  void Configure(Port port) { port_ = std::move(port); }

  bool GoToDefinition(std::string* error_message = nullptr);
  // LSP-only navigation to the type/interface, implementation(s), or declaration of
  // the symbol under the cursor (textDocument/typeDefinition|implementation|
  // declaration). Jumps to the first location returned.
  bool GoToTypeDefinition(std::string* error_message = nullptr);
  bool GoToImplementation(std::string* error_message = nullptr);
  bool GoToDeclaration(std::string* error_message = nullptr);
  bool FindReferences(std::string* error_message = nullptr);

  // Search Workspace Symbols (Ctrl+T): workspace/symbol for `query` from the active
  // buffer's server, the hits delivered on a later drain -- unless a newer query has
  // been asked since, whose answer is the only one delivered. False, with why, when
  // there is no server to ask.
  struct WorkspaceSymbolHit {
    std::string name;
    std::string container;
    std::filesystem::path path;
    std::size_t line = 0;  // 0-based
    int character = 0;     // the server's, in its position encoding
  };
  using WorkspaceSymbolsReady = std::function<void(std::vector<WorkspaceSymbolHit>)>;
  bool QueryWorkspaceSymbols(const std::string& query, WorkspaceSymbolsReady deliver,
                             std::string* error_message = nullptr);
  // Open a hit's file and put the caret on it (its character converted from the
  // encoding of the server that answered).
  void OpenWorkspaceSymbol(const std::filesystem::path& path, std::size_t line, int character);

  // Render the call hierarchy of the symbol under the caret into the
  // `lsp.callHierarchy` output channel: who calls it (`incoming`) or what it calls.
  // Two round-trips — textDocument/prepareCallHierarchy resolves the symbol, then
  // callHierarchy/{incoming,outgoing}Calls walks one level of edges. One level, not
  // a tree: the channel is a flat navigable list, and an expandable tree would need
  // a surface this shell does not have. Entries reuse the references formatter, so
  // each is a clickable file:line:col with context.
  bool ShowCallHierarchy(bool incoming, std::string* error_message = nullptr);

  friend struct AssistServiceTestAccess;

 private:
  struct Merge {
    assist_merge::TwoSourceState sources;
    lsp_encoding::PositionEncoding lsp_encoding = lsp_encoding::PositionEncoding::Utf8;
    std::vector<plugin::PluginHost::LocationResult> plugin_locations;
    std::vector<LspClient::Location> lsp_locations;
    bool acted = false;
    // Per-surface request generation (navigation vs references — see the
    // *_generation_ counters). A callback carries the generation it was
    // dispatched under and drops its result when a newer same-surface request (e.g.
    // a second Go-To-Definition at a different caret in the same file before the
    // first response lands) has bumped the counter. TD-2026-07-17A-030.
    std::uint64_t generation = 0;
  };

  // Navigate once the preferred source resolves (go-to-definition). Waits for the
  // language server when it serves the buffer, else uses the plugin result.
  void ResolveDefinition(const std::shared_ptr<Merge>& merge,
                         const std::filesystem::path& request_path);
  // Render the merged reference set (both sources, de-duplicated by location)
  // into the References output channel once both sources have resolved.
  void PublishReferences(const std::shared_ptr<Merge>& merge,
                         const std::filesystem::path& request_path);

  enum class LspNavigationKind { TypeDefinition, Implementation, Declaration };
  bool GoToLspNavigation(LspNavigationKind kind, std::string* error_message);

  // Open `path`, then run `then` (now, or once a host file has been fetched).
  void OpenThen(const std::filesystem::path& path, std::function<void()> then);
  // Open a plugin-provided navigation target (1-based line/column) and move the
  // caret there.
  void NavigateToPluginLocation(const plugin::PluginHost::LocationResult& location);
  // Open an LSP Location (0-based, server position encoding) and move the caret
  // there. Shared by definition, the type/impl/decl navigations and symbols.
  void NavigateToLspLocation(const LspClient::Location& location,
                             lsp_encoding::PositionEncoding encoding);

  // The column of a reference entry, in whichever units its source spoke. A plugin
  // provider reports a 1-based BYTE column (Lua strings are bytes, and that is
  // what the host hands it); a language server reports a 0-based offset in its
  // negotiated position encoding (bytes for clangd and rust-analyzer, UTF-16
  // units by default). Either is only a character column on an ASCII line. The
  // emitter resolves both through the target line's text, so the printed
  // `path:line:col` names the character the Output click handler then jumps to.
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
  // Emit one reference entry (file:line:column header + the ±1-line context block,
  // plus a trailing blank when `append_separator`) into the given channel,
  // caching file contents in `file_line_cache`. `line` is 1-based.
  void EmitReferenceEntry(const char* channel_id, const char* channel_title,
                          const std::filesystem::path& path, std::size_t line,
                          ReferenceColumn column, bool append_separator,
                          std::map<std::filesystem::path, std::vector<std::string>>&
                              file_line_cache) const;

  AssistHost& host_;
  Port port_;
  // Cursor-jump navigation (definition / type-def / impl / declaration): one
  // counter, since the user only ever jumps once — any new navigation supersedes a
  // pending one. References are an independent surface.
  std::uint64_t navigation_generation_ = 0;
  std::uint64_t references_generation_ = 0;
  // workspace/symbol has no active-file cursor to anchor a staleness check on (it is
  // project-wide), so a slower response for an OLDER query could otherwise clear the
  // channel and render its results over the newer query's. Gate it on a generation
  // token like completions/code actions do (TD-2026-07-17A-034).
  std::uint64_t workspace_symbol_generation_ = 0;
  lsp_encoding::PositionEncoding workspace_symbol_encoding_ = lsp_encoding::PositionEncoding::Utf16;
  // Call hierarchy spans two chained round-trips, so a second invocation while the
  // first is mid-chain would otherwise let the older chain finish last and render
  // over the newer one. Both hops carry this token.
  std::uint64_t call_hierarchy_generation_ = 0;
};

}  // namespace microide::workspace

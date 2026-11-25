#include "workspace/assist/NavigationAssist.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "util/PathMatch.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"
#include "workspace/FileUri.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/lsp/LspViewportPositions.h"
#include "util/ErrorOut.h"

namespace microide::workspace {

namespace {

constexpr std::uintmax_t kReferenceSnippetWholeReadBytes = 256ull * 1024;

constexpr std::uintmax_t kReferenceSnippetScanBytes = 2ull * 1024 * 1024;

constexpr const char* kDefinitionChannel = "lsp.definition";
constexpr const char* kDefinitionTitle = "LSP Definition";

}

bool NavigationAssist::GoToDefinition(std::string* error_message) {
  return QueryLocations(LocationQuery::Definition, error_message);
}

bool NavigationAssist::FindReferences(std::string* error_message) {
  return QueryLocations(LocationQuery::References, error_message);
}

bool NavigationAssist::QueryLocations(LocationQuery query, std::string* error_message) {
  util::ClearError(error_message);
  editor::TextViewport* viewport = host_.RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }

  const bool references = query == LocationQuery::References;
  const std::string language_id = viewport->language_id();
  const std::filesystem::path request_path = viewport->path();
  const std::size_t request_line = viewport->cursor_line();
  const std::size_t request_column = viewport->cursor_column();

  auto merge = std::make_shared<Merge>();
  merge->generation = references ? ++references_generation_ : ++navigation_generation_;
  const auto settle = [this, query](const std::shared_ptr<Merge>& settled,
                                    const std::filesystem::path& path) {
    if (query == LocationQuery::References) {
      PublishReferences(settled, path);
    } else {
      ResolveDefinition(settled, path);
    }
  };

  LspClient* client = host_.ops().lsp_client_for_viewport(*viewport, nullptr);
  merge->sources.lsp_authoritative = client != nullptr;
  if (client != nullptr) {
    host_.ops().ensure_lsp_document_open(*viewport, *client, language_id);
    host_.ops().begin_tracked_lsp_request();
    merge->lsp_encoding = LspEncodingForClient(*client);
    auto on_lsp = [this, request_path, merge,
                   settle](LspResult<std::vector<LspClient::Location>> locations) {
      host_.ops().finish_tracked_lsp_request();
      merge->sources.lsp_pending = false;
      merge->sources.lsp_failed = !locations.answered();
      if (locations.has_value()) {
        merge->lsp_locations = std::move(*locations);
      }
      settle(merge, request_path);
    };
    const std::string uri = FileUriForPath(request_path);
    const LspClient::Position position =
        ByteColumnToLspPosition(*viewport, request_line, request_column, merge->lsp_encoding);
    if (references) {
      client->RequestFindReferencesAsync(uri, position, true, std::move(on_lsp));
    } else {
      client->RequestGoToDefinitionAsync(uri, position, std::move(on_lsp));
    }
  } else {
    merge->sources.lsp_pending = false;
  }

  auto on_plugin = [request_path, merge, settle](
                       std::vector<plugin::PluginHost::LocationResult> locations,
                       std::string ) {
    merge->sources.plugin_pending = false;
    merge->plugin_locations = std::move(locations);
    settle(merge, request_path);
  };
  if (references) {
    host_.plugins().Host().QueryReferencesAsync(language_id, request_path, request_line + 1,
                                                request_column + 1, true, std::move(on_plugin));
  } else {
    host_.plugins().Host().QueryDefinitionAsync(language_id, request_path, request_line + 1,
                                                request_column + 1, std::move(on_plugin));
  }
  return true;
}

void NavigationAssist::ResolveDefinition(const std::shared_ptr<Merge>& merge,
                                                const std::filesystem::path& request_path) {
  if (merge->acted) {
    return;
  }
  if (merge->generation != navigation_generation_ ||
      host_.IsStale(request_path)) {
    return;
  }
  const assist_merge::NavChoice choice = assist_merge::ChooseNavigation(
      merge->sources.lsp_authoritative, merge->sources.lsp_pending,
      !merge->lsp_locations.empty(), merge->sources.plugin_pending,
      !merge->plugin_locations.empty());
  switch (choice) {
    case assist_merge::NavChoice::Pending:
      return;
    case assist_merge::NavChoice::None: {
      merge->acted = true;
      const char* message = merge->sources.lsp_failed
                                ? "Language server did not respond (no definition resolved)"
                                : "No definition found";
      host_.Report(kDefinitionChannel, kDefinitionTitle, message);
      return;
    }
    case assist_merge::NavChoice::UsePlugin:
      merge->acted = true;
      NavigateToPluginLocation(merge->plugin_locations.front());
      return;
    case assist_merge::NavChoice::UseLsp:
      merge->acted = true;
      NavigateToLspLocation(merge->lsp_locations.front(), merge->lsp_encoding);
      return;
  }
}

void NavigationAssist::NavigateToLspLocation(const LspClient::Location& location,
                                          lsp_encoding::PositionEncoding encoding) {
  const std::optional<std::filesystem::path> path = PathFromFileUri(location.uri);
  if (!path.has_value()) {
    return;
  }
  OpenThen(*path, [this, location, encoding]() {
    if (editor::TextViewport* active = host_.ops().active_editor_viewport(); active != nullptr) {
      const std::size_t target_line =
          static_cast<std::size_t>(std::max(location.range.start.line, 0));
      active->JumpCursorTo(
          target_line,
          LspPositionToByteColumn(*active, target_line, location.range.start.character, encoding));
      host_.ops().reset_caret_blink();
      host_.ops().request_frame();
    }
  });
}

void NavigationAssist::OpenThen(const std::filesystem::path& path, std::function<void()> then) {
  if (port_.open_file_then) {
    (void)port_.open_file_then(path, std::move(then));
    return;
  }
  if (port_.open_file_in_new_tab(path)) {
    then();
  }
}

bool NavigationAssist::GoToLspNavigation(LspNavigationKind kind, std::string* error_message) {
  util::ClearError(error_message);
  editor::TextViewport* viewport = host_.RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }
  LspClient* client = host_.PrepareLspRequest(*viewport, error_message);
  if (client == nullptr) {
    return false;
  }
  const std::filesystem::path request_path = viewport->path();
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
  const LspClient::Position position = ByteColumnToLspPosition(
      *viewport, viewport->cursor_line(), viewport->cursor_column(), encoding);
  const char* empty_message = kind == LspNavigationKind::TypeDefinition ? "No type definition found"
                              : kind == LspNavigationKind::Implementation ? "No implementation found"
                                                                          : "No declaration found";
  const std::uint64_t request_generation = ++navigation_generation_;
  auto on_result = [this, request_path, encoding, empty_message, request_generation](
                       LspResult<std::vector<LspClient::Location>> locations) {
    host_.ops().finish_tracked_lsp_request();
    if (request_generation != navigation_generation_ ||
        host_.IsStale(request_path)) {
      return;
    }
    if (!locations.answered() || !locations.has_value() || locations->empty()) {
      const char* message = locations.answered() ? empty_message : "Language server did not respond";
      host_.Report(kDefinitionChannel, kDefinitionTitle, message);
      return;
    }
    NavigateToLspLocation(locations->front(), encoding);
  };
  const std::string uri = FileUriForPath(request_path);
  switch (kind) {
    case LspNavigationKind::TypeDefinition:
      client->RequestGoToTypeDefinitionAsync(uri, position, std::move(on_result));
      break;
    case LspNavigationKind::Implementation:
      client->RequestGoToImplementationAsync(uri, position, std::move(on_result));
      break;
    case LspNavigationKind::Declaration:
      client->RequestGoToDeclarationAsync(uri, position, std::move(on_result));
      break;
  }
  return true;
}

bool NavigationAssist::GoToTypeDefinition(std::string* error_message) {
  return GoToLspNavigation(LspNavigationKind::TypeDefinition, error_message);
}

bool NavigationAssist::GoToImplementation(std::string* error_message) {
  return GoToLspNavigation(LspNavigationKind::Implementation, error_message);
}

bool NavigationAssist::GoToDeclaration(std::string* error_message) {
  return GoToLspNavigation(LspNavigationKind::Declaration, error_message);
}

void NavigationAssist::PublishReferences(const std::shared_ptr<Merge>& merge,
                                          const std::filesystem::path& request_path) {
  if (merge->acted || merge->sources.AnyPending()) {
    return;
  }
  merge->acted = true;
  if (merge->generation != references_generation_ ||
      host_.IsStale(request_path)) {
    return;
  }

  struct RefTarget {
    std::filesystem::path path;
    std::size_t line = 0;
    ReferenceColumn column;
  };
  std::vector<RefTarget> lsp_targets;
  lsp_targets.reserve(merge->lsp_locations.size());
  for (const auto& location : merge->lsp_locations) {
    const std::optional<std::filesystem::path> path = PathFromFileUri(location.uri);
    if (!path.has_value()) {
      continue;
    }
    lsp_targets.push_back(
        RefTarget{*path, static_cast<std::size_t>(std::max(location.range.start.line, 0)) + 1,
                  ReferenceColumn::LspCharacter(location.range.start.character,
                                                merge->lsp_encoding)});
  }
  std::vector<RefTarget> plugin_targets;
  plugin_targets.reserve(merge->plugin_locations.size());
  for (const auto& location : merge->plugin_locations) {
    if (location.path.empty()) {
      continue;
    }
    plugin_targets.push_back(
        RefTarget{location.path, location.line, ReferenceColumn::PluginByte(location.column)});
  }

  const auto& primary = merge->sources.lsp_authoritative ? lsp_targets : plugin_targets;
  const auto& secondary = merge->sources.lsp_authoritative ? plugin_targets : lsp_targets;
  const std::vector<RefTarget> merged =
      assist_merge::RankedUnion(primary, secondary, [](const RefTarget& target) {
        const std::size_t units =
            target.column.units == ReferenceColumn::Units::LspCodeUnit0Based
                ? target.column.value + 1
                : target.column.value;
        return target.path.generic_string() + ":" + std::to_string(target.line) + ":" +
               std::to_string(units);
      });

  host_.output().Clear("lsp.references");
  if (merged.empty()) {
    host_.output().AppendLine(
        "lsp.references", "LSP References",
        merge->sources.lsp_failed ? "Language server did not respond (no references resolved)"
                                  : "No references found");
    return;
  }
  std::map<std::filesystem::path, std::vector<std::string>> file_line_cache;
  for (std::size_t index = 0; index < merged.size(); ++index) {
    EmitReferenceEntry("lsp.references", "LSP References", merged[index].path, merged[index].line,
                       merged[index].column, index + 1 < merged.size(), file_line_cache);
  }
}

bool NavigationAssist::ShowCallHierarchy(bool incoming, std::string* error_message) {
  util::ClearError(error_message);
  editor::TextViewport* viewport = host_.RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }
  LspClient* client = host_.PrepareLspRequest(*viewport, error_message);
  if (client == nullptr) {
    return false;
  }

  static constexpr const char* kChannelId = "lsp.callHierarchy";
  const char* const title = incoming ? "Incoming Calls" : "Outgoing Calls";
  if (!client->SupportsCallHierarchy()) {
    host_.ops().finish_tracked_lsp_request();
    host_.output().Clear(kChannelId);
    host_.output().AppendLine(kChannelId, title,
                                 "This language server does not provide call hierarchy");
    return true;
  }
  const std::uint64_t generation = ++call_hierarchy_generation_;
  const std::size_t request_line = viewport->cursor_line();
  const std::size_t request_column = viewport->cursor_column();
  host_.output().Clear(kChannelId);
  host_.output().AppendLine(kChannelId, title, "Resolving call hierarchy…");

  const auto finish_empty = [this, title](const char* message) {
    host_.output().Clear(kChannelId);
    host_.output().AppendLine(kChannelId, title, message);
  };

  client->RequestPrepareCallHierarchyAsync(
      FileUriForPath(viewport->path()),
      ByteColumnToLspPosition(*viewport, request_line, request_column,
                              LspEncodingForClient(*client)),
      [this, client, incoming, title, generation, finish_empty](
          LspResult<std::vector<LspClient::CallHierarchyItem>> items) {
        if (generation != call_hierarchy_generation_) {
          host_.ops().finish_tracked_lsp_request();
          return;
        }
        if (!items.answered()) {
          host_.ops().finish_tracked_lsp_request();
          finish_empty("Language server did not respond");
          return;
        }
        if (!items.has_value() || items->empty()) {
          host_.ops().finish_tracked_lsp_request();
          finish_empty("No callable symbol at the cursor");
          return;
        }
        const LspClient::CallHierarchyItem& root = items->front();
        std::string root_label = root.name;
        if (!root.detail.empty()) {
          root_label += "  ·  " + root.detail;
        }
        const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
        const auto render = [this, title, root_label, incoming, generation, finish_empty,
                             encoding](
                                LspResult<std::vector<LspClient::CallHierarchyCall>> calls) {
          host_.ops().finish_tracked_lsp_request();
          if (generation != call_hierarchy_generation_) {
            return;
          }
          if (!calls.answered()) {
            finish_empty("Language server did not respond");
            return;
          }
          if (!calls.has_value() || calls->empty()) {
            finish_empty(incoming ? "No callers found" : "No calls found");
            return;
          }
          host_.output().Clear(kChannelId);
          host_.output().AppendLine(
              kChannelId, title,
              (incoming ? "Callers of " : "Called by ") + root_label);
          std::map<std::filesystem::path, std::vector<std::string>> file_line_cache;
          for (std::size_t i = 0; i < calls->size(); ++i) {
            const LspClient::CallHierarchyCall& call = (*calls)[i];
            const std::optional<std::filesystem::path> path = PathFromFileUri(call.item.uri);
            if (!path.has_value()) {
              continue;
            }
            std::string header = call.item.name;
            if (!call.item.detail.empty()) {
              header += "  ·  " + call.item.detail;
            }
            host_.output().AppendLine(kChannelId, title, header);
            const bool last = i + 1 >= calls->size();
            if (incoming && !call.call_ranges.empty()) {
              for (std::size_t r = 0; r < call.call_ranges.size(); ++r) {
                const LspClient::Range& range = call.call_ranges[r];
                EmitReferenceEntry(
                    kChannelId, title, *path,
                    static_cast<std::size_t>(std::max(range.start.line, 0)) + 1,
                    ReferenceColumn::LspCharacter(range.start.character, encoding),
                    last && r + 1 >= call.call_ranges.size(), file_line_cache);
              }
              continue;
            }
            EmitReferenceEntry(
                kChannelId, title, *path,
                static_cast<std::size_t>(std::max(call.item.selection_range.start.line, 0)) + 1,
                ReferenceColumn::LspCharacter(call.item.selection_range.start.character,
                                              encoding),
                !last, file_line_cache);
          }
        };
        if (incoming) {
          client->RequestIncomingCallsAsync(root.raw, render);
        } else {
          client->RequestOutgoingCallsAsync(root.raw, render);
        }
      });
  return true;
}

bool NavigationAssist::QueryWorkspaceSymbols(const std::string& query, WorkspaceSymbolsReady deliver,
                                          std::string* error_message) {
  util::ClearError(error_message);
  editor::TextViewport* viewport = host_.RequireActiveEditableViewport(error_message);
  if (viewport == nullptr) {
    return false;
  }
  LspClient* client = host_.PrepareLspRequest(*viewport, error_message);
  if (client == nullptr) {
    return false;
  }
  const std::uint64_t request_generation = ++workspace_symbol_generation_;
  workspace_symbol_encoding_ = LspEncodingForClient(*client);
  client->RequestWorkspaceSymbolAsync(
      query, [this, request_generation, deliver = std::move(deliver)](
                 LspResult<std::vector<LspClient::WorkspaceSymbol>> symbols) {
        host_.ops().finish_tracked_lsp_request();
        if (request_generation != workspace_symbol_generation_) {
          return;
        }
        std::vector<WorkspaceSymbolHit> hits;
        if (symbols.has_value()) {
          hits.reserve(symbols->size());
          for (const LspClient::WorkspaceSymbol& symbol : *symbols) {
            std::optional<std::filesystem::path> path = PathFromFileUri(symbol.location.uri);
            if (!path.has_value()) {
              continue;
            }
            hits.push_back(WorkspaceSymbolHit{
                .name = symbol.name,
                .container = symbol.container_name,
                .path = std::move(*path),
                .line = static_cast<std::size_t>(std::max(symbol.location.range.start.line, 0)),
                .character = symbol.location.range.start.character,
            });
          }
        }
        deliver(std::move(hits));
      });
  return true;
}

void NavigationAssist::OpenWorkspaceSymbol(const std::filesystem::path& path, std::size_t line, int character) {
  LspClient::Location location;
  location.uri = FileUriForPath(path);
  location.range.start.line = static_cast<int>(line);
  location.range.start.character = character;
  location.range.end = location.range.start;
  NavigateToLspLocation(location, workspace_symbol_encoding_);
}

void NavigationAssist::NavigateToPluginLocation(const plugin::PluginHost::LocationResult& location) {
  if (location.path.empty()) {
    return;
  }
  OpenThen(location.path, [this, location]() {
    if (editor::TextViewport* active = host_.ops().active_editor_viewport(); active != nullptr) {
      active->JumpCursorTo(location.line > 0 ? location.line - 1 : 0,
                           location.column > 0 ? location.column - 1 : 0);
      host_.ops().reset_caret_blink();
      host_.ops().request_frame();
    }
  });
}

void NavigationAssist::EmitReferenceEntry(
    const char* channel_id, const char* channel_title, const std::filesystem::path& path,
    std::size_t line, ReferenceColumn column, bool append_separator,
    std::map<std::filesystem::path, std::vector<std::string>>& file_line_cache) const {
  const std::size_t target_line = line > 0 ? line : 1;
  const std::size_t first_line = target_line > 1 ? target_line - 1 : 1;
  const std::size_t last_line = target_line + 1;

  struct SnippetRow {
    std::size_t number = 0;
    std::string_view text;
  };
  SnippetRow rows[3];
  std::size_t row_count = 0;
  const auto push_row = [&](std::size_t number, std::string_view text) {
    if (row_count < std::size(rows)) {
      rows[row_count++] = SnippetRow{number, text};
    }
  };

  std::vector<std::string> window;
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (const editor::TextViewport* viewport = host_.ops().active_editable_viewport();
      viewport != nullptr && util::SameAsNormalizedPath(viewport->path(), normalized_path)) {
    const editor::LineSpan lines = viewport->lines();
    for (std::size_t line_number = first_line;
         line_number <= last_line && line_number <= lines.size(); ++line_number) {
      push_row(line_number, lines[line_number - 1]);
    }
  } else {
    const std::vector<std::string>* file_lines = nullptr;
    const auto cache_it = file_line_cache.find(path);
    if (cache_it != file_line_cache.end()) {
      file_lines = &cache_it->second;
    } else {
      const util::FileSignature signature = util::StatFileSignature(path);
      if (signature.exists && !signature.error &&
          signature.size <= kReferenceSnippetWholeReadBytes) {
        if (const auto text = util::ReadTextFile(path); text.has_value()) {
          file_lines = &file_line_cache.emplace(path, util::SplitLines(*text)).first->second;
        }
      } else {
        window = util::ReadFileLineWindow(path, first_line, last_line,
                                          kReferenceSnippetScanBytes);
      }
    }
    if (file_lines != nullptr) {
      for (std::size_t line_number = first_line;
           line_number <= last_line && line_number <= file_lines->size(); ++line_number) {
        push_row(line_number, (*file_lines)[line_number - 1]);
      }
    } else {
      for (std::size_t i = 0; i < window.size(); ++i) {
        push_row(first_line + i, window[i]);
      }
    }
  }

  const bool lsp_units = column.units == ReferenceColumn::Units::LspCodeUnit0Based;
  std::size_t display_column = lsp_units ? column.value + 1 : column.value;
  for (std::size_t i = 0; i < row_count; ++i) {
    if (rows[i].number != target_line) {
      continue;
    }
    const std::size_t byte_column =
        lsp_units ? lsp_encoding::LspCharacterToByteColumn(rows[i].text, column.value,
                                                           column.encoding)
                  : std::min(column.value > 0 ? column.value - 1 : 0, rows[i].text.size());
    display_column = util::Utf8CodepointCount(rows[i].text.substr(0, byte_column)) + 1;
    break;
  }

  const std::string label =
      host_.context().current_project_state.root.empty()
          ? path.generic_string()
          : RelativePathLabel(host_.context().current_project_state.root, path);
  host_.output().AppendLine(
      channel_id, channel_title,
      label + ":" + std::to_string(line) + ":" + std::to_string(display_column));
  if (row_count == 0) {
    return;
  }
  for (std::size_t i = 0; i < row_count; ++i) {
    host_.output().AppendLine(
        channel_id, channel_title,
        std::string(rows[i].number == target_line ? " > " : "   ") +
            std::to_string(rows[i].number) + " | " + std::string(rows[i].text));
  }
  if (append_separator) {
    host_.output().AppendLine(channel_id, channel_title, "");
  }
}

}

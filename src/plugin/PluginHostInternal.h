#pragma once

#include "plugin/PluginHost.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "platform/AppDirectories.h"
#include "platform/Filesystem.h"
#include "platform/Subprocess.h"
#include "plugin/PluginContributionInterop.h"
#include "plugin/PluginContributionLimits.h"
#include "plugin/PluginDataDirectoryInterop.h"
#include "plugin/PluginBufferLifecycleInterop.h"
#include "plugin/PluginHoverQueryInterop.h"
#include "plugin/PluginLuaBufferProjectInterop.h"
#include "plugin/PluginLuaInterop.h"
#include "plugin/PluginLuaContextInterop.h"
#include "plugin/PluginDiscoveryInterop.h"
#include "plugin/PluginLifecycleCallbackInterop.h"
#include "plugin/PluginLifecycleLoadInterop.h"
#include "plugin/PluginLifecycleResetInterop.h"
#include "plugin/PluginLanguageProviderQueryInterop.h"
#include "plugin/PluginPathInterop.h"
#include "plugin/PluginPresentationRegistrationParsers.h"
#include "plugin/PluginProcessInterop.h"
#include "plugin/PluginProviderQueryInterop.h"
#include "plugin/PluginRegistrationParsers.h"
#include "plugin/PluginRegistryInterop.h"
#include "plugin/PluginProjectLifecycleInterop.h"
#include "plugin/PluginRuntimeApiInterop.h"
#include "plugin/PluginSidebarHoverInterop.h"
#include "plugin/PluginStateTeardownInterop.h"
#include "plugin/PluginWorkspaceInterop.h"
#include "plugin/PluginHostRuntimeTypes.h"
#include "plugin/PluginThread.h"
#include "plugin/PluginThreadTypes.h"
#include "plugin/LuaError.h"
#include "plugin/LuaRuntime.h"
#include "util/TextFileIO.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin {

// Upper bound on how long a buffer save will park the UI thread waiting for the
// worker to run save participants. Participants normally finish in microseconds;
// this caps the freeze if the worker is mid-PCall on a prior job or a participant
// blocks on slow I/O (e.g. ctx.process.run, which the instruction watchdog cannot
// bound). On timeout the save proceeds with untransformed text and warns -- a save
// is never wedged by a slow plugin.
constexpr std::chrono::milliseconds kSaveParticipantDeadline{2000};

// Per-plugin-call execution context. This is THREAD-LOCAL, not shared host state:
// re-entrancy ("am I already inside a plugin call on THIS thread") and the active
// snapshot / direct-access flags are inherently per-thread. A shared member would
// race — the worker writes the flags while the main thread reads them to decide
// inline-vs-post, and a stale read of "executing" set by the busy worker would make
// the main thread wrongly run Lua inline. Keeping it thread-local makes the worker's
// context invisible to the main thread and vice versa.
struct PluginExecContext {
  const PluginHostSnapshot* snapshot = nullptr;
  bool direct = true;
  bool executing = false;
  // Whether the running call may register setup-time contributions. Decoupled from
  // `direct` so a DETACHED reload (direct=false, because the UI thread is NOT parked
  // and shell writes must defer to the mailbox) can still register commands, sidebars,
  // status items, etc. on the worker. Reactive events and async queries leave this
  // false: a keystroke handler must never mutate the contribution registries the UI
  // is reading. For exclusive contexts (inline / round-trip) direct and
  // allow_registration move together.
  bool allow_registration = true;
  // True only while a reload's load body runs (setup + project-open). A ctx.status.update
  // issued here must mutate the LIVE status order so the reload's publish carries it;
  // outside a reload it targets the published (visible) view instead. Set by
  // RunReloadLoad and restored by the enclosing ExecuteWithContext's ContextGuard.
  bool in_reload = false;
};

// External-linkage so every PluginHost TU shares one per-thread context.
extern thread_local PluginExecContext g_exec;

struct PluginHost::Impl {
  using PluginInstance = runtime_types::PluginInstance;
  using PluginCommand = runtime_types::PluginCommand;
  using SidebarProvider = runtime_types::SidebarProvider;
  using HoverProvider = runtime_types::HoverProvider;
  using SaveParticipantRuntime = runtime_types::SaveParticipantRuntime;
  using CompletionRuntime = runtime_types::CompletionRuntime;
  using CodeActionRuntime = runtime_types::CodeActionRuntime;
  using LanguageQueryRuntime = runtime_types::LanguageQueryRuntime;
  using TestProviderRuntime = runtime_types::TestProviderRuntime;
  using ScmProviderRuntime = runtime_types::ScmProviderRuntime;
  using AnnotationProviderRuntime = runtime_types::AnnotationProviderRuntime;
  using AuthProviderRuntime = runtime_types::AuthProviderRuntime;

  // `callbacks` is the THREAD-ROUTED view the Lua verbs see: read verbs resolve
  // from the active per-call snapshot, write verbs either run directly (round-trip
  // ops, where the UI thread is parked and the worker has exclusive shell access)
  // or post a main-thread action to the worker mailbox (fire-and-forget events).
  // `raw_callbacks` is the unwrapped shell binding the routed view dispatches to.
  Callbacks raw_callbacks{};
  Callbacks callbacks{};
  std::filesystem::path current_project_root;

  // The dedicated worker that runs plugin Lua. Set once at wiring time and read
  // from both threads as a stable pointer; the per-call execution context lives in
  // the thread-local g_exec, not here.
  PluginThread* worker_ = nullptr;

  // How long a save will park the UI waiting for the worker to run participants.
  // Defaults to kSaveParticipantDeadline; tests shorten it to exercise the timeout
  // path without a multi-second wait.
  std::chrono::milliseconds save_participant_deadline_ = kSaveParticipantDeadline;

  // Restores the per-thread execution context on scope exit so nested plugin calls
  // and any early return leave the thread-local flags as they were.
  struct ContextGuard {
    PluginExecContext prev;
    ~ContextGuard() { g_exec = prev; }
  };

  template <typename F>
  void ExecuteWithContext(const PluginHostSnapshot* snapshot, bool direct,
                          bool allow_registration, F&& fn) {
    ContextGuard guard{g_exec};
    g_exec.snapshot = snapshot;
    g_exec.direct = direct;
    g_exec.executing = true;
    g_exec.allow_registration = allow_registration;
    fn();
  }

  // Apply a host-side mutation: run it now when the worker holds exclusive shell
  // access (round-trip with the UI thread parked, or the un-wired inline path),
  // otherwise marshal it to the UI thread via the mailbox.
  void ApplyHostMutation(std::function<void()> fn) {
    if (g_exec.direct || worker_ == nullptr) {
      fn();
    } else {
      worker_->PostToMain(std::move(fn));
    }
  }

  std::optional<PluginHost::ActiveBuffer> ResolveActiveBuffer() const {
    if (g_exec.snapshot != nullptr) {
      if (!g_exec.snapshot->active_buffer.present) {
        return std::nullopt;
      }
      return PluginHost::ActiveBuffer{
          .path = g_exec.snapshot->active_buffer.path,
          .line = g_exec.snapshot->active_buffer.line,
          .column = g_exec.snapshot->active_buffer.column,
      };
    }
    return raw_callbacks.active_buffer ? raw_callbacks.active_buffer() : std::nullopt;
  }

  std::optional<std::string> ResolveSetting(std::string_view id) const {
    if (g_exec.snapshot != nullptr) {
      if (g_exec.snapshot->settings != nullptr) {
        for (const auto& [key, value] : *g_exec.snapshot->settings) {
          if (key == id) {
            return value;
          }
        }
      }
      return std::nullopt;
    }
    return raw_callbacks.get_setting ? raw_callbacks.get_setting(id) : std::nullopt;
  }

  // Build the thread-routed Callbacks view over `raw_callbacks`. Read verbs resolve
  // from the snapshot; write/request verbs go through ApplyHostMutation. Closures
  // capture only owning copies (never a lua_State / Lua ref) so a deferred action
  // is safe to run later on the UI thread.
  Callbacks BuildRoutedCallbacks();

  // Capture the immutable host view a plugin call may read. Runs on the UI thread.
  PluginHostSnapshot CaptureSnapshot() const;

  // Build or reuse the shared immutable resolved-settings block for a snapshot.
  // Rebuilt only when the host settings revision or contributed specs change;
  // otherwise returns the cached shared block. UI-thread only.
  std::shared_ptr<const ResolvedPluginSettings> ResolveSettingsSnapshot() const;

  // Dispatch a plugin call onto the worker. Detached = fire-and-forget (events);
  // Blocking = bounded synchronous round-trip preserving the synchronous API. A
  // non-empty `coalesce_key` posts latest-only: a newer job with the same key drops the
  // superseded queued one (used for cursor/selection events, TD-2026-07-17A-078). An
  // empty key posts FIFO (ordered delivery for buffer_change/open/save/close).
  void RunOnWorkerDetached(PluginHostSnapshot snapshot, std::function<void()> fn,
                           std::string coalesce_key = {});

  // Outcome of a deadline-bounded save-participant round-trip. `text` holds the
  // (possibly transformed) buffer when the worker finished in time; on timeout
  // the worker may still be writing into the shared copy, so `timed_out` callers
  // must ignore `text` and keep their original buffer.
  struct SaveParticipantResult {
    std::string text;
    std::string error;
    bool ok = true;
    bool timed_out = false;
  };

  // Run the registered save participants on the worker, waiting at most
  // `kSaveParticipantDeadline`. Participant Lua only transforms `input` via its
  // return value; any host side effects defer to the mailbox (direct=false) so a
  // job that outlives the UI's wait can never mutate live shell state after the
  // UI has moved on. The result text lives in a shared buffer co-owned by the
  // worker job, so a late completion past a timeout is safe to abandon.
  SaveParticipantResult RunSaveParticipantsBounded(const std::filesystem::path& path,
                                                   std::string input);
  // `allow_registration` defaults to false: a synchronous query/provider/command
  // round-trip must NOT let its Lua PCall register contributions, because the query
  // interop iterates a live runtime vector (completion_runtimes, code_action_runtimes,
  // language_query_runtimes, annotation/scm/test/auth runtimes, …) by reference across
  // that PCall — a mid-iteration push_back would reallocate the vector and dangle the
  // loop's `provider` reference (a use-after-free). Only the reload/load body, which is
  // genuine setup, passes true. This mirrors RunQueryAsync/RunOnWorkerDetached, which
  // already run with allow_registration=false.
  template <typename F>
  void RunOnWorkerBlocking(const PluginHostSnapshot& snapshot, F&& fn,
                           const bool allow_registration = false) {
    if (g_exec.executing || worker_ == nullptr) {
      // Already on the worker (re-entrant) or no worker wired: run inline with
      // exclusive access rather than dead-locking on a self-post.
      ExecuteWithContext(&snapshot, /*direct=*/true, allow_registration,
                         std::forward<F>(fn));
      return;
    }
    worker_->EnsureStarted();
    std::promise<void> done;
    std::future<void> finished = done.get_future();
    // Plain Post (FIFO), NOT PostFront: a synchronous call must preserve ordering
    // with already-queued reactive events so it observes their effects, not jump
    // ahead of them. The hot-path queries (hover/completion/symbols) run through the
    // *Async variants instead; these blocking entry points are discrete user actions
    // (command palette, sidebar item, run tests, SCM/auth), and any speculative query
    // backlog is dedup-capped by PostLatest. The wait is unbounded but bounded in
    // practice by the per-call runtime watchdog (LuaRuntime call budget). Captures by
    // reference are safe: the UI thread blocks on `finished` until the job completes,
    // so every referent outlives the call (which is also why a deadline here would be
    // unsafe -- a late job would touch freed stack locals).
    worker_->Post([this, &snapshot, &fn, &done, allow_registration]() {
      // Release the UI-thread waiter on EVERY exit path, including a throw from
      // `fn`. The SerialWorkQueue firewall swallows worker exceptions, so without
      // this guard a throwing job would leave `done` unsatisfied and the
      // finished.wait() below would deadlock the UI forever.
      struct ReleaseWaiter {
        std::promise<void>& promise;
        ~ReleaseWaiter() { promise.set_value(); }
      } release_waiter{done};
      ExecuteWithContext(&snapshot, /*direct=*/true, allow_registration, fn);
    });
    finished.wait();
  }

  // Dispatch a result-returning call onto the worker without blocking the UI.
  // `produce` runs on the worker (reads resolve from the captured snapshot) and
  // returns the POD result; `deliver` runs on the UI thread during the mailbox
  // drain. A non-empty `dedup_key` collapses superseded in-flight requests of the
  // same kind (a stale cursor-driven query is dropped before it runs); an empty
  // key means every call must run (e.g. command execution), so it is plain-posted.
  // With no worker wired (tests / pre-wire) or when already on the worker, runs
  // inline and delivers synchronously.
  template <typename Result>
  void RunQueryAsync(std::string dedup_key,
                     std::function<Result()> produce,
                     std::function<void(Result)> deliver) {
    PluginHostSnapshot snapshot = CaptureSnapshot();
    if (worker_ == nullptr || g_exec.executing) {
      // A query never registers contributions: match the worker path's
      // allow_registration=false so a re-entrant query taking this inline branch
      // cannot mutate the contribution registries the UI is reading.
      Result result;
      ExecuteWithContext(&snapshot, /*direct=*/true, /*allow_registration=*/false,
                         [&]() { result = produce(); });
      deliver(std::move(result));
      return;
    }
    worker_->EnsureStarted();
    auto task = [this, snapshot = std::move(snapshot), produce = std::move(produce),
                 deliver = std::move(deliver)]() mutable {
      auto result = std::make_shared<Result>();
      ExecuteWithContext(&snapshot, /*direct=*/false, /*allow_registration=*/false,
                         [&]() { *result = produce(); });
      worker_->PostToMain([result, deliver = std::move(deliver)]() mutable {
        deliver(std::move(*result));
      });
    };
    if (dedup_key.empty()) {
      worker_->Post(std::move(task));
    } else {
      worker_->PostLatest(std::move(dedup_key), std::move(task));
    }
  }

#if MICROIDE_HAS_LUA_PLUGINS
  // The Lua-touching core of a reload: tear down the old states, load every plugin
  // root, and dispatch project-open. Runs under an active execution context (inline
  // with exclusive access, or detached on the worker with registration allowed and
  // shell writes deferred). Shared by the synchronous Reload and the detached
  // ReloadAsync so the load sequence lives in exactly one place.
  void RunReloadLoad(const std::filesystem::path& next_project_root,
                     const std::vector<std::filesystem::path>& plugin_roots) {
    // Mark the reload window; the enclosing ExecuteWithContext's ContextGuard restores
    // this to false when the load body returns.
    g_exec.in_reload = true;
    TearDownPlugins();
    disabled_plugin_meta.clear();
    current_project_root = next_project_root;
    for (const auto& plugin_root : plugin_roots) {
      std::string error_message;
      if (!LoadPluginRoot(plugin_root, &error_message) && !error_message.empty()) {
        RecordError(std::move(error_message));
      }
    }
    if (!current_project_root.empty()) {
      project_lifecycle_interop::DispatchProjectOpenCallbacks(
          &plugins, [this](runtime_types::PluginInstance* plugin, int callback_ref,
                           const char* callback_name) {
            CallProjectCallback(plugin, callback_ref, callback_name);
          });
    }
  }

  // Run a reload on the worker WITHOUT parking the UI thread, then publish the rebuilt
  // contribution snapshot and invoke `on_complete` on the UI thread during the mailbox
  // drain. Registration is allowed (it's setup) but shell writes defer, and the
  // reload's RecordError appends marshal to the UI in mailbox order ahead of the
  // completion -- so the error count is read here, on the UI thread, where it is final.
  void ReloadDetached(PluginHostSnapshot snapshot,
                      std::vector<std::filesystem::path> plugin_roots,
                      std::function<void(bool)> on_complete) {
    worker_->EnsureStarted();
    worker_->Post([this, snapshot = std::move(snapshot),
                   plugin_roots = std::move(plugin_roots),
                   on_complete = std::move(on_complete)]() mutable {
      ExecuteWithContext(&snapshot, /*direct=*/false, /*allow_registration=*/true,
                         [&]() { RunReloadLoad(snapshot.project_root, plugin_roots); });
      // Build the snapshot here (live registries are worker-owned); patch the
      // error-count-bearing summary on the UI thread where errors are settled.
      auto built = std::make_shared<ContributionSnapshot>(BuildContributionSnapshot());
      const std::size_t plugin_count = plugins.size();
      const std::size_t command_count = commands.size();
      const std::size_t sidebar_count = sidebars.size();
      const std::size_t hover_count = hover_provider_order.size();
      worker_->PostToMain([this, built, plugin_count, command_count, sidebar_count, hover_count,
                           on_complete = std::move(on_complete)]() mutable {
        published_ = std::move(*built);
        published_.reload_summary = FormatReloadSummary(plugin_count, command_count, sidebar_count,
                                                        hover_count, errors.size());
        ++status_view_revision;
        on_complete(errors.empty());
      });
    });
  }
#endif

  // Immutable, UI-thread-owned view of every setup-time contribution the UI reads
  // (everything that changes only on Reload; the runtime-mutating status items,
  // messages, and errors are not here). The worker builds it from the live
  // registries at the end of a reload; the UI accessors read it. This decouples UI
  // reads from the worker's registry mutations so Reload can stop blocking the UI
  // (the worker rebuilds the live registries and republishes this view without the
  // UI ever touching live mutable plugin state).
  struct ContributionSnapshot {
    std::vector<std::string> command_names;
    std::vector<SidebarProviderInfo> sidebar_providers;
    std::vector<PluginHost::ContributedMenuEntry> menu_entries;
    std::vector<PluginHost::ContributedKeybinding> keybindings;
    std::vector<PluginHost::ContributedSettingSpec> settings;
    std::vector<PluginHost::ContributedFormatter> formatters;
    std::vector<PluginHost::ContributedSaveParticipant> save_participants;
    std::vector<PluginHost::ContributedCompletion> completions;
    std::vector<PluginHost::ContributedCodeAction> code_actions;
    std::vector<PluginHost::ContributedLanguageServer> language_servers;
    std::vector<PluginHost::ContributedDebugAdapter> debug_adapters;
    std::vector<PluginHost::ContributedLaunchConfig> launch_configs;
    std::vector<PluginHost::ContributedTask> tasks;
    std::vector<PluginHost::ContributedTool> tools;
    std::vector<PluginHost::ContributedTestProvider> test_providers;
    std::vector<PluginHost::ContributedScmProvider> scm_providers;
    std::vector<PluginHost::ContributedAnnotationProvider> annotation_providers;
    std::vector<PluginHost::ContributedAuthProvider> auth_providers;
    std::vector<PluginHost::ContributedBracketSet> bracket_sets;
    std::vector<PluginHost::ContributedCommentMarkers> comment_markers;
    std::vector<PluginHost::ContributedIndentRules> indent_rules;
    std::vector<PluginHost::ContributedSnippet> snippets;
    std::vector<PluginHost::ContributedTheme> themes;
    std::vector<PluginHost::ContributedFileIconTheme> file_icon_themes;
    std::vector<PluginHost::LoadedPlugin> loaded_plugins;
    // Sorted status-item view the status bar renders. Setup registers into the live
    // status_item_order on the worker; publish copies it here. Runtime ctx.status.update
    // mutates THIS copy directly on the UI thread (see LuaStatusUpdate) so a status
    // change shows without a reload.
    std::vector<PluginHost::ContributedStatusItem> status_item_order;
    // id->position cache over status_item_order, maintained by ApplyStatusItemUpdate
    // so runtime ctx.status.update resolves the target in O(1). Reset to empty on each
    // publish (this snapshot is rebuilt wholesale) and lazily repopulated on first use.
    std::unordered_map<std::string, std::size_t> status_item_index;
    // Which reactive editor events any loaded plugin subscribes to. The shell gates
    // its per-keystroke sampling on this, so it must read the published (race-free)
    // value rather than scanning the live `plugins` vector the worker rebuilds.
    PluginHost::EditorEventInterest editor_event_interests;
    // The project root the UI resolves plugin paths against. The live
    // current_project_root is worker-owned and rewritten mid-reload, so UI-thread path
    // resolution (snapshot capture, hover) reads this published copy instead.
    std::filesystem::path project_root;
    std::string reload_summary = "Lua plugin runtime unavailable";
  };
  ContributionSnapshot published_;
  // Monotonic stamp the UI render/hit-test paths compare to decide when to re-resolve
  // status geometry. UI-thread-owned: bumped when a snapshot is published and when a
  // runtime status update is applied to published_.status_item_order.
  std::uint64_t status_view_revision = 0;
  // Cached shared resolved-settings block for snapshot capture, plus the revisions
  // it was built at. Reused across snapshots until the host settings revision or the
  // contributed specs (status_view_revision) change. UI-thread-owned (only
  // ResolveSettingsSnapshot reads/writes it); the block itself is immutable so
  // worker snapshots may hold shared references safely. See TD-2026-07-17A-076.
  mutable std::shared_ptr<const ResolvedPluginSettings> cached_settings_;
  mutable std::uint64_t cached_settings_revision_ = 0;
  mutable std::uint64_t cached_settings_specs_revision_ = 0;

  std::vector<PluginInstance> plugins;
  // Plugin ids the user has disabled: their setup is skipped on Reload. disabled_plugin_meta
  // records {id, root} for the ones actually skipped this reload so the UI can list them.
  std::vector<std::string> disabled_plugins;
  std::vector<PluginHost::LoadedPlugin> disabled_plugin_meta;
  std::unordered_map<std::string, PluginCommand> commands;
  std::vector<std::string> command_names;
  std::unordered_map<std::string, SidebarProvider> sidebars;
  std::vector<SidebarProviderInfo> sidebar_providers;
  // Ordered views are sorted projections of the maps above. Registration only
  // flips these dirty bits (O(1)); the const accessors rebuild lazily on the next
  // read, so a plugin registering N commands sorts once instead of N times.
  bool command_names_dirty = false;
  bool sidebar_providers_dirty = false;
  std::unordered_map<std::string, HoverProvider> hovers;
  std::vector<std::string> hover_provider_order;
  std::vector<PluginHost::ContributedMenuEntry> menu_entries;
  std::vector<PluginHost::ContributedKeybinding> keybindings;
  std::vector<PluginHost::ContributedSettingSpec> settings;
  // Worker-owned status registry: setup registers into these; publish copies
  // status_item_order into published_.status_item_order for the UI to render.
  std::unordered_map<std::string, PluginHost::ContributedStatusItem> status_items;
  std::vector<PluginHost::ContributedStatusItem> status_item_order;
  // id->position cache over the worker status_item_order (see the published mirror in
  // ContributionSnapshot); used only on the in-reload ctx.status.update path.
  std::unordered_map<std::string, std::size_t> status_item_index;
  std::vector<PluginHost::ContributedFormatter> formatters;
  std::vector<PluginHost::ContributedSaveParticipant> save_participants;
  std::vector<SaveParticipantRuntime> save_participant_runtimes;
  std::vector<PluginHost::ContributedCompletion> completions;
  std::vector<CompletionRuntime> completion_runtimes;
  std::vector<PluginHost::ContributedCodeAction> code_actions;
  std::vector<CodeActionRuntime> code_action_runtimes;
  std::vector<LanguageQueryRuntime> language_query_runtimes;
  std::vector<PluginHost::ContributedLanguageServer> language_servers;
  std::vector<PluginHost::ContributedDebugAdapter> debug_adapters;
  std::vector<PluginHost::ContributedLaunchConfig> launch_configs;
  std::vector<PluginHost::ContributedTask> tasks;
  std::vector<PluginHost::ContributedTool> tools;
  std::vector<PluginHost::ContributedTestProvider> test_providers;
  std::vector<TestProviderRuntime> test_provider_runtimes;
  std::vector<PluginHost::ContributedScmProvider> scm_providers;
  std::vector<ScmProviderRuntime> scm_provider_runtimes;
  std::vector<PluginHost::ContributedAnnotationProvider> annotation_providers;
  std::vector<AnnotationProviderRuntime> annotation_provider_runtimes;
  std::vector<PluginHost::ContributedAuthProvider> auth_providers;
  std::vector<AuthProviderRuntime> auth_provider_runtimes;
  std::vector<PluginHost::ContributedBracketSet> bracket_sets;
  std::vector<PluginHost::ContributedCommentMarkers> comment_markers;
  std::vector<PluginHost::ContributedIndentRules> indent_rules;
  std::vector<PluginHost::ContributedSnippet> snippets;
  std::vector<PluginHost::ContributedTheme> themes;
  std::vector<PluginHost::ContributedFileIconTheme> file_icon_themes;
  std::vector<std::string> messages;
  std::vector<std::string> errors;
  std::string reload_summary = "Lua plugin runtime unavailable";
  bool startup_plugins_enabled = true;
#if MICROIDE_HAS_LUA_PLUGINS
  PluginInstance* active_plugin = nullptr;
#endif

  [[nodiscard]] bool enabled() const {
#if MICROIDE_HAS_LUA_PLUGINS
    return startup_plugins_enabled;
#else
    return false;
#endif
  }

  // Pure formatter so the summary can be built from worker-known counts plus the
  // UI-side error count (the detached-reload completion supplies `error_count` after
  // the marshalled RecordError appends have settled on the UI thread).
  static std::string FormatReloadSummary(std::size_t plugin_count, std::size_t command_count,
                                         std::size_t sidebar_count, std::size_t hover_count,
                                         std::size_t error_count) {
    std::string summary = "Loaded " + std::to_string(plugin_count) + " plugin";
    if (plugin_count != 1) {
      summary += "s";
    }
    summary += " and " + std::to_string(command_count) + " command";
    if (command_count != 1) {
      summary += "s";
    }
    summary += " and " + std::to_string(sidebar_count) + " sidebar";
    if (sidebar_count != 1) {
      summary += "s";
    }
    summary += " and " + std::to_string(hover_count) + " hover provider";
    if (hover_count != 1) {
      summary += "s";
    }
    if (error_count != 0) {
      summary += " with " + std::to_string(error_count) + " error";
      if (error_count != 1) {
        summary += "s";
      }
    }
    return summary;
  }

  void SetReloadSummary() {
    if (!enabled()) {
      reload_summary = "Lua plugin runtime unavailable";
      return;
    }
    // Read the maps, not the ordered views: the latter rebuild lazily and may be
    // dirty here, but their element counts always match the source maps.
    reload_summary = FormatReloadSummary(plugins.size(), commands.size(), sidebars.size(),
                                         hover_provider_order.size(), errors.size());
  }

  // The reactive editor events any loaded plugin subscribes to. Computed from the live
  // `plugins` vector (worker-owned), captured into the published snapshot so the UI
  // reads a race-free value.
  PluginHost::EditorEventInterest ComputeEditorEventInterests() const {
    PluginHost::EditorEventInterest interest;
#if MICROIDE_HAS_LUA_PLUGINS
    for (const auto& plugin : plugins) {
      interest.buffer_change |= plugin.on_buffer_change_ref != LUA_NOREF;
      interest.cursor_move |= plugin.on_cursor_move_ref != LUA_NOREF;
      interest.selection_change |= plugin.on_selection_change_ref != LUA_NOREF;
      interest.buffer_close |= plugin.on_buffer_close_ref != LUA_NOREF;
      interest.buffer_open |= plugin.on_buffer_open_ref != LUA_NOREF;
      interest.buffer_save |= plugin.on_buffer_save_ref != LUA_NOREF;
    }
#endif
    return interest;
  }

  // Compute the {id, root, enabled} list the UI shows for loaded + disabled plugins.
  std::vector<PluginHost::LoadedPlugin> ComputeLoadedPlugins() const {
    std::vector<PluginHost::LoadedPlugin> result;
    result.reserve(plugins.size() + disabled_plugin_meta.size());
    for (const auto& plugin : plugins) {
      result.push_back(PluginHost::LoadedPlugin{.id = plugin.id, .root = plugin.root, .enabled = true});
    }
    for (const auto& meta : disabled_plugin_meta) {
      result.push_back(meta);
    }
    std::sort(result.begin(), result.end(),
              [](const PluginHost::LoadedPlugin& a, const PluginHost::LoadedPlugin& b) {
                return a.id < b.id;
              });
    return result;
  }

  // Build the immutable UI view from the live setup-time registries, resolving the
  // lazy ordered views first. Called when a reload/shutdown settles; the result is
  // assigned to `published_` (in 4a inline while the UI is parked on the round-trip;
  // a later phase posts it to the UI thread so Reload need not block).
  ContributionSnapshot BuildContributionSnapshot() {
#if MICROIDE_HAS_LUA_PLUGINS
    if (command_names_dirty) {
      registry_interop::RebuildCommandNames(commands, &command_names);
      command_names_dirty = false;
    }
    if (sidebar_providers_dirty) {
      registry_interop::RebuildSidebarProviders(sidebars, &sidebar_providers);
      sidebar_providers_dirty = false;
    }
#endif
    ContributionSnapshot snapshot;
    snapshot.command_names = command_names;
    snapshot.sidebar_providers = sidebar_providers;
    snapshot.menu_entries = menu_entries;
    snapshot.keybindings = keybindings;
    snapshot.settings = settings;
    snapshot.formatters = formatters;
    snapshot.save_participants = save_participants;
    snapshot.completions = completions;
    snapshot.code_actions = code_actions;
    snapshot.language_servers = language_servers;
    snapshot.debug_adapters = debug_adapters;
    snapshot.launch_configs = launch_configs;
    snapshot.tasks = tasks;
    snapshot.tools = tools;
    snapshot.test_providers = test_providers;
    snapshot.scm_providers = scm_providers;
    snapshot.annotation_providers = annotation_providers;
    snapshot.auth_providers = auth_providers;
    snapshot.bracket_sets = bracket_sets;
    snapshot.comment_markers = comment_markers;
    snapshot.indent_rules = indent_rules;
    snapshot.snippets = snippets;
    snapshot.themes = themes;
    snapshot.file_icon_themes = file_icon_themes;
    snapshot.loaded_plugins = ComputeLoadedPlugins();
    snapshot.status_item_order = status_item_order;
    snapshot.editor_event_interests = ComputeEditorEventInterests();
    snapshot.project_root = current_project_root;
    snapshot.reload_summary = reload_summary;
    return snapshot;
  }

  void PublishContributionSnapshot() {
    published_ = BuildContributionSnapshot();
    ++status_view_revision;
  }

  // RecordMessage/RecordError mutate the host-owned messages/errors vectors that the
  // UI thread reads, so the whole record-and-sink runs through ApplyHostMutation:
  // inline under exclusive access (setup/round-trip), or marshalled to the UI thread
  // when a fire-and-forget event records on the worker.
  // Bound the host-owned messages/errors history so a long-running or flooding plugin
  // (repeated ctx.log / callback errors) cannot grow host memory without limit, even
  // though the visible output panel is already capped by WorkspaceOutputChannels
  // (TD-2026-07-17A-020). The real panel gets the full line via the sink first; only the
  // retained debug/test copy is trimmed. Batch-drop the oldest quarter once the ceiling
  // is crossed so the trim is amortized O(1) per append rather than an O(n) front-erase
  // every time.
  static void TrimRecordedHistory(std::vector<std::string>& entries) {
    if (entries.size() <= PluginHost::kMaxRecordedLogEntries) {
      return;
    }
    const std::size_t keep = PluginHost::kMaxRecordedLogEntries * 3 / 4;
    entries.erase(entries.begin(), entries.begin() + (entries.size() - keep));
  }

  void RecordMessage(std::string message) {
    ApplyHostMutation([this, message = std::move(message)]() mutable {
      messages.push_back(std::move(message));
      if (raw_callbacks.log_sink) {
        raw_callbacks.log_sink(messages.back());
      }
      TrimRecordedHistory(messages);
    });
  }

  void RecordError(std::string error) {
    ApplyHostMutation([this, error = std::move(error)]() mutable {
      errors.push_back(std::move(error));
      if (raw_callbacks.error_sink) {
        raw_callbacks.error_sink(errors.back());
      }
      TrimRecordedHistory(errors);
    });
  }

  std::optional<std::string> RelativePathString(const std::filesystem::path& path) const {
    if (current_project_root.empty() || path.empty()) {
      return std::nullopt;
    }
    const std::filesystem::path relative =
        path.lexically_normal().lexically_relative(current_project_root.lexically_normal());
    if (relative.empty()) {
      return std::nullopt;
    }
    return relative.generic_string();
  }

#if MICROIDE_HAS_LUA_PLUGINS
  void PushBufferContext(lua_State* state,
                         const std::filesystem::path& path,
                         std::optional<std::string_view> text = std::nullopt) const {
    lua_createtable(state, 0, 3);
    const std::filesystem::path normalized_path = path.lexically_normal();
    const std::string path_string = normalized_path.generic_string();
    lua_pushlstring(state, path_string.c_str(), path_string.size());
    lua_setfield(state, -2, "path");

    const std::string relative_path = RelativePathString(normalized_path).value_or(
        normalized_path.filename().empty() ? normalized_path.generic_string()
                                           : normalized_path.filename().string());
    lua_pushlstring(state, relative_path.c_str(), relative_path.size());
    lua_setfield(state, -2, "relative_path");

    if (text.has_value()) {
      lua_pushlstring(state, text->data(), text->size());
      lua_setfield(state, -2, "text");
    }
  }

#endif

  std::vector<std::filesystem::path> DiscoverPluginRoots() const {
    return discovery_interop::DiscoverPluginRoots();
  }

#include "plugin/PluginHostLuaApi.inc"

  void ClearMessages() { messages.clear(); }

};

}  // namespace microide::plugin

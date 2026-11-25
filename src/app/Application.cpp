#include "app/Application.h"

#include "render/PaintElement.h"
#include "render/ScopedRenderClip.h"

#include <SDL3/SDL.h>

#include "workspace/WorkspaceStartupOptions.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fstream>
#include <iterator>

#include "app/DirtyRegionPolicy.h"
#include "app/ApplicationPresentationCache.h"
#include "app/EventDrainBudget.h"
#include "app/IdleWaitStrategy.h"
#include "app/SdlWaker.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "platform/CrashDiagnostics.h"
#include "platform/RuntimePaths.h"
#include "platform/SubprocessSandbox.h"
#include "render/RasterDecode.h"
#include "render/RendererInfo.h"
#include "render/SdlSurface.h"
#include "workspace/control/ControlSpec.h"
#include "util/CoverageProfile.h"
#include "util/StartupTrace.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/WindowPresentation.h"

namespace microide::app {

namespace {

workspace::WorkspaceShell::EventResult HandledWithFullRedraw() {
  return workspace::WorkspaceShell::EventResult{
      .handled = true,
      .redraw = workspace::WorkspaceShell::RenderInvalidation{.full = true, .rects = {}},
  };
}

constexpr int kInitialWindowWidth = 1440;
constexpr int kInitialWindowHeight = 900;
constexpr std::size_t kRenderPerfPartialClipWarnThreshold = 8;

bool EventUsesRenderCoordinates(Uint32 event_type) {
  switch (event_type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
      return true;
    default:
      return false;
  }
}

bool CustomWindowChromeEnabled(SDL_Window* window) {
  return window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS) != 0;
}

RenderPresentation RenderPresentationOf(const workspace::WorkspaceShell::WindowPresentationState& state) {
  return RenderPresentation{.logical_width = state.logical_width,
                            .logical_height = state.logical_height,
                            .scale_x = state.scale_x,
                            .scale_y = state.scale_y};
}

std::optional<workspace::WorkspaceShell::WindowPresentationState> CaptureWindowPresentationState(
    SDL_Window* window,
    SDL_Renderer* renderer,
    float ui_scale) {
  if (window == nullptr || renderer == nullptr) {
    return std::nullopt;
  }

  int pixel_width = 0;
  int pixel_height = 0;
  if (!SDL_GetRenderOutputSize(renderer, &pixel_width, &pixel_height)) {
    SDL_Log("SDL_GetRenderOutputSize failed: %s", SDL_GetError());
    return std::nullopt;
  }
  if (pixel_width <= 0 || pixel_height <= 0) {
    return std::nullopt;
  }

  const util::WindowPresentation presentation =
      util::ComputeWindowPresentation(pixel_width, pixel_height,
                                      SDL_GetWindowDisplayScale(window), ui_scale);
  const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
  int window_width = 0;
  int window_height = 0;
  SDL_GetWindowSize(window, &window_width, &window_height);
  const auto window_per_unit = [](float scale, int window_size, int pixel_size) {
    return window_size > 0 && pixel_size > 0
               ? scale * static_cast<float>(window_size) / static_cast<float>(pixel_size)
               : 1.0f;
  };
  return workspace::WorkspaceShell::WindowPresentationState{
      .logical_width = presentation.logical_width,
      .logical_height = presentation.logical_height,
      .scale_x = presentation.presentation_scale_x,
      .scale_y = presentation.presentation_scale_y,
      .window_per_unit =
          SDL_FPoint{window_per_unit(presentation.presentation_scale_x, window_width, pixel_width),
                     window_per_unit(presentation.presentation_scale_y, window_height, pixel_height)},
      .chrome =
          workspace::WorkspaceShell::WindowChromeState{
              .custom_enabled = CustomWindowChromeEnabled(window),
              .maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0,
              .fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0,
          },
  };
}

void SyncWindowState(SDL_Window* window) {
  if (window == nullptr) {
    return;
  }
  if (!SDL_SyncWindow(window)) {
    SDL_Log("SDL_SyncWindow failed: %s", SDL_GetError());
  }
}

void SetWindowIconFromAssets(SDL_Window* window) {
  if (window == nullptr) {
    return;
  }
  const std::filesystem::path icon_path =
      platform::ResolveBundledAssetPath("icons/microide.png");
  if (icon_path.empty()) {
    return;
  }

  std::size_t encoded_size = 0;
  void* encoded = SDL_LoadFile(icon_path.string().c_str(), &encoded_size);
  if (encoded == nullptr) {
    return;
  }
  std::vector<std::uint8_t> rgba;
  int width = 0;
  int height = 0;
  const bool decoded = render::DecodeRasterToRgba(
      std::span<const std::byte>(static_cast<const std::byte*>(encoded), encoded_size), rgba, width,
      height);
  SDL_free(encoded);
  if (!decoded) {
    return;
  }

  const render::SurfacePtr icon(
      SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, rgba.data(), width * 4));
  if (icon != nullptr) {
    SDL_SetWindowIcon(window, icon.get());
  }
}


workspace::ControlSpec ReadControlSpecFile(const std::filesystem::path& spec_path) {
  constexpr std::uintmax_t kMaxControlSpecBytes = 1u << 20;
  std::error_code status_ec;
  const std::filesystem::file_status status = std::filesystem::status(spec_path, status_ec);
  if (status_ec || !std::filesystem::is_regular_file(status)) {
    SDL_Log("control spec is not a regular file: %s", spec_path.string().c_str());
    return {};
  }
  const auto log_too_large = [&spec_path]() {
    SDL_Log("control spec too large (> %llu byte cap): %s",
            static_cast<unsigned long long>(kMaxControlSpecBytes), spec_path.string().c_str());
  };
  if (std::error_code size_ec;
      std::filesystem::file_size(spec_path, size_ec) > kMaxControlSpecBytes && !size_ec) {
    log_too_large();
    return {};
  }
  std::ifstream in(spec_path, std::ios::binary);
  if (!in) {
    SDL_Log("could not read control spec: %s", spec_path.string().c_str());
    return {};
  }
  std::string json(static_cast<std::size_t>(kMaxControlSpecBytes) + 1, '\0');
  in.read(json.data(), static_cast<std::streamsize>(json.size()));
  json.resize(static_cast<std::size_t>(in.gcount()));
  if (json.size() > kMaxControlSpecBytes) {
    log_too_large();
    return {};
  }
  workspace::ControlSpec spec = workspace::ParseControlSpec(json);
  if (!spec.valid) {
    SDL_Log("control spec parse error: %s", spec.parse_error.c_str());
  }
  return spec;
}

std::filesystem::path CurrentDirectoryOrEmpty() {
  std::error_code cwd_ec;
  std::filesystem::path cwd = std::filesystem::current_path(cwd_ec);
  if (cwd_ec) {
    SDL_Log("could not resolve current working directory (%s); starting with no project",
            cwd_ec.message().c_str());
    return {};
  }
  return cwd;
}


struct PendingRedraw {
  bool full = true;
  std::vector<SDL_FRect> rects;
  bool present = false;
  bool frame = false;
  const char* reason = "startup";

  bool DamageOwed() const { return full || !rects.empty(); }
  bool FrameOwed() const { return DamageOwed() || frame; }

  enum class Absorbed { Full, Partial, Covered };
  Absorbed Absorb(const workspace::RenderInvalidation& redraw) {
    present = present || redraw.present;
    frame = frame || redraw.frame;
    if (redraw.full) {
      full = true;
      rects.clear();
      return Absorbed::Full;
    }
    if (full) {
      return Absorbed::Covered;
    }
    rects.insert(rects.end(), redraw.rects.begin(), redraw.rects.end());
    return Absorbed::Partial;
  }
};

}

Application::Application(AppStartupOptions startup_options)
    : startup_options_(std::move(startup_options)) {}

Application::~Application() {
  Shutdown();
}

int Application::Run() {
  if (!Initialize()) {
    return 1;
  }

  running_ = true;
  (void)platform::StartStallWatchdogFromEnvironment();
  PendingRedraw pending;
  bool window_shown = false;
  std::optional<Uint64> owed_wake_deadline;
  const auto apply_scheduled_wake = [&] {
    owed_wake_deadline.reset();
    const auto scheduled = workspace_shell_.HandleScheduledWake();
    if (!scheduled.handled) {
      return;
    }
    const PendingRedraw::Absorbed absorbed = pending.Absorb(scheduled.redraw);
    if (absorbed == PendingRedraw::Absorbed::Partial) {
      pending.reason = "scheduled-partial";
    } else if (absorbed == PendingRedraw::Absorbed::Full) {
      pending.reason = "scheduled-full";
      workspace_shell_.InvalidateCursorKindFingerprint();
    }
  };

  while (running_) {
    platform::NoteMainLoopTurn();
    if (pending.DamageOwed()) {
      const bool was_first_frame = !first_render_complete_;
      Render(pending.full ? std::vector<SDL_FRect>{} : pending.rects, pending.reason);
      pending.present = false;
      pending.frame = false;

      if (!window_shown && first_render_complete_ && window_ != nullptr) {
        SDL_ShowWindow(window_);
        if (scene_texture_.valid()) {
          PresentRetainedScene();
        }
        window_shown = true;
      }

      if (was_first_frame) {
        while (workspace_shell_.ConsumePostRenderRedrawRequest().has_value()) {
        }
      } else if (auto settle = workspace_shell_.ConsumePostRenderRedrawRequest();
                 settle.has_value()) {
        pending.full = settle->full;
        pending.rects.assign(settle->rects.begin(), settle->rects.end());
        pending.reason = "render-settle";
        continue;
      }
      pending.full = false;
      pending.rects.clear();
      pending.reason = "event";

      workspace_shell_.RequestActiveHighlightPrefetch();
    } else if (pending.frame) {
      pending.frame = false;
      Render({}, "prepared", true);
    } else if (pending.present) {
      pending.present = false;
      if (scene_texture_.valid()) {
        PresentRetainedScene();
      } else {
        Render({}, "present-without-scene");
      }
    }

    if (owed_wake_deadline.has_value() && SDL_GetTicks() >= *owed_wake_deadline) {
      apply_scheduled_wake();
      if (pending.FrameOwed()) {
        continue;
      }
    }

    SDL_Event event;
    const workspace::WorkspaceShell::IdleWaitState idle_wait_state =
        workspace_shell_.CurrentIdleWaitState();
    const IdleWaitDecision idle_wait = ChooseIdleWait(idle_wait_state);
    const Uint64 wait_started_ms = SDL_GetTicks();
    bool has_event = false;
    const SdlWakeCounts wakes_before = ReadSdlWakeCounts();
    switch (idle_wait.mode) {
      case IdleWaitMode::Poll:
        has_event = SDL_PollEvent(&event);
        break;
      case IdleWaitMode::WaitTimeout: {
        const SdlBlockingWaitScope blocking;
        const platform::MainLoopIdleScope idle;
        has_event = SDL_WaitEventTimeout(&event, idle_wait.timeout_ms);
        break;
      }
      case IdleWaitMode::Wait: {
        const SdlBlockingWaitScope blocking;
        const platform::MainLoopIdleScope idle;
        has_event = SDL_WaitEvent(&event);
        break;
      }
    }
    if (const SdlWakeCounts wakes_after = ReadSdlWakeCounts();
        idle_wait.mode != IdleWaitMode::Poll && wakes_after.first_push_in_wait_ms != 0) {
      const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                         std::chrono::steady_clock::now().time_since_epoch())
                                                         .count());
      if (const std::optional<std::string> late = DescribeLateWake(
              now_ms - std::min(now_ms, wakes_after.first_push_in_wait_ms),
              idle_wait.mode == IdleWaitMode::WaitTimeout, idle_wait.timeout_ms, wakes_before, wakes_after,
              has_event, has_event ? event.type : 0);
          late.has_value()) {
        SDL_Log("%s", late->c_str());
      }
    }
    owed_wake_deadline = OwedWakeDeadline(owed_wake_deadline, idle_wait, wait_started_ms, has_event);
    if (has_event && IsSdlWakeNudge(event.type)) {
      continue;
    }
    if (!has_event) {
      if (idle_wait.mode == IdleWaitMode::Wait) {
        SDL_Log("SDL_WaitEvent failed: %s", SDL_GetError());
        break;
      }
      apply_scheduled_wake();
      continue;
    }

    int events_processed = 0;
    do {
      if (event.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Event next;
        while (SDL_PeepEvents(&next, 1, SDL_PEEKEVENT, SDL_EVENT_FIRST,
                              SDL_EVENT_LAST) > 0 &&
               next.type == SDL_EVENT_MOUSE_MOTION &&
               next.motion.windowID == event.motion.windowID) {
          SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_MOUSE_MOTION,
                         SDL_EVENT_MOUSE_MOTION);
        }
      }
      const auto result = HandleEvent(event);
      if (result.handled) {
        pending.reason = result.redraw.full ? "event-full" : "event-partial";
        if (pending.Absorb(result.redraw) == PendingRedraw::Absorbed::Full) {
          workspace_shell_.InvalidateCursorKindFingerprint();
        }
        if (result.redraw.full || !result.redraw.rects.empty()) {
          frame_event_type_ = event.type;
        }
      }
      ++events_processed;
      if (ShouldYieldEventDrain(events_processed, pending.FrameOwed())) {
        break;
      }
    } while (SDL_PollEvent(&event));
  }

  return 0;
}

bool Application::Initialize() {
  if (initialized_) {
    return true;
  }

  util::StartupTrace::Reset();
  util::StartupTrace::Scope trace_scope("Application::Initialize");

  editor::runtime_syntax::WarmInBackground();

  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

  if (startup_options_.handle_termination_signals) {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  }
  {
    util::StartupTrace::Scope sdl_init_scope("SDL_Init");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      SDL_Log("SDL_Init failed: %s", SDL_GetError());
      return false;
    }
  }
  if (startup_options_.handle_termination_signals && termination_signals_ == nullptr) {
    termination_event_type_ = RegisterSdlWakeChannel("termination-signal");
    termination_signals_ = std::make_unique<TerminationSignalWatcher>(termination_event_type_);
    if (!termination_signals_->installed()) {
      termination_event_type_ = 0;
    }
  }

  InstallSdlWaker();
  InstallSdlLogSink();

  const SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                       SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS;

  {
    util::StartupTrace::Scope create_window_scope("SDL_CreateWindow");
    window_ = SDL_CreateWindow(
        "microide",
        kInitialWindowWidth,
        kInitialWindowHeight,
        window_flags);
  }
  if (window_ == nullptr) {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    return false;
  }
  if (!SDL_SetWindowMinimumSize(window_, util::kMinimumLayoutWidth, util::kMinimumLayoutHeight)) {
    SDL_Log("SDL_SetWindowMinimumSize failed: %s", SDL_GetError());
  }

  {
    util::StartupTrace::Scope set_icon_scope("SDL_SetWindowIcon");
    SetWindowIconFromAssets(window_);
  }

  {
    const platform::SandboxSupport sandbox = platform::ProbeSandboxSupport();
    SDL_Log("microide sandbox: landlock=%s (abi=%d) seccomp=%s -> kernel confinement %s",
            sandbox.landlock_runtime_available ? "yes" : "no", sandbox.landlock_abi,
            sandbox.seccomp_runtime_available ? "yes" : "no",
            sandbox.fully_active() ? "active" : "unavailable (in-process gate only)");
    workspace_shell_.SetSandboxSupport(sandbox);
  }

  workspace::ControlSpec control_spec;
  {
    if (startup_options_.control_spec_path.has_value()) {
      control_spec = ReadControlSpecFile(*startup_options_.control_spec_path);
    }

    std::optional<std::filesystem::path> explicit_project;
    if (control_spec.valid && control_spec.project.has_value()) {
      explicit_project = *control_spec.project;
    } else if (startup_options_.project_path.has_value()) {
      explicit_project = startup_options_.project_path;
    }

    workspace::WorkspaceStartupOptions shell_startup;
    shell_startup.disable_plugins = startup_options_.disable_plugins;
    shell_startup.safe_mode = startup_options_.safe_mode;
    shell_startup.project_path = explicit_project;
    shell_startup.control_stdout = startup_options_.control_stdout;
    shell_startup.setting_overrides = startup_options_.setting_overrides;
    workspace_shell_.SetStartupOptions(std::move(shell_startup));

    const std::filesystem::path initial_project =
        explicit_project.has_value()  ? *explicit_project
        : startup_options_.safe_mode ? std::filesystem::path{}
                                     : CurrentDirectoryOrEmpty();

    util::StartupTrace::Scope workspace_init_scope("WorkspaceShell::Initialize");
    if (!workspace_shell_.Initialize(initial_project)) {
      SDL_Log("Workspace initialization failed");
      DestroySdlResources();
      return false;
    }
  }

  {
    util::StartupTrace::Scope create_renderer_scope("SDL_CreateRenderer");
    renderer_ = SDL_CreateRenderer(window_, nullptr);
  }
  if (renderer_ == nullptr) {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    DestroySdlResources();
    return false;
  }

  SDL_SetRenderVSync(renderer_, 1);

  {
    const std::string_view driver = render::RendererDriverName(renderer_);
    const bool is_gpu = render::RendererIsGpu(renderer_);
    SDL_Log("microide render: SDL renderer driver='%.*s' (gpu=%s)",
            static_cast<int>(driver.size()), driver.data(), is_gpu ? "yes" : "no");
    workspace_shell_.SetRenderBackendInfo(std::string(driver), is_gpu);
  }

  workspace_shell_.ForceStartControlChannel();
  workspace_shell_.ApplyStartupSettingOverrides();
  if (control_spec.valid) {
    util::StartupTrace::Scope apply_spec_scope("WorkspaceShell::ApplyControlSpec");
    workspace_shell_.ApplyControlSpec(control_spec);
  }
  workspace_shell_.SetDialogWindow(window_);

  {
    util::StartupTrace::Scope window_chrome_scope("WindowChromeSetup");
    if (!SDL_SetWindowHitTest(window_, &Application::WindowHitTestCallback, this)) {
      SDL_Log("SDL_SetWindowHitTest failed: %s", SDL_GetError());
      SDL_SetWindowBordered(window_, true);
      util::StartupTrace::Scope sync_scope("WindowChromeSetup::SyncWindow");
      SyncWindowState(window_);
    }
  }

  {
    util::StartupTrace::Scope presentation_scope("UpdateRendererPresentation");
    UpdateRendererPresentation();
  }

  {
    util::StartupTrace::Scope text_input_scope("SDL_StartTextInput");
    if (!SDL_StartTextInput(window_)) {
      SDL_Log("SDL_StartTextInput failed: %s", SDL_GetError());
    }
  }

  initialized_ = true;
  first_render_complete_ = false;
  redraw_trace_.Configure(util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_REDRAW"),
                          util::PerformanceTrace::FlagEnabled("MICROIDE_TRACE_REDRAW_VERBOSE"));
  redraw_verifier_.Configure(RedrawVerifier::EnabledFromEnvironment());
  if (const char* dump = SDL_getenv("MICROIDE_QA_STALE_DUMP_DIR"); dump != nullptr) {
    redraw_verifier_.SetDumpDirectory(dump);
  }
  return true;
}

void Application::DestroySdlResources() {
  if (window_ != nullptr) {
    SDL_StopTextInput(window_);
  }
  redraw_verifier_.Destroy();
  scene_texture_.Destroy();
  if (renderer_ != nullptr) {
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
  }
  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
}

void Application::Shutdown() {
  if (!initialized_) {
    DestroySdlResources();
    return;
  }

  workspace_shell_.SetDialogWindow(nullptr);

  DestroySdlResources();

  workspace_shell_.Shutdown();

  util::PerformanceTrace::DumpSummaryOnce();
  util::StartupTrace::DumpSummaryOnce();
  util::DumpPerformanceCountersOnce();

  initialized_ = false;
  first_render_complete_ = false;
  presentation_state_dirty_ = true;

  if (startup_options_.quick_exit_on_shutdown) {
    (void)util::FlushCoverageProfile();
    std::quick_exit(0);
  }
}

workspace::WorkspaceShell::EventResult Application::HandleEvent(const SDL_Event& event) {
  if (termination_event_type_ != 0 && event.type == termination_event_type_) {
    SDL_Log("microide: signal %d received; saving the session and quitting", static_cast<int>(event.user.code));
    workspace_shell_.RequestQuit(true);
    if (workspace_shell_.ConsumeQuitRequested()) {
      running_ = false;
    }
    return workspace::WorkspaceShell::EventResult{.handled = true, .redraw = {}};
  }
  switch (event.type) {
    case SDL_EVENT_QUIT:
      SDL_Log("microide quit: SDL_EVENT_QUIT (the window manager closed the window)");
      workspace_shell_.RequestQuit();
      if (workspace_shell_.ConsumeQuitRequested()) {
        running_ = false;
      }
      return workspace::WorkspaceShell::EventResult{
          .handled = true,
          .redraw = {},
      };
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_EXPOSED:
      presentation_state_dirty_ = true;
      UpdateRendererPresentation();
      if (scene_texture_.valid()) {
        return workspace::WorkspaceShell::EventResult{
            .handled = true,
            .redraw = workspace::WorkspaceShell::RenderInvalidation{
                .full = false,
                .rects = {SDL_FRect{0.0f, 0.0f, 1.0f, 1.0f}},
            },
        };
      }
      return HandledWithFullRedraw();
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
      presentation_state_dirty_ = true;
      UpdateRendererPresentation();
      return HandledWithFullRedraw();
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
      presentation_state_dirty_ = true;
      scene_texture_.NoteResizeEvent(SDL_GetTicksNS());
      UpdateRendererPresentation();
      ReseedPointerAndForceCursorReassert();
      return HandledWithFullRedraw();
    case SDL_EVENT_WINDOW_MOVED:
      ReseedPointerAndForceCursorReassert();
      return HandledWithFullRedraw();
    default:
      break;
  }

  SDL_Event converted_event = event;
  if (renderer_ != nullptr && EventUsesRenderCoordinates(event.type)) {
    SDL_ConvertEventToRenderCoordinates(renderer_, &converted_event);
  }

  auto result = workspace_shell_.HandleEvent(converted_event);
  ConsumeWindowActions();
  if (workspace_shell_.ConsumeQuitRequested()) {
    SDL_Log("microide quit: the main loop ends after event 0x%x", static_cast<unsigned>(event.type));
    running_ = false;
    return workspace::WorkspaceShell::EventResult{
        .handled = true,
        .redraw = result.redraw,
    };
  }
  return result;
}

void Application::PresentRetainedScene() {
  SDL_RenderTexture(renderer_, scene_texture_.texture(), nullptr, nullptr);
  SDL_RenderPresent(renderer_);
}

void Application::Render(std::vector<SDL_FRect> dirty_rects, const char* reason, bool prepared_only) {
  if (renderer_ == nullptr) {
    return;
  }
  ++frame_index_;

  bool full_redraw_requested = (dirty_rects.empty() && !prepared_only) || !scene_texture_.valid();
  SDL_FRect dirty_bounds{};
  if (redraw_trace_.verbose() && !dirty_rects.empty()) {
    dirty_bounds = dirty_rects.front();
    for (const SDL_FRect& rect : dirty_rects) {
      SDL_GetRectUnionFloat(&dirty_bounds, &rect, &dirty_bounds);
    }
  }
  util::PerformanceTrace::Scope trace_scope(
      full_redraw_requested ? "Application::Render(full)" : "Application::Render(partial)");
  std::optional<util::StartupTrace::Scope> first_render_scope;
  if (!first_render_complete_ && util::StartupTrace::Enabled()) {
    first_render_scope.emplace("Application::FirstRender");
  }

  int width = 0;
  int height = 0;
  {
    util::PerformanceTrace::Scope presentation_scope("Application::UpdateRendererPresentation");
    if (!UpdateRendererPresentation(&width, &height)) {
      return;
    }
  }

  const Uint64 render_start = SDL_GetTicksNS();
  bool scene_texture_ready = false;
  {
    util::PerformanceTrace::Scope scene_texture_scope("Application::EnsureSceneTexture");
    const auto presentation = workspace_shell_.CurrentWindowPresentationState();
    scene_texture_ready =
        presentation.has_value() && scene_texture_.Ensure(renderer_, RenderPresentationOf(*presentation));
  }

  workspace::WorkspaceShell::FrameToken frame_token;
  {
    util::PerformanceTrace::Scope prepare_scope("Application::WorkspacePrepareFrame");
    frame_token = workspace_shell_.PrepareFrameOnce(renderer_, width, height);
  }
  if (const workspace::RenderInvalidation prepared =
          workspace_shell_.ConsumePendingRenderInvalidation();
      prepared_only && scene_texture_ready && scene_texture_.valid()) {
    if (!prepared.full && prepared.rects.empty()) {
      return;
    }
    full_redraw_requested = prepared.full;
    dirty_rects.assign(prepared.rects.begin(), prepared.rects.end());
  } else if (prepared_only) {
    dirty_rects.clear();
  } else if (!dirty_rects.empty()) {
    if (prepared.full) {
      dirty_rects.clear();
    } else {
      dirty_rects.insert(dirty_rects.end(), prepared.rects.begin(), prepared.rects.end());
    }
  }

  const std::size_t dirty_rect_count = dirty_rects.size();
  const auto presentation = workspace_shell_.CurrentWindowPresentationState();
  const render::TextClipPadding clip_padding = PadClipForPresentationScale(
      workspace_shell_.PartialRedrawClipPadding(), presentation ? presentation->scale_x : 1.0f,
      presentation ? presentation->scale_y : 1.0f);
  AnalyzeDirtyRegions(dirty_rects, clip_padding, width, height,
                      SDL_FPoint{presentation ? presentation->scale_x : 1.0f,
                                 presentation ? presentation->scale_y : 1.0f},
                      dirty_region_analysis_);
  const DirtyRegionAnalysis& dirty_region_analysis = dirty_region_analysis_;
  const std::size_t merged_clip_count = dirty_region_analysis.merged_clip_rects.size();
  const float dirty_coverage = dirty_region_analysis.coverage;
  const bool promote_partial_to_full =
      scene_texture_.valid() && ShouldPromotePartialFrameToFull(dirty_region_analysis);
  const bool full_redraw = dirty_rects.empty() || !scene_texture_.valid() || promote_partial_to_full;
  std::size_t rendered_clip_count = 0;
#if !defined(NDEBUG)
  SDL_assert(frame_token.valid() &&
             "Application::WorkspaceRenderClip requires PrepareFrameOnce() in current frame");
#endif
  const auto render_fallback_full = [&]() {
    {
      util::PerformanceTrace::Scope fallback_scope("Application::WorkspaceRender(fallback-full)");
      util::AddPerformanceCounter(util::PerfCounterId::RenderSceneFallbackFrames);
      workspace_shell_.RenderClip(frame_token, renderer_, width, height);
    }
    SDL_RenderPresent(renderer_);
    redraw_trace_.Record(RedrawFrameStats{.full_redraw_requested = true,
                                          .full_redraw = true,
                                          .promoted_partial_to_full = false,
                                          .dirty_rect_count = 0,
                                          .rendered_clip_count = 0,
                                          .frame_index = frame_index_,
                                          .reason = "fallback-full",
                                          .elapsed_ns = SDL_GetTicksNS() - render_start});
  };
  if (scene_texture_ready) {
    if (!SDL_SetRenderTarget(renderer_, scene_texture_.texture())) {
      SDL_Log("SDL_SetRenderTarget(scene texture) failed: %s", SDL_GetError());
      scene_texture_.Destroy();
      render_fallback_full();
      first_render_complete_ = true;
      workspace_shell_.OnFramePresented();
      return;
    }
    scene_texture_.ApplyRenderTargetPresentation(renderer_);

    if (full_redraw) {
      if (promote_partial_to_full) {
        util::AddPerformanceCounter(util::PerfCounterId::RenderPromotedFullFrames);
        if (util::PerformanceTrace::Enabled()) {
          SDL_Log(
              "microide perf: promoting partial frame to full redraw (%zu dirty rects, %zu coalesced clip rects, %.1f%% coalesced coverage)",
              dirty_rect_count, merged_clip_count, dirty_coverage * 100.0f);
        }
      }
      util::PerformanceTrace::Scope workspace_scope("Application::WorkspaceRender(full)");
      workspace_shell_.RenderClip(frame_token, renderer_, width, height);
    } else {
      bool rendered_partial = false;
      util::PerformanceTrace::ScopeLabel partial_loop_label(
          "Application::WorkspaceRender(partial-loop)");
      partial_loop_label.Field("clips", static_cast<long long>(merged_clip_count));
      partial_loop_label.Field("dirty", static_cast<long long>(dirty_rect_count));
      util::PerformanceTrace::Scope partial_loop_scope(partial_loop_label.View());
      for (const SDL_Rect& merged_clip : dirty_region_analysis.merged_clip_rects) {
        const SDL_Rect clip_rect = render::AlignClipToDevicePixels(renderer_, merged_clip);
        SDL_SetRenderClipRect(renderer_, &clip_rect);
        util::PerformanceTrace::Scope partial_scope(
            "Application::WorkspaceRender(partial-clip)");
        const SDL_FRect dirty_rect_hint = DeviceClipHint(
            clip_rect, presentation ? presentation->scale_x : 1.0f,
            presentation ? presentation->scale_y : 1.0f);
        workspace_shell_.RenderClip(frame_token, renderer_, width, height, dirty_rect_hint);
        util::AddPerformanceCounter(util::PerfCounterId::RenderPartialClips);
        util::AddPerformanceCounter(
            util::PerfCounterId::RenderPartialClipPixels,
            static_cast<std::uint64_t>(clip_rect.w) * static_cast<std::uint64_t>(clip_rect.h));
        rendered_partial = true;
        ++rendered_clip_count;
      }
      if (!rendered_partial) {
        util::PerformanceTrace::Scope partial_fallback_scope(
            "Application::WorkspaceRender(partial-fallback-full)");
        workspace_shell_.RenderClip(frame_token, renderer_, width, height);
        dirty_rects.clear();
      }
    }

    SDL_SetRenderClipRect(renderer_, nullptr);
    if (redraw_verifier_.enabled() && presentation.has_value() && !full_redraw && !dirty_rects.empty()) {
      redraw_verifier_.Verify(
          renderer_, scene_texture_.texture(), RenderPresentationOf(*presentation),
          VerifiedFrameInfo{.clips = dirty_region_analysis.merged_clip_rects,
                            .reason = reason,
                            .frame_index = frame_index_,
                            .scale = RenderPresentationOf(*presentation).scale_x},
          [&] { workspace_shell_.RenderClip(frame_token, renderer_, width, height); });
    }
    if (redraw_verifier_.enabled()) {
      for (const render::PaintOverrun& overrun : render::TakePaintOverruns()) {
        SDL_Log("QA-OVERRUN frame=%llu element=%s rect=%d,%d,%d,%d draw=%d,%d,%d,%d logical=%.3f,%.3f,%.3f,%.3f "
                "draw-logical=%.3f,%.3f,%.3f,%.3f scale=%.3f,%.3f",
                static_cast<unsigned long long>(frame_index_), overrun.element.c_str(), overrun.element_rect.x,
                overrun.element_rect.y, overrun.element_rect.w, overrun.element_rect.h, overrun.draw_rect.x,
                overrun.draw_rect.y, overrun.draw_rect.w, overrun.draw_rect.h, overrun.element_logical.x,
                overrun.element_logical.y, overrun.element_logical.w, overrun.element_logical.h,
                overrun.draw_logical.x, overrun.draw_logical.y, overrun.draw_logical.w, overrun.draw_logical.h,
                overrun.scale.x, overrun.scale.y);
      }
    }
    SDL_SetRenderTarget(renderer_, nullptr);
    {
      util::PerformanceTrace::Scope present_scope("Application::PresentRetainedScene");
      PresentRetainedScene();
    }
    util::AddPerformanceCounter(util::PerfCounterId::RenderFramesRetained);
    scene_texture_.MarkValid();
    if (!full_redraw && util::PerformanceTrace::Enabled() &&
        rendered_clip_count >= kRenderPerfPartialClipWarnThreshold) {
      SDL_Log(
          "microide perf: partial frame replayed %zu coalesced clip rects from %zu dirty rects",
          rendered_clip_count, dirty_rect_count);
    }
    redraw_trace_.Record(RedrawFrameStats{.full_redraw_requested = full_redraw_requested,
                                          .full_redraw = full_redraw,
                                          .promoted_partial_to_full = promote_partial_to_full,
                                          .dirty_rect_count = dirty_rect_count,
                                          .rendered_clip_count = rendered_clip_count,
                                          .frame_index = frame_index_,
                                          .reason = reason,
                                          .elapsed_ns = SDL_GetTicksNS() - render_start,
                                          .dirty_bounds = dirty_bounds,
                                          .event_type = std::exchange(frame_event_type_, 0)});
  } else {
    render_fallback_full();
  }
  first_render_complete_ = true;
  workspace_shell_.OnFramePresented();
}


void Application::ApplyMinimumWindowSize(SDL_FPoint window_per_unit) {
  util::WindowSize usable{};
  SDL_Rect bounds{};
  if (const SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
      display != 0 && SDL_GetDisplayUsableBounds(display, &bounds)) {
    usable = util::WindowSize{.width = bounds.w, .height = bounds.h};
  }
  const util::WindowSize minimum = util::MinimumWindowSize(window_per_unit.x, window_per_unit.y, usable);
  if (minimum.width == applied_minimum_window_width_ && minimum.height == applied_minimum_window_height_) {
    return;
  }
  applied_minimum_window_width_ = minimum.width;
  applied_minimum_window_height_ = minimum.height;
  if (!SDL_SetWindowMinimumSize(window_, minimum.width, minimum.height)) {
    SDL_Log("SDL_SetWindowMinimumSize failed: %s", SDL_GetError());
  }
  int width = 0;
  int height = 0;
  SDL_GetWindowSize(window_, &width, &height);
  if (width < minimum.width || height < minimum.height) {
    SDL_SetWindowSize(window_, std::max(width, minimum.width), std::max(height, minimum.height));
  }
}

bool Application::UpdateRendererPresentation(int* logical_width, int* logical_height) {
  if (window_ == nullptr || renderer_ == nullptr) {
    return false;
  }

  const float current_ui_scale = workspace_shell_.UiScale();
  const auto cached = workspace_shell_.CurrentWindowPresentationState();
  if (CanReuseCachedPresentationState(presentation_state_dirty_, cached, last_presented_ui_scale_,
                                      current_ui_scale)) {
    if (logical_width != nullptr) {
      *logical_width = cached->logical_width;
    }
    if (logical_height != nullptr) {
      *logical_height = cached->logical_height;
    }
    return true;
  }

  const auto presentation = CaptureWindowPresentationState(window_, renderer_, current_ui_scale);
  if (!presentation.has_value()) {
    return false;
  }

  workspace_shell_.SetWindowPresentationState(*presentation);
  ApplyMinimumWindowSize(presentation->window_per_unit);
  if (!ApplyRenderPresentation(renderer_, RenderPresentationOf(*presentation))) {
    SDL_Log("ApplyRenderPresentation failed: %s", SDL_GetError());
    return false;
  }

  presentation_state_dirty_ = false;
  last_presented_ui_scale_ = current_ui_scale;
  if (logical_width != nullptr) {
    *logical_width = presentation->logical_width;
  }
  if (logical_height != nullptr) {
    *logical_height = presentation->logical_height;
  }
  return true;
}

void Application::ReseedPointerAndForceCursorReassert() {
  if (renderer_ != nullptr) {
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_RenderCoordinatesFromWindow(renderer_, mouse_x, mouse_y, &mouse_x, &mouse_y);
    workspace_shell_.SeedPointerPosition(mouse_x, mouse_y);
  }
  workspace_shell_.ForceCursorReassert();
}

void Application::ConsumeWindowActions() {
  if (window_ == nullptr) {
    return;
  }

  switch (workspace_shell_.ConsumeWindowAction()) {
    case workspace::WorkspaceShell::WindowAction::None:
      return;
    case workspace::WorkspaceShell::WindowAction::Minimize:
      SDL_MinimizeWindow(window_);
      return;
    case workspace::WorkspaceShell::WindowAction::ToggleMaximize: {
      const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
      if ((flags & SDL_WINDOW_MAXIMIZED) != 0) {
        SDL_RestoreWindow(window_);
      } else {
        SDL_MaximizeWindow(window_);
      }
      SyncWindowState(window_);
      UpdateRendererPresentation();
      return;
    }
    case workspace::WorkspaceShell::WindowAction::ToggleFullscreen: {
      const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
      SDL_SetWindowFullscreen(window_, (flags & SDL_WINDOW_FULLSCREEN) == 0);
      SyncWindowState(window_);
      UpdateRendererPresentation();
      return;
    }
  }
}

SDL_HitTestResult Application::WindowHitTest(const SDL_Point& area) const {
  if (renderer_ == nullptr) {
    return SDL_HITTEST_NORMAL;
  }

  float render_x = static_cast<float>(area.x);
  float render_y = static_cast<float>(area.y);
  if (!SDL_RenderCoordinatesFromWindow(renderer_, static_cast<float>(area.x),
                                       static_cast<float>(area.y), &render_x, &render_y)) {
    return SDL_HITTEST_NORMAL;
  }

  return workspace_shell_.WindowHitTest(render_x, render_y);
}

SDL_HitTestResult SDLCALL Application::WindowHitTestCallback(SDL_Window* window,
                                                             const SDL_Point* area,
                                                             void* data) {
  (void) window;
  if (area == nullptr || data == nullptr) {
    return SDL_HITTEST_NORMAL;
  }
  return static_cast<Application*>(data)->WindowHitTest(*area);
}

}

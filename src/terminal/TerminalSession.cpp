#include "terminal/TerminalSession.h"

#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <util.h>
#elif defined(__unix__)
#include <pty.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace microide::terminal {

namespace {

constexpr std::size_t kMaxScrollbackLines = 2000;
#if defined(__unix__) || defined(__APPLE__)
constexpr auto kTerminalHangupGrace = std::chrono::milliseconds(75);
constexpr auto kTerminalTerminateGrace = std::chrono::milliseconds(150);
constexpr auto kTerminalKillGrace = std::chrono::milliseconds(100);
constexpr auto kTerminalWaitPollInterval = std::chrono::milliseconds(10);
#endif

SDL_Color MakeColor(Uint8 r, Uint8 g, Uint8 b, Uint8 a = 0xff) {
  return SDL_Color{r, g, b, a};
}

SDL_Color BasicAnsiColor(int index, bool bright) {
  static const std::array<SDL_Color, 8> kNormal = {
      MakeColor(0x1f, 0x24, 0x2c), MakeColor(0xc3, 0x4b, 0x59),
      MakeColor(0x8a, 0xb1, 0x66), MakeColor(0xd8, 0xb2, 0x5d),
      MakeColor(0x5a, 0x8c, 0xe6), MakeColor(0xb0, 0x72, 0xd1),
      MakeColor(0x56, 0xa8, 0xc9), MakeColor(0xb8, 0xc0, 0xcc),
  };
  static const std::array<SDL_Color, 8> kBright = {
      MakeColor(0x4a, 0x51, 0x5c), MakeColor(0xf0, 0x71, 0x78),
      MakeColor(0xa4, 0xc7, 0x6d), MakeColor(0xe7, 0xc5, 0x47),
      MakeColor(0x72, 0xa7, 0xff), MakeColor(0xcb, 0x8f, 0xf8),
      MakeColor(0x74, 0xc7, 0xec), MakeColor(0xf5, 0xf7, 0xfa),
  };
  const int clamped_index = std::clamp(index, 0, 7);
  return bright ? kBright[clamped_index] : kNormal[clamped_index];
}

SDL_Color Ansi256Color(int index) {
  if (index < 0) {
    return BasicAnsiColor(0, false);
  }
  if (index < 8) {
    return BasicAnsiColor(index, false);
  }
  if (index < 16) {
    return BasicAnsiColor(index - 8, true);
  }
  if (index < 232) {
    const int value = index - 16;
    const int red = value / 36;
    const int green = (value / 6) % 6;
    const int blue = value % 6;
    static constexpr std::array<Uint8, 6> kCube = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};
    return MakeColor(kCube[red], kCube[green], kCube[blue]);
  }
  const Uint8 gray = static_cast<Uint8>(8 + (index - 232) * 10);
  return MakeColor(gray, gray, gray);
}

std::optional<int> Base64Value(unsigned char character) {
  if (character >= 'A' && character <= 'Z') {
    return static_cast<int>(character - 'A');
  }
  if (character >= 'a' && character <= 'z') {
    return static_cast<int>(character - 'a' + 26);
  }
  if (character >= '0' && character <= '9') {
    return static_cast<int>(character - '0' + 52);
  }
  if (character == '+') {
    return 62;
  }
  if (character == '/') {
    return 63;
  }
  return std::nullopt;
}

std::optional<std::string> DecodeBase64(std::string_view text) {
  std::string compact;
  compact.reserve(text.size());
  for (unsigned char character : text) {
    if (character == '\r' || character == '\n' || character == '\t' || character == ' ') {
      continue;
    }
    compact.push_back(static_cast<char>(character));
  }

  if (compact.empty()) {
    return std::string{};
  }
  if ((compact.size() % 4) != 0) {
    return std::nullopt;
  }

  std::string decoded;
  decoded.reserve((compact.size() / 4) * 3);
  for (std::size_t index = 0; index < compact.size(); index += 4) {
    const char a = compact[index];
    const char b = compact[index + 1];
    const char c = compact[index + 2];
    const char d = compact[index + 3];

    const auto a_value = Base64Value(static_cast<unsigned char>(a));
    const auto b_value = Base64Value(static_cast<unsigned char>(b));
    if (!a_value.has_value() || !b_value.has_value()) {
      return std::nullopt;
    }
    if ((c == '=' && d != '=') || (index + 4 != compact.size() && (c == '=' || d == '='))) {
      return std::nullopt;
    }

    const int sextet0 = *a_value;
    const int sextet1 = *b_value;
    const int sextet2 =
        c == '=' ? 0 : Base64Value(static_cast<unsigned char>(c)).value_or(-1);
    const int sextet3 =
        d == '=' ? 0 : Base64Value(static_cast<unsigned char>(d)).value_or(-1);
    if (sextet2 < 0 || sextet3 < 0) {
      return std::nullopt;
    }

    decoded.push_back(static_cast<char>((sextet0 << 2) | (sextet1 >> 4)));
    if (c != '=') {
      decoded.push_back(static_cast<char>(((sextet1 & 0x0f) << 4) | (sextet2 >> 2)));
    }
    if (d != '=') {
      decoded.push_back(static_cast<char>(((sextet2 & 0x03) << 6) | sextet3));
    }
  }

  return decoded;
}

#if defined(__unix__) || defined(__APPLE__)
bool SendSignalToTerminalProcessGroup(int child_pid, int signal_number) {
  if (child_pid <= 0) {
    return true;
  }
  if (kill(-child_pid, signal_number) == 0) {
    return true;
  }
  if (kill(child_pid, signal_number) == 0) {
    return true;
  }
  return errno == ESRCH;
}

bool ReapTerminalChildNoHang(int child_pid) {
  if (child_pid <= 0) {
    return true;
  }

  int status = 0;
  while (true) {
    const pid_t result = waitpid(child_pid, &status, WNOHANG);
    if (result == child_pid) {
      return true;
    }
    if (result == 0) {
      return false;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return result < 0 && errno == ECHILD;
  }
}

bool WaitForTerminalChildExit(int child_pid, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ReapTerminalChildNoHang(child_pid)) {
      return true;
    }
    std::this_thread::sleep_for(kTerminalWaitPollInterval);
  }
  return ReapTerminalChildNoHang(child_pid);
}

void RequestTerminalChildShutdown(int child_pid) {
  if (child_pid <= 0 || ReapTerminalChildNoHang(child_pid)) {
    return;
  }

  SendSignalToTerminalProcessGroup(child_pid, SIGHUP);
  if (WaitForTerminalChildExit(child_pid, kTerminalHangupGrace)) {
    return;
  }

  SendSignalToTerminalProcessGroup(child_pid, SIGTERM);
  if (WaitForTerminalChildExit(child_pid, kTerminalTerminateGrace)) {
    return;
  }

  SendSignalToTerminalProcessGroup(child_pid, SIGKILL);
  WaitForTerminalChildExit(child_pid, kTerminalKillGrace);
}
#endif

std::string DefaultShellPath() {
  if (const char* shell = std::getenv("SHELL"); shell != nullptr && shell[0] != '\0') {
    return shell;
  }
  return "/bin/sh";
}

std::string ShellProgramName(const std::string& shell_path) {
  const std::size_t slash = shell_path.find_last_of("/\\");
  return slash == std::string::npos ? shell_path : shell_path.substr(slash + 1);
}

std::vector<int> ParseCsiParameters(std::string_view body) {
  std::vector<int> params;
  std::string current;
  for (char character : body) {
    if (character == ';') {
      params.push_back(current.empty() ? 0 : std::atoi(current.c_str()));
      current.clear();
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(character)) ||
        ((character == '-' || character == '+') && current.empty())) {
      current.push_back(character);
      continue;
    }
  }
  if (!current.empty() || (!body.empty() && body.back() == ';')) {
    params.push_back(current.empty() ? 0 : std::atoi(current.c_str()));
  }
  return params;
}

int CsiParamOrDefault(const std::vector<int>& params, std::size_t index, int fallback) {
  if (index >= params.size() || params[index] <= 0) {
    return fallback;
  }
  return params[index];
}

int MouseModifierBits(SDL_Keymod modifiers) {
  int bits = 0;
  if ((modifiers & SDL_KMOD_SHIFT) != 0) {
    bits |= 4;
  }
  if ((modifiers & SDL_KMOD_ALT) != 0) {
    bits |= 8;
  }
  if ((modifiers & SDL_KMOD_CTRL) != 0) {
    bits |= 16;
  }
  return bits;
}

std::string SanitizeOscTitle(std::string_view text) {
  std::string sanitized;
  sanitized.reserve(std::min<std::size_t>(text.size(), 256));
  for (unsigned char character : text) {
    if ((character < 0x20 && character != ' ') || character == 0x7f) {
      continue;
    }
    sanitized.push_back(static_cast<char>(character));
    if (sanitized.size() >= 256) {
      break;
    }
  }

  const std::size_t first = sanitized.find_first_not_of(' ');
  if (first == std::string::npos) {
    return {};
  }
  const std::size_t last = sanitized.find_last_not_of(' ');
  return sanitized.substr(first, last - first + 1);
}

bool ClipboardPayloadIsText(std::string_view text) {
  for (unsigned char character : text) {
    if (character == '\0') {
      return false;
    }
  }
  return true;
}

TerminalCell MakeAsciiTerminalCell(char character, const TerminalStyle& style) {
  return TerminalCell{
      .character = character,
      .text = {},
      .style = style,
  };
}

TerminalCell MakeUtf8TerminalCell(std::string_view glyph, const TerminalStyle& style) {
  if (glyph.size() == 1 && static_cast<unsigned char>(glyph.front()) < 0x80) {
    return MakeAsciiTerminalCell(glyph.front(), style);
  }
  return TerminalCell{
      .character = '\0',
      .text = std::string(glyph),
      .style = style,
  };
}

}  // namespace

TerminalSession::~TerminalSession() {
  Stop();
}

void TerminalSession::SetWakeEventType(Uint32 event_type) {
  std::scoped_lock lock(mutex_);
  wake_event_type_ = event_type;
  if (event_type == 0) {
    wake_event_pending_ = false;
  }
}

bool TerminalSession::Start(const std::filesystem::path& working_directory, std::string_view command) {
  Stop();

#if !defined(__unix__) && !defined(__APPLE__)
  {
    std::scoped_lock lock(mutex_);
    working_directory_ = working_directory;
    default_launch_label_ = command.empty() ? "terminal unavailable" : std::string(command);
    launch_label_ = default_launch_label_;
    lines_ = {TerminalLine{}};
    running_ = false;
    stop_requested_ = false;
    wake_event_pending_ = false;
    current_style_ = TerminalStyle{};
    escape_sequence_buffer_.clear();
    pending_utf8_sequence_.clear();
    escape_mode_ = EscapeMode::None;
    osc_escape_pending_ = false;
    pending_clipboard_text_.reset();
    use_alternate_screen_ = false;
    mouse_tracking_normal_ = false;
    mouse_tracking_drag_ = false;
    mouse_tracking_any_ = false;
    mouse_sgr_ext_mode_ = false;
    application_cursor_keys_mode_ = false;
    origin_mode_ = false;
    auto_wrap_mode_ = true;
    bracketed_paste_mode_ = false;
    focus_event_mode_ = false;
    cursor_visible_ = true;
    primary_screen_ = ScreenState{};
    alternate_screen_ = ScreenState{};
    rows_ = 24;
    columns_ = 80;
    cursor_row_ = 0;
    cursor_column_ = 0;
    saved_cursor_row_ = 0;
    saved_cursor_column_ = 0;
    ResetScrollRegionLocked();
    AppendOutputLocked("terminal support is only available on POSIX hosts.");
  }
  PushWakeEvent();
  return false;
#else
  int master_fd = -1;
  int slave_fd = -1;
  winsize window_size{};
  window_size.ws_row = 24;
  window_size.ws_col = 80;
  if (openpty(&master_fd, &slave_fd, nullptr, nullptr, &window_size) != 0) {
    return false;
  }

  const std::string shell_path = DefaultShellPath();
  const std::string shell_name = ShellProgramName(shell_path);
  const std::string command_string(command);

  const pid_t child_pid = fork();
  if (child_pid < 0) {
    close(master_fd);
    close(slave_fd);
    return false;
  }

  if (child_pid == 0) {
    setsid();
    ioctl(slave_fd, TIOCSCTTY, 0);
    dup2(slave_fd, STDIN_FILENO);
    dup2(slave_fd, STDOUT_FILENO);
    dup2(slave_fd, STDERR_FILENO);
    close(master_fd);
    if (slave_fd > STDERR_FILENO) {
      close(slave_fd);
    }

    chdir(working_directory.c_str());
    setenv("TERM", "xterm-256color", 1);
    if (command_string.empty()) {
      execl(shell_path.c_str(), shell_name.c_str(), "-i", nullptr);
    } else {
      execl(shell_path.c_str(), shell_name.c_str(), "-lc", command_string.c_str(), nullptr);
    }
    _exit(127);
  }

  close(slave_fd);

  {
    std::scoped_lock lock(mutex_);
    working_directory_ = working_directory;
    default_launch_label_ = command_string.empty() ? shell_name : command_string;
    launch_label_ = default_launch_label_;
    lines_ = {TerminalLine{}};
    current_style_ = TerminalStyle{};
    escape_sequence_buffer_.clear();
    pending_utf8_sequence_.clear();
    master_fd_ = master_fd;
    child_pid_ = child_pid;
    running_ = true;
    stop_requested_ = false;
    wake_event_pending_ = false;
    escape_mode_ = EscapeMode::None;
    osc_escape_pending_ = false;
    pending_clipboard_text_.reset();
    use_alternate_screen_ = false;
    mouse_tracking_normal_ = false;
    mouse_tracking_drag_ = false;
    mouse_tracking_any_ = false;
    mouse_sgr_ext_mode_ = false;
    application_cursor_keys_mode_ = false;
    origin_mode_ = false;
    auto_wrap_mode_ = true;
    bracketed_paste_mode_ = false;
    focus_event_mode_ = false;
    cursor_visible_ = true;
    primary_screen_ = ScreenState{};
    alternate_screen_ = ScreenState{};
    rows_ = 24;
    columns_ = 80;
    cursor_row_ = 0;
    cursor_column_ = 0;
    saved_cursor_row_ = 0;
    saved_cursor_column_ = 0;
    ResetScrollRegionLocked();
  }

  reader_thread_ = std::thread(&TerminalSession::ReaderMain, this, master_fd, child_pid);
  PushWakeEvent();
  return true;
#endif
}

void TerminalSession::Stop() {
#if defined(__unix__) || defined(__APPLE__)
  int master_fd = -1;
  int child_pid = -1;
  {
    std::scoped_lock lock(mutex_);
    stop_requested_ = true;
    master_fd = master_fd_;
    child_pid = child_pid_;
    master_fd_ = -1;
  }

  if (master_fd >= 0) {
    close(master_fd);
  }
  RequestTerminalChildShutdown(child_pid);

  if (reader_thread_.joinable()) {
    reader_thread_.join();
  }

  {
    std::scoped_lock lock(mutex_);
    running_ = false;
    child_pid_ = -1;
    master_fd_ = -1;
    stop_requested_ = false;
    current_style_ = TerminalStyle{};
    escape_sequence_buffer_.clear();
    pending_utf8_sequence_.clear();
    default_launch_label_.clear();
    launch_label_.clear();
    escape_mode_ = EscapeMode::None;
    osc_escape_pending_ = false;
    pending_clipboard_text_.reset();
    use_alternate_screen_ = false;
    mouse_tracking_normal_ = false;
    mouse_tracking_drag_ = false;
    mouse_tracking_any_ = false;
    mouse_sgr_ext_mode_ = false;
    application_cursor_keys_mode_ = false;
    origin_mode_ = false;
    auto_wrap_mode_ = true;
    bracketed_paste_mode_ = false;
    focus_event_mode_ = false;
    cursor_visible_ = true;
    primary_screen_ = ScreenState{};
    alternate_screen_ = ScreenState{};
    cursor_row_ = 0;
    cursor_column_ = 0;
    saved_cursor_row_ = 0;
    saved_cursor_column_ = 0;
    ResetScrollRegionLocked();
    if (lines_.empty()) {
      lines_.push_back(TerminalLine{});
    }
  }
#else
  if (reader_thread_.joinable()) {
    reader_thread_.join();
  }

  std::scoped_lock lock(mutex_);
  running_ = false;
  stop_requested_ = false;
  current_style_ = TerminalStyle{};
  escape_sequence_buffer_.clear();
  pending_utf8_sequence_.clear();
  default_launch_label_.clear();
  launch_label_.clear();
  escape_mode_ = EscapeMode::None;
  osc_escape_pending_ = false;
  pending_clipboard_text_.reset();
  use_alternate_screen_ = false;
  mouse_tracking_normal_ = false;
  mouse_tracking_drag_ = false;
  mouse_tracking_any_ = false;
  mouse_sgr_ext_mode_ = false;
  application_cursor_keys_mode_ = false;
  origin_mode_ = false;
  auto_wrap_mode_ = true;
  bracketed_paste_mode_ = false;
  focus_event_mode_ = false;
  cursor_visible_ = true;
  primary_screen_ = ScreenState{};
  alternate_screen_ = ScreenState{};
  cursor_row_ = 0;
  cursor_column_ = 0;
  saved_cursor_row_ = 0;
  saved_cursor_column_ = 0;
  ResetScrollRegionLocked();
  if (lines_.empty()) {
    lines_.push_back(TerminalLine{});
  }
#endif
}

void TerminalSession::Resize(std::size_t rows, std::size_t columns) {
  util::PerformanceTrace::Scope trace_scope("TerminalSession::Resize");
  const std::size_t clamped_rows = std::max<std::size_t>(1, rows);
  const std::size_t clamped_columns = std::max<std::size_t>(1, columns);

  {
    std::scoped_lock lock(mutex_);
    rows_ = clamped_rows;
    columns_ = clamped_columns;
    if (use_alternate_screen_ && rows_ > 0) {
      cursor_row_ = std::min(cursor_row_, rows_ - 1);
      saved_cursor_row_ = std::min(saved_cursor_row_, rows_ - 1);
    }
    if (columns_ > 0) {
      cursor_column_ = std::min(cursor_column_, columns_ - 1);
      saved_cursor_column_ = std::min(saved_cursor_column_, columns_ - 1);
    }
    primary_screen_.cursor_column = columns_ > 0 ? std::min(primary_screen_.cursor_column, columns_ - 1)
                                                 : primary_screen_.cursor_column;
    primary_screen_.saved_cursor_column =
        columns_ > 0 ? std::min(primary_screen_.saved_cursor_column, columns_ - 1)
                     : primary_screen_.saved_cursor_column;
    if (rows_ > 0) {
      alternate_screen_.cursor_row = std::min(alternate_screen_.cursor_row, rows_ - 1);
      alternate_screen_.saved_cursor_row = std::min(alternate_screen_.saved_cursor_row, rows_ - 1);
    }
    alternate_screen_.cursor_column =
        columns_ > 0 ? std::min(alternate_screen_.cursor_column, columns_ - 1)
                     : alternate_screen_.cursor_column;
    alternate_screen_.saved_cursor_column =
        columns_ > 0 ? std::min(alternate_screen_.saved_cursor_column, columns_ - 1)
                     : alternate_screen_.saved_cursor_column;
    ClampScrollRegionLocked();
    if (use_alternate_screen_) {
      lines_.resize(std::max<std::size_t>(1, rows_));
    }
    EnsureCursorLineExistsLocked();
    TrimScrollbackLocked();
  }

#if defined(__unix__) || defined(__APPLE__)
  int master_fd = -1;
  {
    std::scoped_lock lock(mutex_);
    master_fd = master_fd_;
  }
  if (master_fd < 0) {
    return;
  }

  winsize window_size{};
  window_size.ws_row = static_cast<unsigned short>(std::min<std::size_t>(clamped_rows, 65535));
  window_size.ws_col = static_cast<unsigned short>(std::min<std::size_t>(clamped_columns, 65535));
  ioctl(master_fd, TIOCSWINSZ, &window_size);
#else
  (void)rows;
  (void)columns;
#endif
}

void TerminalSession::SendBytes(std::string_view bytes) {
#if defined(__unix__) || defined(__APPLE__)
  if (bytes.empty()) {
    return;
  }

  int master_fd = -1;
  {
    std::scoped_lock lock(mutex_);
    master_fd = master_fd_;
  }
#ifdef MICROIDE_TESTING
  if (master_fd < 0) {
    std::scoped_lock lock(mutex_);
    test_sent_bytes_.append(bytes);
    return;
  }
#endif
  if (master_fd < 0) {
    return;
  }

  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written =
        write(master_fd, bytes.data() + static_cast<std::ptrdiff_t>(offset), bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
#else
  (void)bytes;
#endif
}

void TerminalSession::SendKey(Key key) {
  std::string bytes;
  {
    std::scoped_lock lock(mutex_);
    bytes = FormatKeyBytesLocked(key);
  }
  SendBytes(bytes);
}

void TerminalSession::PasteText(std::string_view text) {
  if (text.empty()) {
    return;
  }

  std::string bytes;
  {
    std::scoped_lock lock(mutex_);
    bytes = FormatPasteBytesLocked(text);
  }
  SendBytes(bytes);
}

bool TerminalSession::running() const {
  std::scoped_lock lock(mutex_);
  return running_;
}

std::size_t TerminalSession::LineCount() const {
  std::scoped_lock lock(mutex_);
  return lines_.size();
}

std::vector<TerminalLine> TerminalSession::SnapshotLines() const {
  std::scoped_lock lock(mutex_);
  return lines_;
}

std::vector<TerminalLine> TerminalSession::SnapshotLineRange(std::size_t start_row,
                                                             std::size_t max_lines) const {
  std::scoped_lock lock(mutex_);
  if (start_row >= lines_.size() || max_lines == 0) {
    return {};
  }

  const std::size_t end_row = std::min(lines_.size(), start_row + max_lines);
  return std::vector<TerminalLine>(lines_.begin() + static_cast<std::ptrdiff_t>(start_row),
                                   lines_.begin() + static_cast<std::ptrdiff_t>(end_row));
}

std::string TerminalSession::LaunchLabel() const {
  std::scoped_lock lock(mutex_);
  return launch_label_;
}

std::size_t TerminalSession::rows() const {
  std::scoped_lock lock(mutex_);
  return rows_;
}

std::size_t TerminalSession::columns() const {
  std::scoped_lock lock(mutex_);
  return columns_;
}

std::size_t TerminalSession::cursor_row() const {
  std::scoped_lock lock(mutex_);
  return cursor_row_;
}

std::size_t TerminalSession::cursor_column() const {
  std::scoped_lock lock(mutex_);
  return cursor_column_;
}

bool TerminalSession::cursor_visible() const {
  std::scoped_lock lock(mutex_);
  return cursor_visible_;
}

bool TerminalSession::using_alternate_screen() const {
  std::scoped_lock lock(mutex_);
  return use_alternate_screen_;
}

bool TerminalSession::ConsumeWakeEvent() {
  std::scoped_lock lock(mutex_);
  const bool pending = wake_event_pending_;
  wake_event_pending_ = false;
  return pending;
}

bool TerminalSession::WantsMouseCapture() const {
  std::scoped_lock lock(mutex_);
  return CurrentMouseTrackingModeLocked() != MouseTrackingMode::Disabled;
}

bool TerminalSession::WantsMouseMotionCapture(bool buttons_down) const {
  std::scoped_lock lock(mutex_);
  switch (CurrentMouseTrackingModeLocked()) {
    case MouseTrackingMode::Any:
      return true;
    case MouseTrackingMode::Drag:
      return buttons_down;
    case MouseTrackingMode::Normal:
    case MouseTrackingMode::Disabled:
    default:
      return false;
  }
}

bool TerminalSession::WantsFocusEvents() const {
  std::scoped_lock lock(mutex_);
  return focus_event_mode_;
}

std::optional<std::string> TerminalSession::ConsumePendingClipboardText() {
  std::scoped_lock lock(mutex_);
  std::optional<std::string> pending = std::move(pending_clipboard_text_);
  pending_clipboard_text_.reset();
  return pending;
}

void TerminalSession::SendFocusEvent(bool focused) {
  std::scoped_lock lock(mutex_);
  if (!focus_event_mode_) {
    return;
  }
  SendBytesLocked(focused ? "\x1b[I" : "\x1b[O");
}

bool TerminalSession::SendMouseButton(MouseButton button,
                                      bool pressed,
                                      std::size_t row,
                                      std::size_t column,
                                      SDL_Keymod modifiers) {
  std::string bytes;
  {
    std::scoped_lock lock(mutex_);
    if (!EncodeMouseEventLocked(button, pressed, false, row, column, modifiers, bytes)) {
      return false;
    }
  }
  SendBytes(bytes);
  return true;
}

bool TerminalSession::SendMouseMotion(MouseButton button,
                                      std::size_t row,
                                      std::size_t column,
                                      SDL_Keymod modifiers) {
  std::string bytes;
  {
    std::scoped_lock lock(mutex_);
    if (!EncodeMouseEventLocked(button, true, true, row, column, modifiers, bytes)) {
      return false;
    }
  }
  SendBytes(bytes);
  return true;
}

void TerminalSession::ReaderMain(int master_fd, int child_pid) {
#if defined(__unix__) || defined(__APPLE__)
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t count = read(master_fd, buffer.data(), buffer.size());
    if (count > 0) {
      {
        std::scoped_lock lock(mutex_);
        AppendOutputLocked(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
      }
      PushWakeEvent();
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    break;
  }

  int status = 0;
  while (waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {
  }

  bool emit_exit_marker = false;
  {
    std::scoped_lock lock(mutex_);
    emit_exit_marker = !stop_requested_;
    running_ = false;
    child_pid_ = -1;
      if (emit_exit_marker) {
        if (lines_.empty()) {
          lines_.push_back(TerminalLine{});
        }
        if (!lines_.back().cells.empty()) {
          lines_.push_back(TerminalLine{});
        }
        cursor_row_ = lines_.size() - 1;
        cursor_column_ = lines_.back().cells.size();
        AppendOutputLocked("[process exited]");
        lines_.push_back(TerminalLine{});
        TrimScrollbackLocked();
      }
    }
  PushWakeEvent();
#else
  (void)master_fd;
  (void)child_pid;
#endif
}

void TerminalSession::AppendOutputLocked(std::string_view data) {
  if (lines_.empty()) {
    lines_.push_back(TerminalLine{});
  }

  for (const unsigned char byte : data) {
    if (escape_mode_ == EscapeMode::AfterEscape) {
      if (byte == '[') {
        escape_sequence_buffer_.assign(1, '[');
        escape_mode_ = EscapeMode::Csi;
        continue;
      }
      if (byte == ']') {
        escape_sequence_buffer_.assign(1, ']');
        escape_mode_ = EscapeMode::Osc;
        osc_escape_pending_ = false;
        continue;
      }
      if (byte == '(' || byte == ')' || byte == '*' || byte == '+' || byte == '-' ||
          byte == '.' || byte == '/') {
        escape_sequence_buffer_.assign(1, static_cast<char>(byte));
        escape_mode_ = EscapeMode::CharsetDesignate;
        continue;
      }
      if (byte == '7') {
        SaveCursorLocked();
      } else if (byte == '8') {
        RestoreCursorLocked();
      } else if (byte == 'Z') {
        SendBytesLocked("\x1b[?1;2c");
      } else if (byte == 'D' || byte == 'E') {
        AdvanceCursorRowLocked();
        if (byte == 'E') {
          cursor_column_ = 0;
        }
      } else if (byte == 'M') {
        if (use_alternate_screen_) {
          const std::size_t scroll_region_top = ActiveScrollRegionTopLocked();
          const std::size_t scroll_region_bottom = ActiveScrollRegionBottomLocked();
          if (cursor_row_ == scroll_region_top) {
            ScrollRegionDownLocked(scroll_region_top, scroll_region_bottom, 1);
          } else if (cursor_row_ > 0) {
            --cursor_row_;
          }
        } else if (cursor_row_ > 0) {
          --cursor_row_;
        }
      }
      escape_sequence_buffer_.clear();
      escape_mode_ = EscapeMode::None;
      continue;
    }

    if (escape_mode_ == EscapeMode::CharsetDesignate) {
      escape_sequence_buffer_.clear();
      escape_mode_ = EscapeMode::None;
      continue;
    }

    if (escape_mode_ == EscapeMode::Csi) {
      escape_sequence_buffer_.push_back(static_cast<char>(byte));
      if (byte >= '@' && byte <= '~') {
        HandleEscapeSequenceLocked(escape_sequence_buffer_);
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::None;
      }
      continue;
    }

    if (escape_mode_ == EscapeMode::Osc) {
      if (osc_escape_pending_) {
        if (byte == '\\') {
          HandleOscSequenceLocked(escape_sequence_buffer_);
          escape_sequence_buffer_.clear();
          escape_mode_ = EscapeMode::None;
          osc_escape_pending_ = false;
          continue;
        }
        osc_escape_pending_ = false;
      }
      if (byte == '\a') {
        HandleOscSequenceLocked(escape_sequence_buffer_);
        escape_sequence_buffer_.clear();
        escape_mode_ = EscapeMode::None;
        continue;
      }
      if (byte == '\x1b') {
        osc_escape_pending_ = true;
        continue;
      }
      escape_sequence_buffer_.push_back(static_cast<char>(byte));
      continue;
    }

    if (byte == '\x1b') {
      if (!pending_utf8_sequence_.empty()) {
        PutGlyphLocked("\xEF\xBF\xBD");
        pending_utf8_sequence_.clear();
      }
      escape_mode_ = EscapeMode::AfterEscape;
      escape_sequence_buffer_.clear();
      osc_escape_pending_ = false;
      continue;
    }

    if (!pending_utf8_sequence_.empty() && byte < 0x80) {
      PutGlyphLocked("\xEF\xBF\xBD");
      pending_utf8_sequence_.clear();
    }

    switch (byte) {
      case '\r':
        cursor_column_ = 0;
        break;
      case '\n':
        AdvanceCursorRowLocked();
        break;
      case '\b':
        if (cursor_column_ > 0) {
          --cursor_column_;
        }
        break;
      case 0x7f: {
        if (cursor_column_ > 0) {
          --cursor_column_;
        }
        EnsureCursorLineExistsLocked();
        auto& line = lines_[cursor_row_];
        if (cursor_column_ < line.cells.size()) {
          line.cells.erase(line.cells.begin() +
                           static_cast<std::ptrdiff_t>(cursor_column_));
        }
        break;
      }
      case '\t': {
        constexpr std::size_t kTerminalTabStop = 8;
        const std::size_t remainder = cursor_column_ % kTerminalTabStop;
        const std::size_t spaces =
            remainder == 0 ? kTerminalTabStop : kTerminalTabStop - remainder;
        for (std::size_t i = 0; i < spaces; ++i) {
          PutCharacterLocked(' ');
        }
        break;
      }
      default:
        if (byte >= 32 || byte >= 0x80) {
          if (byte < 0x80) {
            PutCharacterLocked(static_cast<char>(byte));
            break;
          }

          while (true) {
            if (pending_utf8_sequence_.empty()) {
              const std::size_t sequence_length = util::Utf8SequenceLength(byte);
              if (sequence_length == 0) {
                PutGlyphLocked("\xEF\xBF\xBD");
                break;
              }
              pending_utf8_sequence_.assign(1, static_cast<char>(byte));
              if (sequence_length == 1) {
                PutGlyphLocked(pending_utf8_sequence_);
                pending_utf8_sequence_.clear();
              }
              break;
            }

            if (!util::IsUtf8ContinuationByte(byte)) {
              PutGlyphLocked("\xEF\xBF\xBD");
              pending_utf8_sequence_.clear();
              continue;
            }

            pending_utf8_sequence_.push_back(static_cast<char>(byte));
            const std::size_t sequence_length =
                util::Utf8SequenceLength(
                    static_cast<unsigned char>(pending_utf8_sequence_.front()));
            if (sequence_length != 0 && pending_utf8_sequence_.size() >= sequence_length) {
              PutGlyphLocked(pending_utf8_sequence_);
              pending_utf8_sequence_.clear();
            }
            break;
          }
        }
        break;
    }
  }

  TrimScrollbackLocked();
}

void TerminalSession::HandleEscapeSequenceLocked(std::string_view sequence) {
  if (sequence.empty() || sequence.front() != '[') {
    return;
  }

  const char final = sequence.back();
  std::string_view body = sequence.substr(1, sequence.size() - 2);
  char prefix = '\0';
  if (!body.empty() && !std::isdigit(static_cast<unsigned char>(body.front())) &&
      body.front() != ';') {
    prefix = body.front();
    body.remove_prefix(1);
  }
  std::vector<int> params = ParseCsiParameters(body);

  if (prefix == '?') {
    if (final == 'h' || final == 'l') {
      const bool enabled = final == 'h';
      for (const int mode : params) {
        HandlePrivateModeLocked(mode, enabled);
      }
    } else if (final == 'n') {
      for (const int mode : params) {
        if (mode == 6) {
          SendBytesLocked("\x1b[?" + std::to_string(cursor_row_ + 1) + ";" +
                          std::to_string(cursor_column_ + 1) + "R");
        }
      }
    }
    return;
  }

  if (prefix == '>') {
    if (final == 'c') {
      SendBytesLocked("\x1b[>0;10;1c");
    }
    return;
  }

  switch (final) {
    case 'c':
      SendBytesLocked("\x1b[?1;2c");
      return;
    case 'm': {
      if (params.empty()) {
        params.push_back(0);
      }
      for (std::size_t i = 0; i < params.size(); ++i) {
        const int code = params[i];
        if (code == 0) {
          current_style_ = TerminalStyle{};
          continue;
        }
        if (code == 1) {
          current_style_.bold = true;
          continue;
        }
        if (code == 22) {
          current_style_.bold = false;
          continue;
        }
        if (code == 7) {
          current_style_.inverse = true;
          continue;
        }
        if (code == 27) {
          current_style_.inverse = false;
          continue;
        }
        if (code == 39) {
          current_style_.foreground.reset();
          continue;
        }
        if (code == 49) {
          current_style_.background.reset();
          continue;
        }
        if (code >= 30 && code <= 37) {
          current_style_.foreground = BasicAnsiColor(code - 30, current_style_.bold);
          continue;
        }
        if (code >= 40 && code <= 47) {
          current_style_.background = BasicAnsiColor(code - 40, false);
          continue;
        }
        if (code >= 90 && code <= 97) {
          current_style_.foreground = BasicAnsiColor(code - 90, true);
          continue;
        }
        if (code >= 100 && code <= 107) {
          current_style_.background = BasicAnsiColor(code - 100, true);
          continue;
        }
        if ((code == 38 || code == 48) && i + 1 < params.size()) {
          SDL_Color color{};
          bool parsed = false;
          if (params[i + 1] == 5 && i + 2 < params.size()) {
            color = Ansi256Color(params[i + 2]);
            i += 2;
            parsed = true;
          } else if (params[i + 1] == 2 && i + 4 < params.size()) {
            color = MakeColor(static_cast<Uint8>(std::clamp(params[i + 2], 0, 255)),
                              static_cast<Uint8>(std::clamp(params[i + 3], 0, 255)),
                              static_cast<Uint8>(std::clamp(params[i + 4], 0, 255)));
            i += 4;
            parsed = true;
          }
          if (!parsed) {
            continue;
          }
          if (code == 38) {
            current_style_.foreground = color;
          } else {
            current_style_.background = color;
          }
        }
      }
      return;
    }
    case 'A':
      cursor_row_ = cursor_row_ > static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1))
                        ? cursor_row_ - static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1))
                        : 0;
      return;
    case 'B':
      MoveCursorLocked(cursor_row_ + static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)),
                       cursor_column_);
      return;
    case 'C': {
      const std::size_t delta = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      const std::size_t next_column = cursor_column_ + delta;
      cursor_column_ =
          columns_ > 0 ? std::min(next_column, columns_ - 1) : next_column;
      return;
    }
    case 'D':
      cursor_column_ = cursor_column_ > static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1))
                           ? cursor_column_ -
                                 static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1))
                           : 0;
      return;
    case 'E':
      MoveCursorLocked(cursor_row_ + static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)),
                       0);
      return;
    case 'F':
      MoveCursorLocked(cursor_row_ > static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1))
                           ? cursor_row_ -
                                 static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1))
                           : 0,
                       0);
      return;
    case 'G':
      MoveCursorLocked(cursor_row_,
                       static_cast<std::size_t>(std::max(0, CsiParamOrDefault(params, 0, 1) - 1)));
      return;
    case 'H':
    case 'f':
      MoveCursorLocked(
          static_cast<std::size_t>(std::max(0, CsiParamOrDefault(params, 0, 1) - 1)) +
              ((use_alternate_screen_ && origin_mode_) ? ActiveScrollRegionTopLocked() : 0),
          static_cast<std::size_t>(std::max(0, CsiParamOrDefault(params, 1, 1) - 1)));
      return;
    case 'J':
      EraseInDisplayLocked(params.empty() ? 0 : params.front());
      return;
    case 'K':
      EraseInLineLocked(params.empty() ? 0 : params.front());
      return;
    case 'L': {
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      EnsureCursorLineExistsLocked();
      if (use_alternate_screen_) {
        const std::size_t scroll_region_top = ActiveScrollRegionTopLocked();
        const std::size_t scroll_region_bottom = ActiveScrollRegionBottomLocked();
        if (cursor_row_ >= scroll_region_top && cursor_row_ <= scroll_region_bottom) {
          ScrollRegionDownLocked(cursor_row_, scroll_region_bottom, count);
          return;
        }
      }
      lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(cursor_row_), count,
                    TerminalLine{});
      TrimScrollbackLocked();
      return;
    }
    case 'M': {
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      EnsureCursorLineExistsLocked();
      if (use_alternate_screen_) {
        const std::size_t scroll_region_top = ActiveScrollRegionTopLocked();
        const std::size_t scroll_region_bottom = ActiveScrollRegionBottomLocked();
        if (cursor_row_ >= scroll_region_top && cursor_row_ <= scroll_region_bottom) {
          ScrollRegionUpLocked(cursor_row_, scroll_region_bottom, count);
          return;
        }
      }
      const std::size_t erase_end = std::min(lines_.size(), cursor_row_ + count);
      lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(cursor_row_),
                   lines_.begin() + static_cast<std::ptrdiff_t>(erase_end));
      EnsureCursorLineExistsLocked();
      return;
    }
    case 'P': {
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      EnsureCursorLineExistsLocked();
      auto& line = lines_[cursor_row_];
      if (cursor_column_ < line.cells.size()) {
        const std::size_t erase_end = std::min(line.cells.size(), cursor_column_ + count);
        line.cells.erase(line.cells.begin() + static_cast<std::ptrdiff_t>(cursor_column_),
                         line.cells.begin() + static_cast<std::ptrdiff_t>(erase_end));
      }
      return;
    }
    case 'X': {
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      EnsureCursorLineExistsLocked();
      ClearLineRangeLocked(lines_[cursor_row_], cursor_column_, cursor_column_ + count);
      return;
    }
    case '@': {
      const std::size_t count = static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1));
      EnsureCursorLineExistsLocked();
      auto& line = lines_[cursor_row_];
      ResizeLineLocked(line, cursor_column_);
      line.cells.insert(line.cells.begin() + static_cast<std::ptrdiff_t>(cursor_column_), count,
                        MakeAsciiTerminalCell(' ', current_style_));
      if (columns_ > 0 && line.cells.size() > columns_) {
        line.cells.resize(columns_);
      }
      return;
    }
    case 'S':
      if (use_alternate_screen_) {
        ScrollRegionUpLocked(ActiveScrollRegionTopLocked(), ActiveScrollRegionBottomLocked(),
                             static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)));
      }
      return;
    case 'T':
      if (use_alternate_screen_) {
        ScrollRegionDownLocked(ActiveScrollRegionTopLocked(), ActiveScrollRegionBottomLocked(),
                               static_cast<std::size_t>(CsiParamOrDefault(params, 0, 1)));
      }
      return;
    case 'n':
      if (!params.empty()) {
        if (params.front() == 5) {
          SendBytesLocked("\x1b[0n");
        } else if (params.front() == 6) {
          SendBytesLocked("\x1b[" + std::to_string(cursor_row_ + 1) + ";" +
                          std::to_string(cursor_column_ + 1) + "R");
        }
      }
      return;
    case 'd':
      MoveCursorLocked(
          static_cast<std::size_t>(std::max(0, CsiParamOrDefault(params, 0, 1) - 1)) +
              ((use_alternate_screen_ && origin_mode_) ? ActiveScrollRegionTopLocked() : 0),
          cursor_column_);
      return;
    case 'r': {
      const std::size_t terminal_rows = std::max<std::size_t>(1, rows_);
      const int terminal_rows_int = static_cast<int>(terminal_rows);
      const int scroll_region_top_param = CsiParamOrDefault(params, 0, 1);
      const int scroll_region_bottom_param =
          params.size() > 1 ? CsiParamOrDefault(params, 1, terminal_rows_int) : terminal_rows_int;
      const std::size_t scroll_region_top =
          static_cast<std::size_t>(std::max(0, scroll_region_top_param - 1));
      const std::size_t scroll_region_bottom =
          static_cast<std::size_t>(std::max(0, scroll_region_bottom_param - 1));
      if (scroll_region_top >= terminal_rows || scroll_region_bottom >= terminal_rows ||
          scroll_region_top > scroll_region_bottom) {
        return;
      }
      scroll_region_top_ = scroll_region_top;
      scroll_region_bottom_ = scroll_region_bottom;
      MoveCursorLocked(0, 0);
      return;
    }
    case 's':
      saved_cursor_row_ = cursor_row_;
      saved_cursor_column_ = cursor_column_;
      return;
    case 'u':
      MoveCursorLocked(saved_cursor_row_, saved_cursor_column_);
      return;
    default:
      return;
  }
}

void TerminalSession::HandleOscSequenceLocked(std::string_view sequence) {
  if (sequence.empty() || sequence.front() != ']') {
    return;
  }

  const std::string_view body = sequence.substr(1);
  const std::size_t separator = body.find(';');
  if (separator == std::string_view::npos) {
    return;
  }

  const std::string_view command = body.substr(0, separator);
  if (command == "52") {
    const std::string_view remainder = body.substr(separator + 1);
    const std::size_t second_separator = remainder.find(';');
    if (second_separator == std::string_view::npos) {
      return;
    }

    const std::string_view selection = remainder.substr(0, second_separator);
    const std::string_view encoded = remainder.substr(second_separator + 1);
    if ((!selection.empty() && selection != "c") || encoded == "?") {
      return;
    }

    const auto decoded = DecodeBase64(encoded);
    if (!decoded.has_value() || !ClipboardPayloadIsText(*decoded)) {
      return;
    }
    pending_clipboard_text_ = *decoded;
    return;
  }

  if (command != "0" && command != "1" && command != "2") {
    return;
  }

  const std::string title = SanitizeOscTitle(body.substr(separator + 1));
  launch_label_ = title.empty() ? default_launch_label_ : title;
}

void TerminalSession::HandlePrivateModeLocked(int mode, bool enabled) {
  switch (mode) {
    case 1:
      application_cursor_keys_mode_ = enabled;
      return;
    case 6:
      origin_mode_ = enabled;
      if (enabled) {
        MoveCursorLocked(ActiveScrollRegionTopLocked(), 0);
      } else {
        MoveCursorLocked(0, 0);
      }
      return;
    case 7:
      auto_wrap_mode_ = enabled;
      return;
    case 1000:
      mouse_tracking_normal_ = enabled;
      return;
    case 1002:
      mouse_tracking_drag_ = enabled;
      return;
    case 1003:
      mouse_tracking_any_ = enabled;
      return;
    case 1006:
      mouse_sgr_ext_mode_ = enabled;
      return;
    case 2004:
      bracketed_paste_mode_ = enabled;
      return;
    case 1004:
      focus_event_mode_ = enabled;
      return;
    case 25:
      cursor_visible_ = enabled;
      return;
    case 47:
    case 1047:
      SetAlternateScreenLocked(enabled, false);
      return;
    case 1048:
      if (enabled) {
        SaveCursorLocked();
      } else {
        RestoreCursorLocked();
      }
      return;
    case 1049:
      if (enabled) {
        SaveCursorLocked();
      }
      SetAlternateScreenLocked(enabled, true);
      if (!enabled) {
        RestoreCursorLocked();
      }
      return;
    default:
      return;
  }
}

TerminalSession::MouseTrackingMode TerminalSession::CurrentMouseTrackingModeLocked() const {
  if (mouse_tracking_any_) {
    return MouseTrackingMode::Any;
  }
  if (mouse_tracking_drag_) {
    return MouseTrackingMode::Drag;
  }
  if (mouse_tracking_normal_) {
    return MouseTrackingMode::Normal;
  }
  return MouseTrackingMode::Disabled;
}

void TerminalSession::SendBytesLocked(std::string_view bytes) {
  if (bytes.empty()) {
    return;
  }

#ifdef MICROIDE_TESTING
  if (master_fd_ < 0) {
    test_sent_bytes_.append(bytes);
    return;
  }
#endif

#if defined(__unix__) || defined(__APPLE__)
  if (master_fd_ < 0) {
    return;
  }

  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written =
        write(master_fd_, bytes.data() + static_cast<std::ptrdiff_t>(offset), bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    offset += static_cast<std::size_t>(written);
  }
#else
  (void)bytes;
#endif
}

std::string TerminalSession::FormatKeyBytesLocked(Key key) const {
  switch (key) {
    case Key::Escape:
      return "\x1b";
    case Key::Enter:
      return "\r";
    case Key::Backspace:
      return "\x7f";
    case Key::Tab:
      return "\t";
    case Key::Up:
      return application_cursor_keys_mode_ ? "\x1bOA" : "\x1b[A";
    case Key::Down:
      return application_cursor_keys_mode_ ? "\x1bOB" : "\x1b[B";
    case Key::Right:
      return application_cursor_keys_mode_ ? "\x1bOC" : "\x1b[C";
    case Key::Left:
      return application_cursor_keys_mode_ ? "\x1bOD" : "\x1b[D";
    case Key::Home:
      return application_cursor_keys_mode_ ? "\x1bOH" : "\x1b[H";
    case Key::End:
      return application_cursor_keys_mode_ ? "\x1bOF" : "\x1b[F";
    case Key::PageUp:
      return "\x1b[5~";
    case Key::PageDown:
      return "\x1b[6~";
    case Key::Insert:
      return "\x1b[2~";
    case Key::Delete:
      return "\x1b[3~";
    default:
      return {};
  }
}

std::string TerminalSession::FormatPasteBytesLocked(std::string_view text) const {
  if (!bracketed_paste_mode_) {
    return std::string(text);
  }

  std::string bytes;
  bytes.reserve(text.size() + 12);
  bytes.append("\x1b[200~");
  bytes.append(text);
  bytes.append("\x1b[201~");
  return bytes;
}

bool TerminalSession::EncodeMouseEventLocked(MouseButton button,
                                             bool pressed,
                                             bool motion,
                                             std::size_t row,
                                             std::size_t column,
                                             SDL_Keymod modifiers,
                                             std::string& out_bytes) const {
  out_bytes.clear();
  if (master_fd_ < 0) {
    return false;
  }

  const MouseTrackingMode tracking_mode = CurrentMouseTrackingModeLocked();
  if (tracking_mode == MouseTrackingMode::Disabled) {
    return false;
  }
  if (motion) {
    if (tracking_mode == MouseTrackingMode::Normal) {
      return false;
    }
    if (tracking_mode == MouseTrackingMode::Drag && button == MouseButton::None) {
      return false;
    }
  }

  const std::size_t clamped_row =
      rows_ > 0 ? std::min(row, rows_ - 1) : row;
  const std::size_t clamped_column =
      columns_ > 0 ? std::min(column, columns_ - 1) : column;

  int code = 0;
  switch (button) {
    case MouseButton::Left:
      code = 0;
      break;
    case MouseButton::Middle:
      code = 1;
      break;
    case MouseButton::Right:
      code = 2;
      break;
    case MouseButton::WheelUp:
      code = 64;
      break;
    case MouseButton::WheelDown:
      code = 65;
      break;
    case MouseButton::None:
    default:
      code = 3;
      break;
  }

  if (!pressed && !motion && button != MouseButton::WheelUp && button != MouseButton::WheelDown) {
    code = 3;
  }
  if (motion) {
    code |= 32;
  }
  code |= MouseModifierBits(modifiers);

  if (mouse_sgr_ext_mode_) {
    out_bytes = "\x1b[<" + std::to_string(code) + ";" + std::to_string(clamped_column + 1) + ";" +
                std::to_string(clamped_row + 1) +
                ((pressed || motion || button == MouseButton::WheelUp ||
                  button == MouseButton::WheelDown)
                     ? "M"
                     : "m");
    return true;
  }

  const int encoded_button = std::clamp(32 + code, 0, 255);
  const int encoded_column =
      std::clamp(33 + static_cast<int>(std::min<std::size_t>(clamped_column, 222)), 0, 255);
  const int encoded_row =
      std::clamp(33 + static_cast<int>(std::min<std::size_t>(clamped_row, 222)), 0, 255);
  out_bytes.push_back('\x1b');
  out_bytes.push_back('[');
  out_bytes.push_back('M');
  out_bytes.push_back(static_cast<char>(encoded_button));
  out_bytes.push_back(static_cast<char>(encoded_column));
  out_bytes.push_back(static_cast<char>(encoded_row));
  return true;
}

void TerminalSession::EnsureCursorLineExistsLocked() {
  if (lines_.size() <= cursor_row_) {
    lines_.resize(cursor_row_ + 1);
  }
}

void TerminalSession::SaveCursorLocked() {
  saved_cursor_row_ = cursor_row_;
  saved_cursor_column_ = cursor_column_;
}

void TerminalSession::RestoreCursorLocked() {
  MoveCursorLocked(saved_cursor_row_, saved_cursor_column_);
}

void TerminalSession::SaveActiveScreenLocked() {
  ScreenState& screen = use_alternate_screen_ ? alternate_screen_ : primary_screen_;
  screen.lines = lines_;
  screen.cursor_row = cursor_row_;
  screen.cursor_column = cursor_column_;
  screen.saved_cursor_row = saved_cursor_row_;
  screen.saved_cursor_column = saved_cursor_column_;
  screen.scroll_region_top = scroll_region_top_;
  screen.scroll_region_bottom = scroll_region_bottom_;
}

void TerminalSession::RestoreSavedScreenLocked() {
  const ScreenState& screen = use_alternate_screen_ ? alternate_screen_ : primary_screen_;
  lines_ = screen.lines.empty() ? std::vector<TerminalLine>{TerminalLine{}} : screen.lines;
  pending_utf8_sequence_.clear();
  if (use_alternate_screen_) {
    lines_.resize(std::max<std::size_t>(1, rows_));
  }
  cursor_row_ = screen.cursor_row;
  cursor_column_ = screen.cursor_column;
  saved_cursor_row_ = screen.saved_cursor_row;
  saved_cursor_column_ = screen.saved_cursor_column;
  scroll_region_top_ = screen.scroll_region_top;
  scroll_region_bottom_ = screen.scroll_region_bottom;
  if (use_alternate_screen_ && rows_ > 0) {
    cursor_row_ = std::min(cursor_row_, rows_ - 1);
    saved_cursor_row_ = std::min(saved_cursor_row_, rows_ - 1);
  }
  ClampScrollRegionLocked();
  EnsureCursorLineExistsLocked();
}

void TerminalSession::ResetScreenLocked(bool fill_rows) {
  lines_.assign(fill_rows ? std::max<std::size_t>(1, rows_) : 1, TerminalLine{});
  pending_utf8_sequence_.clear();
  cursor_row_ = 0;
  cursor_column_ = 0;
  saved_cursor_row_ = 0;
  saved_cursor_column_ = 0;
  ResetScrollRegionLocked();
}

void TerminalSession::SetAlternateScreenLocked(bool enabled, bool clear) {
  if (enabled) {
    SaveActiveScreenLocked();
    if (!use_alternate_screen_) {
      use_alternate_screen_ = true;
      RestoreSavedScreenLocked();
    }
    if (clear || alternate_screen_.lines.empty()) {
      ResetScreenLocked(true);
      SaveActiveScreenLocked();
    }
    return;
  }

  if (!use_alternate_screen_) {
    return;
  }

  SaveActiveScreenLocked();
  use_alternate_screen_ = false;
  RestoreSavedScreenLocked();
}

void TerminalSession::AdvanceCursorRowLocked(bool wrapped_from_previous) {
  if (use_alternate_screen_) {
    const std::size_t terminal_rows = std::max<std::size_t>(1, rows_);
    if (lines_.size() < terminal_rows) {
      lines_.resize(terminal_rows);
    }
    const std::size_t scroll_region_top = ActiveScrollRegionTopLocked();
    const std::size_t scroll_region_bottom = ActiveScrollRegionBottomLocked();
    if (cursor_row_ >= scroll_region_top && cursor_row_ <= scroll_region_bottom) {
      if (cursor_row_ == scroll_region_bottom) {
        ScrollRegionUpLocked(scroll_region_top, scroll_region_bottom, 1);
        cursor_column_ = 0;
        EnsureCursorLineExistsLocked();
        lines_[cursor_row_].wrapped_from_previous = wrapped_from_previous;
        return;
      }
      ++cursor_row_;
      cursor_column_ = 0;
      EnsureCursorLineExistsLocked();
      lines_[cursor_row_].wrapped_from_previous = wrapped_from_previous;
      return;
    }
    if (cursor_row_ + 1 >= terminal_rows) {
      ScrollRegionUpLocked(0, terminal_rows - 1, 1);
      cursor_row_ = terminal_rows - 1;
      cursor_column_ = 0;
      EnsureCursorLineExistsLocked();
      lines_[cursor_row_].wrapped_from_previous = wrapped_from_previous;
      return;
    }
  }

  ++cursor_row_;
  cursor_column_ = 0;
  EnsureCursorLineExistsLocked();
  lines_[cursor_row_].wrapped_from_previous = wrapped_from_previous;
}

void TerminalSession::MoveCursorLocked(std::size_t row, std::size_t column) {
  if (use_alternate_screen_) {
    const std::size_t max_row = origin_mode_ ? ActiveScrollRegionBottomLocked()
                                             : std::max<std::size_t>(1, rows_) - 1;
    const std::size_t min_row = origin_mode_ ? ActiveScrollRegionTopLocked() : 0;
    cursor_row_ = std::clamp(row, min_row, max_row);
  } else {
    cursor_row_ = row;
  }
  cursor_column_ =
      columns_ > 0 ? std::min(column, columns_ - 1) : column;
  EnsureCursorLineExistsLocked();
}

void TerminalSession::PutCharacterLocked(char character) {
  PutGlyphLocked(std::string_view(&character, 1));
}

void TerminalSession::PutGlyphLocked(std::string_view glyph) {
  if (columns_ > 0 && cursor_column_ >= columns_) {
    if (auto_wrap_mode_) {
      AdvanceCursorRowLocked(true);
    } else {
      cursor_column_ = columns_ - 1;
    }
  }

  EnsureCursorLineExistsLocked();
  auto& line = lines_[cursor_row_];
  ResizeLineLocked(line, cursor_column_);
  if (cursor_column_ == line.cells.size()) {
    line.cells.push_back(MakeUtf8TerminalCell(glyph, current_style_));
  } else {
    line.cells[cursor_column_] = MakeUtf8TerminalCell(glyph, current_style_);
  }
  ++cursor_column_;
}

void TerminalSession::ResizeLineLocked(TerminalLine& line, std::size_t size) {
  if (line.cells.size() < size) {
    line.cells.resize(size, MakeAsciiTerminalCell(' ', TerminalStyle{}));
  }
}

void TerminalSession::ClearLineRangeLocked(TerminalLine& line, std::size_t start, std::size_t end) {
  if (start >= end) {
    return;
  }

  ResizeLineLocked(line, end);
  for (std::size_t i = start; i < end; ++i) {
    line.cells[i] = MakeAsciiTerminalCell(' ', current_style_);
  }
}

void TerminalSession::EraseInLineLocked(int mode) {
  EnsureCursorLineExistsLocked();
  auto& line = lines_[cursor_row_];
  switch (mode) {
    case 1:
      ClearLineRangeLocked(line, 0, cursor_column_ + 1);
      break;
    case 2:
      ClearLineRangeLocked(line, 0, std::max<std::size_t>(line.cells.size(), cursor_column_ + 1));
      break;
    case 0:
    default:
      if (cursor_column_ < line.cells.size()) {
        ClearLineRangeLocked(line, cursor_column_, line.cells.size());
      }
      break;
  }
}

void TerminalSession::EraseInDisplayLocked(int mode) {
  EnsureCursorLineExistsLocked();
  switch (mode) {
    case 1:
      for (std::size_t row = 0; row < cursor_row_; ++row) {
        lines_[row].cells.clear();
      }
      EraseInLineLocked(1);
      break;
    case 2:
      lines_.assign(use_alternate_screen_ ? std::max<std::size_t>(1, rows_)
                                          : std::max<std::size_t>(1, cursor_row_ + 1),
                    TerminalLine{});
      break;
    case 0:
    default:
      EraseInLineLocked(0);
      for (std::size_t row = cursor_row_ + 1; row < lines_.size(); ++row) {
        lines_[row].cells.clear();
      }
      break;
  }
}

void TerminalSession::ResetScrollRegionLocked() {
  scroll_region_top_ = 0;
  scroll_region_bottom_ = rows_ > 0 ? rows_ - 1 : 0;
}

void TerminalSession::ClampScrollRegionLocked() {
  if (rows_ == 0) {
    scroll_region_top_ = 0;
    scroll_region_bottom_ = 0;
    return;
  }
  scroll_region_top_ = std::min(scroll_region_top_, rows_ - 1);
  scroll_region_bottom_ = std::min(scroll_region_bottom_, rows_ - 1);
  if (scroll_region_bottom_ < scroll_region_top_) {
    ResetScrollRegionLocked();
  }
}

std::size_t TerminalSession::ActiveScrollRegionTopLocked() const {
  if (rows_ == 0) {
    return 0;
  }
  return std::min(scroll_region_top_, rows_ - 1);
}

std::size_t TerminalSession::ActiveScrollRegionBottomLocked() const {
  if (rows_ == 0) {
    return 0;
  }
  return std::clamp(scroll_region_bottom_, ActiveScrollRegionTopLocked(), rows_ - 1);
}

void TerminalSession::ScrollRegionUpLocked(std::size_t top, std::size_t bottom, std::size_t count) {
  if (!use_alternate_screen_ || rows_ == 0) {
    return;
  }

  const std::size_t terminal_rows = std::max<std::size_t>(1, rows_);
  if (lines_.size() < terminal_rows) {
    lines_.resize(terminal_rows);
  }

  const std::size_t clamped_top = std::min(top, terminal_rows - 1);
  const std::size_t clamped_bottom = std::clamp(bottom, clamped_top, terminal_rows - 1);
  const std::size_t region_size = clamped_bottom - clamped_top + 1;
  const std::size_t shift = std::min(count, region_size);
  if (shift == 0) {
    return;
  }

  auto begin = lines_.begin() + static_cast<std::ptrdiff_t>(clamped_top);
  auto end = lines_.begin() + static_cast<std::ptrdiff_t>(clamped_bottom + 1);
  std::rotate(begin, begin + static_cast<std::ptrdiff_t>(shift), end);
  std::fill(end - static_cast<std::ptrdiff_t>(shift), end, TerminalLine{});
}

void TerminalSession::ScrollRegionDownLocked(std::size_t top,
                                             std::size_t bottom,
                                             std::size_t count) {
  if (!use_alternate_screen_ || rows_ == 0) {
    return;
  }

  const std::size_t terminal_rows = std::max<std::size_t>(1, rows_);
  if (lines_.size() < terminal_rows) {
    lines_.resize(terminal_rows);
  }

  const std::size_t clamped_top = std::min(top, terminal_rows - 1);
  const std::size_t clamped_bottom = std::clamp(bottom, clamped_top, terminal_rows - 1);
  const std::size_t region_size = clamped_bottom - clamped_top + 1;
  const std::size_t shift = std::min(count, region_size);
  if (shift == 0) {
    return;
  }

  auto begin = lines_.begin() + static_cast<std::ptrdiff_t>(clamped_top);
  auto end = lines_.begin() + static_cast<std::ptrdiff_t>(clamped_bottom + 1);
  std::rotate(begin, end - static_cast<std::ptrdiff_t>(shift), end);
  std::fill(begin, begin + static_cast<std::ptrdiff_t>(shift), TerminalLine{});
}

void TerminalSession::TrimScrollbackLocked() {
  const std::size_t max_lines = use_alternate_screen_ ? std::max<std::size_t>(1, rows_)
                                                      : kMaxScrollbackLines +
                                                            std::max<std::size_t>(1, rows_);
  if (lines_.size() <= max_lines) {
    return;
  }

  const std::size_t trim_count = lines_.size() - max_lines;
  lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(trim_count));
  cursor_row_ = cursor_row_ > trim_count ? cursor_row_ - trim_count : 0;
  saved_cursor_row_ = saved_cursor_row_ > trim_count ? saved_cursor_row_ - trim_count : 0;
  if (lines_.empty()) {
    lines_.push_back(TerminalLine{});
  }
}

bool TerminalSession::ReserveWakeEvent(Uint32& event_type) const {
  std::scoped_lock lock(mutex_);
  if (wake_event_type_ == 0 || wake_event_pending_) {
    return false;
  }

  wake_event_pending_ = true;
  event_type = wake_event_type_;
  return true;
}

void TerminalSession::PushWakeEvent() const {
  Uint32 event_type = 0;
  if (!ReserveWakeEvent(event_type)) {
    return;
  }

  SDL_Event event{};
  event.type = event_type;
  if (!SDL_PushEvent(&event)) {
    std::scoped_lock lock(mutex_);
    wake_event_pending_ = false;
  }
}

}  // namespace microide::terminal

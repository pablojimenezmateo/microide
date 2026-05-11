#include "platform/ProcessBackend.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <array>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace microide::platform {

namespace {

#if defined(__unix__) || defined(__APPLE__)

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  ~UniqueFd() { Reset(); }

  int Get() const { return fd_; }
  bool IsValid() const { return fd_ >= 0; }

  int Release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  void Reset(int fd = -1) {
    if (fd_ >= 0) {
      close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

bool OpenPipe(bool enabled, std::array<UniqueFd, 2>* pipe_fds) {
  if (pipe_fds == nullptr) {
    return false;
  }
  (*pipe_fds)[0].Reset();
  (*pipe_fds)[1].Reset();
  if (!enabled) {
    return true;
  }

  int raw_pipe[2] = {-1, -1};
  if (pipe(raw_pipe) != 0) {
    return false;
  }
  (*pipe_fds)[0].Reset(raw_pipe[0]);
  (*pipe_fds)[1].Reset(raw_pipe[1]);
  return true;
}

void DrainReadyPipe(UniqueFd* fd, std::string* output) {
  if (fd == nullptr || !fd->IsValid() || output == nullptr) {
    return;
  }

  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t bytes_read = read(fd->Get(), buffer.data(), buffer.size());
    if (bytes_read > 0) {
      output->append(buffer.data(), static_cast<std::size_t>(bytes_read));
      if (bytes_read == static_cast<ssize_t>(buffer.size())) {
        continue;
      }
      return;
    }
    if (bytes_read == 0) {
      fd->Reset();
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    fd->Reset();
    return;
  }
}

void DrainCapturedPipes(UniqueFd* stdout_fd,
                        std::string* stdout_text,
                        UniqueFd* stderr_fd,
                        std::string* stderr_text) {
  while ((stdout_fd != nullptr && stdout_fd->IsValid()) ||
         (stderr_fd != nullptr && stderr_fd->IsValid())) {
    std::array<pollfd, 2> poll_fds{};
    nfds_t count = 0;
    if (stdout_fd != nullptr && stdout_fd->IsValid()) {
      poll_fds[count++] =
          pollfd{.fd = stdout_fd->Get(), .events = POLLIN | POLLHUP, .revents = 0};
    }
    if (stderr_fd != nullptr && stderr_fd->IsValid()) {
      poll_fds[count++] =
          pollfd{.fd = stderr_fd->Get(), .events = POLLIN | POLLHUP, .revents = 0};
    }

    if (count == 0) {
      break;
    }

    const int poll_result = poll(poll_fds.data(), count, -1);
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    for (nfds_t index = 0; index < count; ++index) {
      if ((poll_fds[index].revents & (POLLIN | POLLHUP)) == 0) {
        continue;
      }
      if (stdout_fd != nullptr && stdout_fd->IsValid() &&
          poll_fds[index].fd == stdout_fd->Get()) {
        DrainReadyPipe(stdout_fd, stdout_text);
        continue;
      }
      if (stderr_fd != nullptr && stderr_fd->IsValid() &&
          poll_fds[index].fd == stderr_fd->Get()) {
        DrainReadyPipe(stderr_fd, stderr_text);
      }
    }
  }
}

void WriteAllToPipe(int fd, const std::string& text) {
  const char* data = text.data();
  std::size_t remaining = text.size();
  while (remaining > 0) {
    const ssize_t written = write(fd, data, remaining);
    if (written > 0) {
      data += written;
      remaining -= static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
}

void ApplyEnvironmentOverrides(const std::vector<SubprocessEnvironmentOverride>& overrides) {
  for (const auto& override_entry : overrides) {
    if (override_entry.name.empty()) {
      continue;
    }

    if (override_entry.value.has_value()) {
      (void)setenv(override_entry.name.c_str(), override_entry.value->c_str(), 1);
      continue;
    }

    (void)unsetenv(override_entry.name.c_str());
  }
}

class PosixAsyncProcessBackend final : public AsyncProcessBackend {
 public:
  bool Start(const std::vector<std::string>& argv, const std::string& cwd) override {
    if (argv.empty() || running_) {
      return false;
    }

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};

    if (pipe(stdin_pipe) != 0) {
      return false;
    }
    if (pipe(stdout_pipe) != 0) {
      close(stdin_pipe[0]);
      close(stdin_pipe[1]);
      return false;
    }

    fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);

    const pid_t pid = fork();
    if (pid < 0) {
      close(stdin_pipe[0]);
      close(stdin_pipe[1]);
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      return false;
    }

    if (pid == 0) {
      dup2(stdin_pipe[0], STDIN_FILENO);
      dup2(stdout_pipe[1], STDOUT_FILENO);

      close(stdin_pipe[0]);
      close(stdin_pipe[1]);
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);

      if (!cwd.empty()) {
        if (chdir(cwd.c_str()) != 0) {
          _exit(127);
        }
      }

      std::vector<char*> raw_argv;
      raw_argv.reserve(argv.size() + 1);
      for (const std::string& arg : argv) {
        raw_argv.push_back(const_cast<char*>(arg.c_str()));
      }
      raw_argv.push_back(nullptr);
      execvp(raw_argv[0], raw_argv.data());
      _exit(errno == ENOENT ? 127 : 126);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    pid_ = pid;
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];
    running_ = true;
    exit_code_.reset();
    return true;
  }

  bool IsRunning() override {
    if (!running_ || pid_ < 0) {
      return false;
    }
    int status = 0;
    const pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result == pid_) {
      running_ = false;
      Close();
      pid_ = -1;
      if (WIFEXITED(status)) {
        exit_code_ = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        exit_code_ = 128 + WTERMSIG(status);
      } else {
        exit_code_ = std::nullopt;
      }
    }
    return running_;
  }

  bool Write(std::string_view data) override {
    if (!running_ || stdin_fd_ < 0) {
      return false;
    }
    const char* ptr = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
      const ssize_t written = write(stdin_fd_, ptr, remaining);
      if (written > 0) {
        ptr += written;
        remaining -= static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && errno == EINTR) {
        continue;
      }
      CloseStdin();
      return false;
    }
    return true;
  }

  std::optional<std::string> Read(std::size_t max_bytes, int timeout_ms) override {
    if (stdout_fd_ < 0) {
      return std::nullopt;
    }

    pollfd pfd{.fd = stdout_fd_, .events = POLLIN | POLLHUP, .revents = 0};
    const int ready = poll(&pfd, 1, timeout_ms);
    if (ready < 0 && errno != EINTR) {
      return std::nullopt;
    }
    if (ready == 0) {
      return std::string{};
    }

    if ((pfd.revents & POLLIN) == 0) {
      IsRunning();
      return std::nullopt;
    }

    std::string result;
    result.resize(max_bytes);
    const ssize_t n = read(stdout_fd_, result.data(), max_bytes);
    if (n > 0) {
      result.resize(static_cast<std::size_t>(n));
      return result;
    }
    if (n == 0 || (n < 0 && errno != EINTR)) {
      CloseStdout();
      IsRunning();
      return std::nullopt;
    }
    return std::string{};
  }

  std::optional<std::string> ReadExact(std::size_t n, int timeout_ms) override {
    if (stdout_fd_ < 0) {
      return std::nullopt;
    }
    std::string result;
    result.reserve(n);

    while (result.size() < n) {
      const std::size_t remaining = n - result.size();
      pollfd pfd{.fd = stdout_fd_, .events = POLLIN | POLLHUP, .revents = 0};
      const int ready = poll(&pfd, 1, timeout_ms);
      if (ready <= 0) {
        if (ready < 0 && errno == EINTR) {
          continue;
        }
        return std::nullopt;
      }
      if ((pfd.revents & POLLIN) == 0) {
        return std::nullopt;
      }
      const std::size_t old_size = result.size();
      result.resize(old_size + remaining);
      const ssize_t bytes_read = read(stdout_fd_, result.data() + old_size, remaining);
      if (bytes_read > 0) {
        result.resize(old_size + static_cast<std::size_t>(bytes_read));
        continue;
      }
      if (bytes_read == 0 || (bytes_read < 0 && errno != EINTR)) {
        CloseStdout();
        return std::nullopt;
      }
      result.resize(old_size);
    }
    return result;
  }

  void Shutdown(int timeout_ms) override {
    if (!running_ || pid_ < 0) {
      Close();
      return;
    }
    Close();
    kill(pid_, SIGTERM);

    const int kSliceMs = 20;
    int elapsed = 0;
    while (elapsed < timeout_ms) {
      int status = 0;
      if (waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        running_ = false;
        if (WIFEXITED(status)) {
          exit_code_ = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
          exit_code_ = 128 + WTERMSIG(status);
        } else {
          exit_code_ = std::nullopt;
        }
        return;
      }
      struct timespec ts{.tv_sec = 0, .tv_nsec = kSliceMs * 1'000'000L};
      nanosleep(&ts, nullptr);
      elapsed += kSliceMs;
    }

    if (pid_ >= 0) {
      kill(pid_, SIGKILL);
      int status = 0;
      while (waitpid(pid_, &status, 0) < 0) {
        if (errno != EINTR) {
          break;
        }
      }
    }
    pid_ = -1;
    running_ = false;
    exit_code_ = 128 + SIGKILL;
  }

  int pid() const override { return static_cast<int>(pid_); }
  std::optional<int> exit_code() const override { return exit_code_; }

 private:
  void CloseStdin() {
    if (stdin_fd_ >= 0) {
      close(stdin_fd_);
      stdin_fd_ = -1;
    }
  }

  void CloseStdout() {
    if (stdout_fd_ >= 0) {
      close(stdout_fd_);
      stdout_fd_ = -1;
    }
  }

  void Close() {
    CloseStdin();
    CloseStdout();
  }

  pid_t pid_ = -1;
  int stdin_fd_ = -1;
  int stdout_fd_ = -1;
  bool running_ = false;
  std::optional<int> exit_code_;
};

#elif defined(_WIN32)

class UniqueHandle {
 public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE handle) : handle_(handle) {}

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.Release()) {}

  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  ~UniqueHandle() { Reset(); }

  HANDLE Get() const { return handle_; }
  bool IsValid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

  HANDLE Release() {
    HANDLE handle = handle_;
    handle_ = nullptr;
    return handle;
  }

  void Reset(HANDLE handle = nullptr) {
    if (IsValid()) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }

 private:
  HANDLE handle_ = nullptr;
};

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
  if (length <= 0) {
    return {};
  }
  std::wstring wide;
  wide.resize(static_cast<std::size_t>(length));
  if (MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                          length) <= 0) {
    return {};
  }
  return wide;
}

std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    return {};
  }
  std::string utf8;
  utf8.resize(static_cast<std::size_t>(length));
  if (WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(),
                          length, nullptr, nullptr) <= 0) {
    return {};
  }
  return utf8;
}

std::string FormatWindowsError(DWORD error_code) {
  wchar_t* buffer = nullptr;
  const DWORD size = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  if (size == 0 || buffer == nullptr) {
    return "Windows error " + std::to_string(error_code);
  }
  std::wstring_view message(buffer, size);
  std::string utf8 = WideToUtf8(message);
  LocalFree(buffer);
  while (!utf8.empty() && (utf8.back() == '\r' || utf8.back() == '\n' || utf8.back() == ' ')) {
    utf8.pop_back();
  }
  return utf8.empty() ? ("Windows error " + std::to_string(error_code)) : utf8;
}

std::wstring QuoteWindowsCommandArg(std::string_view arg) {
  const std::wstring wide = Utf8ToWide(arg);
  if (wide.empty()) {
    return L"\"\"";
  }
  if (wide.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return wide;
  }

  std::wstring quoted;
  quoted.push_back(L'"');
  std::size_t backslash_count = 0;
  for (wchar_t ch : wide) {
    if (ch == L'\\') {
      ++backslash_count;
      continue;
    }
    if (ch == L'"') {
      quoted.append(backslash_count * 2 + 1, L'\\');
      quoted.push_back(L'"');
      backslash_count = 0;
      continue;
    }
    if (backslash_count > 0) {
      quoted.append(backslash_count, L'\\');
      backslash_count = 0;
    }
    quoted.push_back(ch);
  }
  if (backslash_count > 0) {
    quoted.append(backslash_count * 2, L'\\');
  }
  quoted.push_back(L'"');
  return quoted;
}

std::wstring BuildWindowsCommandLine(const std::vector<std::string>& argv) {
  std::wstring command_line;
  bool first = true;
  for (const std::string& arg : argv) {
    if (!first) {
      command_line.push_back(L' ');
    }
    first = false;
    command_line += QuoteWindowsCommandArg(arg);
  }
  return command_line;
}

std::wstring ToLowerWide(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
    return static_cast<wchar_t>(towlower(ch));
  });
  return value;
}

std::vector<wchar_t> BuildEnvironmentBlock(
    const std::vector<SubprocessEnvironmentOverride>& overrides) {
  std::map<std::wstring, std::pair<std::wstring, std::wstring>> environment;

  LPWCH raw_block = GetEnvironmentStringsW();
  if (raw_block != nullptr) {
    for (LPCWCH entry = raw_block; *entry != L'\0'; entry += wcslen(entry) + 1) {
      std::wstring_view value(entry, wcslen(entry));
      std::size_t equals = value.find(L'=');
      if (equals == std::wstring_view::npos || equals == 0) {
        continue;
      }
      std::wstring key(value.substr(0, equals));
      std::wstring actual_value(value.substr(equals + 1));
      environment[ToLowerWide(key)] = {key, actual_value};
    }
    FreeEnvironmentStringsW(raw_block);
  }

  for (const auto& override_entry : overrides) {
    if (override_entry.name.empty()) {
      continue;
    }
    const std::wstring key = Utf8ToWide(override_entry.name);
    const std::wstring normalized_key = ToLowerWide(key);
    if (!override_entry.value.has_value()) {
      environment.erase(normalized_key);
      continue;
    }
    environment[normalized_key] = {key, Utf8ToWide(*override_entry.value)};
  }

  std::vector<wchar_t> block;
  for (const auto& [_, entry] : environment) {
    const std::wstring line = entry.first + L"=" + entry.second;
    block.insert(block.end(), line.begin(), line.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

bool CreateInheritablePipe(UniqueHandle* read_end, UniqueHandle* write_end) {
  if (read_end == nullptr || write_end == nullptr) {
    return false;
  }
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE raw_read = nullptr;
  HANDLE raw_write = nullptr;
  if (!CreatePipe(&raw_read, &raw_write, &attributes, 0)) {
    return false;
  }
  read_end->Reset(raw_read);
  write_end->Reset(raw_write);
  return true;
}

bool CreateChildProcess(const std::vector<std::string>& argv,
                        const std::string& cwd,
                        const std::vector<SubprocessEnvironmentOverride>& environment_overrides,
                        HANDLE stdin_handle,
                        HANDLE stdout_handle,
                        HANDLE stderr_handle,
                        PROCESS_INFORMATION* process_information,
                        std::string* error_message) {
  if (process_information == nullptr) {
    return false;
  }

  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  startup_info.hStdInput = stdin_handle;
  startup_info.hStdOutput = stdout_handle;
  startup_info.hStdError = stderr_handle;

  std::wstring command_line = BuildWindowsCommandLine(argv);
  std::wstring working_directory = Utf8ToWide(cwd);
  std::vector<wchar_t> environment_block = BuildEnvironmentBlock(environment_overrides);

  ZeroMemory(process_information, sizeof(*process_information));
  const BOOL ok = CreateProcessW(
      nullptr,
      command_line.empty() ? nullptr : command_line.data(),
      nullptr,
      nullptr,
      TRUE,
      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
      environment_block.empty() ? nullptr : environment_block.data(),
      working_directory.empty() ? nullptr : working_directory.c_str(),
      &startup_info,
      process_information);
  if (!ok) {
    if (error_message != nullptr) {
      *error_message = FormatWindowsError(GetLastError());
    }
    return false;
  }
  return true;
}

void ReadHandleToString(HANDLE handle, std::string* output) {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE || output == nullptr) {
    return;
  }

  std::array<char, 4096> buffer{};
  DWORD bytes_read = 0;
  while (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr)) {
    if (bytes_read == 0) {
      return;
    }
    output->append(buffer.data(), static_cast<std::size_t>(bytes_read));
  }
}

class WindowsAsyncProcessBackend final : public AsyncProcessBackend {
 public:
  bool Start(const std::vector<std::string>& argv, const std::string& cwd) override {
    if (argv.empty() || process_.IsValid()) {
      return false;
    }

    UniqueHandle child_stdout_read;
    UniqueHandle child_stdout_write;
    UniqueHandle child_stdin_read;
    UniqueHandle child_stdin_write;
    if (!CreateInheritablePipe(&child_stdout_read, &child_stdout_write) ||
        !CreateInheritablePipe(&child_stdin_read, &child_stdin_write)) {
      return false;
    }

    SetHandleInformation(child_stdout_read.Get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stdin_write.Get(), HANDLE_FLAG_INHERIT, 0);

    PROCESS_INFORMATION process_information{};
    if (!CreateChildProcess(argv, cwd, {}, child_stdin_read.Get(), child_stdout_write.Get(),
                            GetStdHandle(STD_ERROR_HANDLE), &process_information, &last_error_)) {
      return false;
    }

    child_stdout_write.Reset();
    child_stdin_read.Reset();
    process_.Reset(process_information.hProcess);
    thread_.Reset(process_information.hThread);
    stdin_write_ = std::move(child_stdin_write);
    stdout_read_ = std::move(child_stdout_read);
    exit_code_.reset();
    return true;
  }

  bool IsRunning() override {
    if (!process_.IsValid()) {
      return false;
    }
    const DWORD wait_result = WaitForSingleObject(process_.Get(), 0);
    if (wait_result == WAIT_TIMEOUT) {
      return true;
    }
    if (wait_result == WAIT_OBJECT_0) {
      DWORD exit_code = 0;
      if (GetExitCodeProcess(process_.Get(), &exit_code)) {
        exit_code_ = static_cast<int>(exit_code);
      }
      process_.Reset();
      thread_.Reset();
      return false;
    }
    return false;
  }

  bool Write(std::string_view data) override {
    if (!stdin_write_.IsValid()) {
      return false;
    }
    const char* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
      DWORD written = 0;
      if (!WriteFile(stdin_write_.Get(), cursor,
                     static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 20)), &written,
                     nullptr)) {
        stdin_write_.Reset();
        return false;
      }
      cursor += written;
      remaining -= written;
    }
    return true;
  }

  std::optional<std::string> Read(std::size_t max_bytes, int timeout_ms) override {
    if (!stdout_read_.IsValid()) {
      return std::nullopt;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    for (;;) {
      DWORD available = 0;
      if (!PeekNamedPipe(stdout_read_.Get(), nullptr, 0, nullptr, &available, nullptr)) {
        stdout_read_.Reset();
        IsRunning();
        return std::nullopt;
      }
      if (available > 0) {
        std::string result;
        result.resize(std::min<std::size_t>(max_bytes, static_cast<std::size_t>(available)));
        DWORD bytes_read = 0;
        if (!ReadFile(stdout_read_.Get(), result.data(), static_cast<DWORD>(result.size()),
                      &bytes_read, nullptr) ||
            bytes_read == 0) {
          stdout_read_.Reset();
          IsRunning();
          return std::nullopt;
        }
        result.resize(static_cast<std::size_t>(bytes_read));
        return result;
      }
      if (!IsRunning()) {
        stdout_read_.Reset();
        return std::nullopt;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return std::string{};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  std::optional<std::string> ReadExact(std::size_t n, int timeout_ms) override {
    std::string result;
    result.reserve(n);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    while (result.size() < n) {
      const int remaining_timeout = static_cast<int>(std::max<std::int64_t>(
          0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                   std::chrono::steady_clock::now())
                 .count()));
      const auto chunk = Read(n - result.size(), remaining_timeout);
      if (!chunk.has_value()) {
        return std::nullopt;
      }
      if (chunk->empty()) {
        if (std::chrono::steady_clock::now() >= deadline) {
          return std::nullopt;
        }
        continue;
      }
      result += *chunk;
    }
    return result;
  }

  void Shutdown(int timeout_ms) override {
    stdin_write_.Reset();
    stdout_read_.Reset();
    if (!process_.IsValid()) {
      return;
    }
    const DWORD wait_result = WaitForSingleObject(process_.Get(), std::max(timeout_ms, 0));
    if (wait_result == WAIT_TIMEOUT) {
      TerminateProcess(process_.Get(), 1);
      WaitForSingleObject(process_.Get(), INFINITE);
    }
    DWORD exit_code = 0;
    if (GetExitCodeProcess(process_.Get(), &exit_code)) {
      exit_code_ = static_cast<int>(exit_code);
    }
    process_.Reset();
    thread_.Reset();
  }

  int pid() const override { return static_cast<int>(process_id_); }
  std::optional<int> exit_code() const override { return exit_code_; }

 private:
  UniqueHandle process_;
  UniqueHandle thread_;
  UniqueHandle stdin_write_;
  UniqueHandle stdout_read_;
  DWORD process_id_ = 0;
  std::optional<int> exit_code_;
  std::string last_error_;
};

#else

class UnavailableAsyncProcessBackend final : public AsyncProcessBackend {
 public:
  bool Start(const std::vector<std::string>&, const std::string&) override { return false; }
  bool IsRunning() override { return false; }
  bool Write(std::string_view) override { return false; }
  std::optional<std::string> Read(std::size_t, int) override { return std::nullopt; }
  std::optional<std::string> ReadExact(std::size_t, int) override { return std::nullopt; }
  void Shutdown(int) override {}
  int pid() const override { return -1; }
  std::optional<int> exit_code() const override { return std::nullopt; }
};

#endif

}  // namespace

SubprocessResult RunSubprocessWithBackend(const std::vector<std::string>& argv,
                                         const SubprocessOptions& options) {
  SubprocessResult result;
  if (argv.empty()) {
    return result;
  }

#if defined(__unix__) || defined(__APPLE__)
  std::array<UniqueFd, 2> stdout_pipe;
  std::array<UniqueFd, 2> stderr_pipe;
  std::array<UniqueFd, 2> stdin_pipe;

  const bool needs_stdin = !options.stdin_text.empty();
  if (!OpenPipe(options.capture_stdout, &stdout_pipe) ||
      !OpenPipe(options.capture_stderr, &stderr_pipe) || !OpenPipe(needs_stdin, &stdin_pipe)) {
    return result;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    return result;
  }

  if (pid == 0) {
    if (options.capture_stdout) {
      dup2(stdout_pipe[1].Get(), STDOUT_FILENO);
    }
    if (options.capture_stderr) {
      dup2(stderr_pipe[1].Get(), STDERR_FILENO);
    } else if (options.silence_stderr) {
      UniqueFd devnull(open("/dev/null", O_WRONLY));
      if (devnull.IsValid()) {
        dup2(devnull.Get(), STDERR_FILENO);
      }
    }
    if (needs_stdin) {
      dup2(stdin_pipe[0].Get(), STDIN_FILENO);
    }

    stdout_pipe[0].Reset();
    stdout_pipe[1].Reset();
    stderr_pipe[0].Reset();
    stderr_pipe[1].Reset();
    stdin_pipe[0].Reset();
    stdin_pipe[1].Reset();

    if (!options.cwd.empty()) {
      if (chdir(options.cwd.string().c_str()) != 0) {
        _exit(127);
      }
    }
    ApplyEnvironmentOverrides(options.environment_overrides);

    std::vector<char*> raw_argv;
    raw_argv.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
      raw_argv.push_back(const_cast<char*>(arg.c_str()));
    }
    raw_argv.push_back(nullptr);
    execvp(raw_argv[0], raw_argv.data());
    _exit(errno == ENOENT ? 127 : 126);
  }

  stdout_pipe[1].Reset();
  stderr_pipe[1].Reset();
  stdin_pipe[0].Reset();
  if (needs_stdin) {
    WriteAllToPipe(stdin_pipe[1].Get(), options.stdin_text);
    stdin_pipe[1].Reset();
  }

  DrainCapturedPipes(options.capture_stdout ? &stdout_pipe[0] : nullptr, &result.stdout_text,
                     options.capture_stderr ? &stderr_pipe[0] : nullptr, &result.stderr_text);
  stdout_pipe[0].Reset();
  stderr_pipe[0].Reset();

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      result.exit_code = -1;
      return result;
    }
  }

  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  return result;
#elif defined(_WIN32)
  UniqueHandle stdout_read;
  UniqueHandle stdout_write;
  UniqueHandle stderr_read;
  UniqueHandle stderr_write;
  UniqueHandle stdin_read;
  UniqueHandle stdin_write;

  if (options.capture_stdout && !CreateInheritablePipe(&stdout_read, &stdout_write)) {
    result.stderr_text = "Failed to create stdout pipe";
    return result;
  }
  if (options.capture_stderr && !CreateInheritablePipe(&stderr_read, &stderr_write)) {
    result.stderr_text = "Failed to create stderr pipe";
    return result;
  }
  if (!options.stdin_text.empty() && !CreateInheritablePipe(&stdin_read, &stdin_write)) {
    result.stderr_text = "Failed to create stdin pipe";
    return result;
  }

  if (stdout_read.IsValid()) {
    SetHandleInformation(stdout_read.Get(), HANDLE_FLAG_INHERIT, 0);
  }
  if (stderr_read.IsValid()) {
    SetHandleInformation(stderr_read.Get(), HANDLE_FLAG_INHERIT, 0);
  }
  if (stdin_write.IsValid()) {
    SetHandleInformation(stdin_write.Get(), HANDLE_FLAG_INHERIT, 0);
  }

  UniqueHandle null_stderr;
  if (!options.capture_stderr && options.silence_stderr) {
    null_stderr.Reset(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  }

  HANDLE child_stdin = stdin_read.IsValid() ? stdin_read.Get() : GetStdHandle(STD_INPUT_HANDLE);
  HANDLE child_stdout =
      stdout_write.IsValid() ? stdout_write.Get() : GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE child_stderr = stderr_write.IsValid()
                            ? stderr_write.Get()
                            : (null_stderr.IsValid() ? null_stderr.Get()
                                                     : GetStdHandle(STD_ERROR_HANDLE));

  PROCESS_INFORMATION process_information{};
  std::string launch_error;
  if (!CreateChildProcess(argv, options.cwd.string(), options.environment_overrides, child_stdin,
                          child_stdout, child_stderr, &process_information, &launch_error)) {
    result.stderr_text = launch_error.empty() ? "Failed to start subprocess" : launch_error;
    return result;
  }

  UniqueHandle process(process_information.hProcess);
  UniqueHandle thread(process_information.hThread);
  stdout_write.Reset();
  stderr_write.Reset();
  stdin_read.Reset();

  std::thread stdout_thread;
  std::thread stderr_thread;
  if (options.capture_stdout && stdout_read.IsValid()) {
    stdout_thread = std::thread([&]() { ReadHandleToString(stdout_read.Get(), &result.stdout_text); });
  }
  if (options.capture_stderr && stderr_read.IsValid()) {
    stderr_thread = std::thread([&]() { ReadHandleToString(stderr_read.Get(), &result.stderr_text); });
  }

  if (stdin_write.IsValid()) {
    DWORD written = 0;
    const char* cursor = options.stdin_text.data();
    std::size_t remaining = options.stdin_text.size();
    while (remaining > 0) {
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1u << 20));
      if (!WriteFile(stdin_write.Get(), cursor, chunk, &written, nullptr)) {
        break;
      }
      cursor += written;
      remaining -= written;
    }
    stdin_write.Reset();
  }

  WaitForSingleObject(process.Get(), INFINITE);
  if (stdout_thread.joinable()) {
    stdout_thread.join();
  }
  if (stderr_thread.joinable()) {
    stderr_thread.join();
  }
  stdout_read.Reset();
  stderr_read.Reset();

  DWORD exit_code = 0;
  if (GetExitCodeProcess(process.Get(), &exit_code)) {
    result.exit_code = static_cast<int>(exit_code);
  }
  return result;
#endif
  result.stderr_text = "Subprocess execution is not implemented on this platform";
  return result;
}

std::unique_ptr<AsyncProcessBackend> CreateAsyncProcessBackend() {
#if defined(__unix__) || defined(__APPLE__)
  return std::make_unique<PosixAsyncProcessBackend>();
#elif defined(_WIN32)
  return std::make_unique<WindowsAsyncProcessBackend>();
#else
  return std::make_unique<UnavailableAsyncProcessBackend>();
#endif
}

}  // namespace microide::platform

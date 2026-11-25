#include "util/PosixFd.h"
#include "app/AppStartupOptions.h"
#include "app/Application.h"
#include "workspace/persistence/PersistedRecordDump.h"
#include "platform/CrashDiagnostics.h"
#include "platform/HostPlatform.h"
#include "platform/OpenFileLimit.h"
#include "util/CrashPointControl.h"
#include "util/DebugTrace.h"
#include "util/TraceChannel.h"
#include "workspace/control/ControlChannelService.h"
#include "workspace/control/ControlClient.h"
#include "workspace/control/ControlProtocol.h"
#include "workspace/ManPage.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#ifndef MICROIDE_VERSION
#define MICROIDE_VERSION "dev"
#endif

int main(int argc, char** argv) {
  microide::util::MarkTracingMainThread();
  microide::util::EnsureStandardDescriptorsOpen();
  microide::platform::RaiseOpenFileLimit();

  microide::platform::IgnoreBrokenPipeSignal();
  microide::platform::InstallCrashBacktraceHandler();
  (void)microide::util::CrashPointControl::InstallFromEnvironment();
  microide::platform::AllowParentToTraceWhenAskedByQa();
  (void)microide::platform::UseUtf8CharacterLocale();

  if (argc >= 2) {
    const std::string_view command =
        argv[1] != nullptr ? std::string_view(argv[1]) : std::string_view{};
    if (command == "control-help") {
      std::cout << microide::workspace::ControlChannelHelpText();
      std::cout << "\nRunnable commands\n-----------------\n";
      for (const std::string& usage : microide::workspace::WorkspaceDocumentedCommandUsages()) {
        std::cout << "  " << usage << '\n';
      }
      return 0;
    }
    if (command == "control-commands") {
      for (const std::string& usage : microide::workspace::WorkspaceDocumentedCommandUsages()) {
        std::cout << usage << '\n';
      }
      return 0;
    }
    if (command == "control-list") {
      std::cout << microide::workspace::ControlListInstancesText();
      return 0;
    }
    if (command == "control-send") {
      return microide::workspace::RunControlSend(argc, argv);
    }
    if (command == "control-man") {
      std::cout << microide::workspace::RenderManPage();
      return 0;
    }
    if (command == "dump-state" || command == "microide-dump-state") {
      if (argc != 3 || argv[2] == nullptr || std::string_view(argv[2]).empty()) {
        std::cerr << "usage: microide dump-state <persisted-file>\n";
        return 2;
      }
      std::string dump;
      std::string error;
      if (!microide::persistence::DumpPersistedRecordFile(
              std::filesystem::path(argv[2]), &dump, &error)) {
        std::cerr << "dump-state failed: " << error << '\n';
        return 1;
      }
      std::cout << dump;
      return 0;
    }
  }

  const microide::app::AppStartupParseResult parsed =
      microide::app::ParseAppStartupOptions(argc, argv);
  if (parsed.show_usage) {
    return 0;
  }
  if (parsed.show_version) {
    std::cout << "microide " << MICROIDE_VERSION << '\n';
    return 0;
  }
  if (parsed.exit_code != 0) {
    return parsed.exit_code;
  }

  if (parsed.options.dap_log_path.has_value()) {
    microide::util::DebugTrace::EnableToFile(*parsed.options.dap_log_path);
    std::cerr << "DAP trace \xE2\x86\x92 " << parsed.options.dap_log_path->string() << '\n';
  }

  microide::app::Application application(parsed.options);
  return application.Run();
}

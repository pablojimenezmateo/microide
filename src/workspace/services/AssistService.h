#pragma once

#include <string_view>

#include "workspace/assist/AssistHost.h"
#include "workspace/assist/CodeActionAssist.h"
#include "workspace/assist/CompletionAssist.h"
#include "workspace/assist/NavigationAssist.h"
#include "workspace/assist/RefactorAssist.h"
#include "workspace/assist/SignatureHelpAssist.h"
#include "workspace/assist/SnippetAssist.h"

namespace microide::workspace {

class AssistService {
 public:
  using Operations = AssistHost::Operations;
  struct Ports {
    CompletionAssist::Port completion;
    CodeActionAssist::Port code_actions;
    NavigationAssist::Port navigation;
    SignatureHelpAssist::Port signature_help;
  };

  AssistService() = default;
  AssistService(const AssistService&) = delete;
  AssistService& operator=(const AssistService&) = delete;

  void Configure(WorkspaceContext& context,
                 WorkspacePluginRuntime& plugin_runtime,
                 WorkspaceOutputChannels& output_channels,
                 WorkspaceLanguageContract& language_contract,
                 WorkspaceEditService& edits,
                 Operations operations,
                 Ports ports) {
    host_.Configure(context, plugin_runtime, output_channels, language_contract, edits,
                    std::move(operations));
    completion_.Configure(std::move(ports.completion));
    code_actions_.Configure(std::move(ports.code_actions));
    navigation_.Configure(std::move(ports.navigation));
    signature_help_.Configure(std::move(ports.signature_help));
  }

  CompletionAssist& completion() { return completion_; }
  SnippetAssist& snippets() { return snippets_; }
  CodeActionAssist& code_actions() { return code_actions_; }
  NavigationAssist& navigation() { return navigation_; }
  RefactorAssist& refactor() { return refactor_; }
  SignatureHelpAssist& signature_help() { return signature_help_; }

  void AfterTyping(std::string_view typed) {
    completion_.AfterTyping(typed);
    signature_help_.AfterTyping(typed);
  }

 private:
  AssistHost host_;
  CompletionAssist completion_{host_};
  SnippetAssist snippets_{host_};
  CodeActionAssist code_actions_{host_};
  NavigationAssist navigation_{host_};
  RefactorAssist refactor_{host_};
  SignatureHelpAssist signature_help_{host_};
};

}

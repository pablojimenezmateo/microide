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

// The editor's language assists, one focused unit per feature over a shared
// AssistHost (the shell seam and the helpers every feature needs). Callers reach
// the feature they mean — `assist.completion().ApplySelectedCompletion()` — so no
// one object accretes every assist's state and entry points again.
class AssistService {
 public:
  using Operations = AssistHost::Operations;

  AssistService() = default;
  AssistService(const AssistService&) = delete;
  AssistService& operator=(const AssistService&) = delete;

  void Configure(WorkspaceContext& context,
                 WorkspacePluginRuntime& plugin_runtime,
                 WorkspaceOutputChannels& output_channels,
                 WorkspaceLanguageContract& language_contract,
                 WorkspaceEditService& edits,
                 Operations operations) {
    host_.Configure(context, plugin_runtime, output_channels, language_contract, edits,
                    std::move(operations));
  }

  CompletionAssist& completion() { return completion_; }
  SnippetAssist& snippets() { return snippets_; }
  CodeActionAssist& code_actions() { return code_actions_; }
  NavigationAssist& navigation() { return navigation_; }
  RefactorAssist& refactor() { return refactor_; }
  SignatureHelpAssist& signature_help() { return signature_help_; }

  // The editor's typing hook: completion and parameter hints both follow typing.
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

}  // namespace microide::workspace

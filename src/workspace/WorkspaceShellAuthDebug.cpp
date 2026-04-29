#include "workspace/WorkspaceShell.h"

namespace microide::workspace {

bool WorkspaceShell::StartDebugger(std::string_view type, std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (type.empty()) {
    if (error_message != nullptr) {
      *error_message = "Debugger type is required";
    }
    return false;
  }
  if (dap_manager_.GetDebugger(std::string(type)) == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Unknown debugger: " + std::string(type);
    }
    return false;
  }
  context_.current_project_state.debug_session.running = true;
  context_.current_project_state.debug_session.type = std::string(type);
  context_.current_project_state.debug_session.channel_id = "debug." + std::string(type);
  context_.current_project_state.debug_session.status_text = "Running";
  output_channels_.AppendLine(context_.current_project_state.debug_session.channel_id,
                              "Debugger " + std::string(type),
                              "Debugger " + std::string(type) + " started");
  ShowOutputChannel(context_.current_project_state.debug_session.channel_id);
  return true;
}

void WorkspaceShell::StopDebugger() {
  if (context_.current_project_state.debug_session.running &&
      !context_.current_project_state.debug_session.channel_id.empty()) {
    output_channels_.AppendLine(context_.current_project_state.debug_session.channel_id,
                                "Debugger " + context_.current_project_state.debug_session.type,
                                "Debugger stopped");
  }
  dap_manager_.ShutdownAll();
  context_.current_project_state.debug_session = DebugSessionState{};
}

bool WorkspaceShell::LoginAuthProvider(std::string_view provider_id,
                                       const std::vector<std::string>& scopes,
                                       std::string* error_message) {
  plugin::PluginHost::AuthSessionData session;
  if (!plugin_runtime_.Host().LoginAuthProvider(provider_id, scopes, &session, error_message)) {
    return false;
  }
  auth_provider_registry_.AddSession(AuthSession{
      .id = session.id,
      .provider_id = std::string(provider_id),
      .account = session.account,
      .access_token = session.access_token,
      .scopes = session.scopes,
  });
  if (!session.access_token.empty()) {
    secret_storage_.Store(std::string(provider_id) + "." + session.id, session.access_token);
  }
  output_channels_.AppendLine("auth." + std::string(provider_id), "Auth " + std::string(provider_id),
                              "Logged in as " + session.account);
  ShowOutputChannel("auth." + std::string(provider_id));
  RequestChromeRedraw();
  RequestSidebarRedraw();
  return true;
}

bool WorkspaceShell::RefreshAuthSession(std::string_view provider_id,
                                        std::string_view session_id,
                                        std::string* error_message) {
  plugin::PluginHost::AuthSessionData session;
  if (!plugin_runtime_.Host().RefreshAuthSession(provider_id, session_id, &session,
                                                 error_message)) {
    return false;
  }
  auth_provider_registry_.RemoveSession(std::string(session_id));
  auth_provider_registry_.AddSession(AuthSession{
      .id = session.id,
      .provider_id = std::string(provider_id),
      .account = session.account,
      .access_token = session.access_token,
      .scopes = session.scopes,
  });
  if (!session.access_token.empty()) {
    secret_storage_.Store(std::string(provider_id) + "." + session.id, session.access_token);
  }
  output_channels_.AppendLine("auth." + std::string(provider_id), "Auth " + std::string(provider_id),
                              "Refreshed session " + session.id);
  ShowOutputChannel("auth." + std::string(provider_id));
  RequestChromeRedraw();
  RequestSidebarRedraw();
  return true;
}

bool WorkspaceShell::LogoutAuthSession(std::string_view provider_id,
                                       std::string_view session_id,
                                       std::string* error_message) {
  if (!plugin_runtime_.Host().LogoutAuthSession(provider_id, session_id, error_message)) {
    return false;
  }
  auth_provider_registry_.RemoveSession(std::string(session_id));
  secret_storage_.Delete(std::string(provider_id) + "." + std::string(session_id));
  output_channels_.AppendLine("auth." + std::string(provider_id), "Auth " + std::string(provider_id),
                              "Logged out session " + std::string(session_id));
  ShowOutputChannel("auth." + std::string(provider_id));
  RequestChromeRedraw();
  RequestSidebarRedraw();
  return true;
}

}  // namespace microide::workspace

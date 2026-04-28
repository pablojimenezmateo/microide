#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspacePersistenceFormat.h"

namespace microide::workspace {

std::string EncodeSessionNodePath(const std::vector<std::size_t>& path);
std::optional<std::vector<std::size_t>> DecodeSessionNodePath(std::string_view text);

bool ParseUserConfigText(std::string_view text, PersistedUserConfigState* state);
std::string SerializeUserConfig(const PersistedUserConfigState& state);
bool ParseProjectConfigText(std::string_view text, PersistedProjectConfigState* state);
std::string SerializeProjectConfig(const PersistedProjectConfigState& state);
bool ParseProjectSessionText(std::string_view text, PersistedProjectSessionState* state);
std::string SerializeProjectSession(const PersistedProjectSessionState& state);
bool ParseWorkspaceSessionText(std::string_view text, PersistedWorkspaceSessionState* state);
std::string SerializeWorkspaceSession(const PersistedWorkspaceSessionState& state);

}  // namespace microide::workspace

#include "workspace/WorkspaceSaveParticipants.h"

namespace microide::workspace {

SaveParticipantRegistry::SaveParticipantRegistry() = default;
SaveParticipantRegistry::~SaveParticipantRegistry() = default;

void SaveParticipantRegistry::Register(const SaveParticipantSpec& spec) {
  specs_.push_back(spec);
}

}  // namespace microide::workspace

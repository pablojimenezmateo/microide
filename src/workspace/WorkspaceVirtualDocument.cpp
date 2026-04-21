#include "workspace/WorkspaceVirtualDocument.h"

namespace microide::workspace {

VirtualDocumentRegistry::VirtualDocumentRegistry() = default;
VirtualDocumentRegistry::~VirtualDocumentRegistry() = default;

void VirtualDocumentRegistry::Register(const VirtualDocumentSpec& spec) {
  documents_[spec.uri] = spec;
}

const VirtualDocumentSpec* VirtualDocumentRegistry::GetDocument(const std::string& uri) const {
  auto it = documents_.find(uri);
  if (it == documents_.end()) {
    return nullptr;
  }
  return &it->second;
}

void VirtualDocumentRegistry::UpdateContent(const std::string& uri, const std::string& content) {
  auto it = documents_.find(uri);
  if (it != documents_.end()) {
    it->second.content = content;
    if (on_change_) {
      on_change_(uri);
    }
  }
}

std::vector<std::string> VirtualDocumentRegistry::DocumentUris() const {
  std::vector<std::string> result;
  for (const auto& [uri, _] : documents_) {
    result.push_back(uri);
  }
  return result;
}

void VirtualDocumentRegistry::Remove(const std::string& uri) { documents_.erase(uri); }

}  // namespace microide::workspace

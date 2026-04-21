#pragma once

#include <functional>
#include <string>
#include <unordered_map>

namespace microide::workspace {

// Virtual document: a document provided by a plugin (not backed by a real file).
// Used for diff views, merge views, generated docs, etc.
struct VirtualDocumentSpec {
  std::string uri;  // e.g., "virtual://provider-id/document-id"
  std::string language_id;
  std::string content;
  bool editable = false;
  std::string plugin_id;
};

// Virtual document provider: creates and manages virtual documents.
class VirtualDocumentRegistry {
 public:
  VirtualDocumentRegistry();
  ~VirtualDocumentRegistry();

  // Register a virtual document.
  void Register(const VirtualDocumentSpec& spec);

  // Get virtual document by URI.
  const VirtualDocumentSpec* GetDocument(const std::string& uri) const;

  // Update document content.
  void UpdateContent(const std::string& uri, const std::string& content);

  // List all virtual documents.
  std::vector<std::string> DocumentUris() const;

  // Remove virtual document.
  void Remove(const std::string& uri);

  // Set change callback.
  using OnContentChange = std::function<void(const std::string& uri)>;
  void SetOnChange(OnContentChange callback) { on_change_ = std::move(callback); }

 private:
  std::unordered_map<std::string, VirtualDocumentSpec> documents_;
  OnContentChange on_change_;
};

}  // namespace microide::workspace

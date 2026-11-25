#pragma once

#include <functional>
#include <string>
#include <unordered_map>

namespace microide::workspace {

struct VirtualDocumentSpec {
  std::string uri;
  std::string language_id;
  std::string content;
  bool editable = false;
  std::string plugin_id;
};

class VirtualDocumentRegistry {
 public:
  VirtualDocumentRegistry();
  ~VirtualDocumentRegistry();

  void Register(const VirtualDocumentSpec& spec);

  const VirtualDocumentSpec* GetDocument(const std::string& uri) const;

  void UpdateContent(const std::string& uri, const std::string& content);

  std::vector<std::string> DocumentUris() const;

  void Remove(const std::string& uri);

  void Clear() { documents_.clear(); }

  using OnContentChange = std::function<void(const std::string& uri)>;
  void SetOnChange(OnContentChange callback) { on_change_ = std::move(callback); }

 private:
  std::unordered_map<std::string, VirtualDocumentSpec> documents_;
  OnContentChange on_change_;
};

}

#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// Blame/annotation provider: adds inline blame, decoration, or margin annotations.
struct AnnotationProviderSpec {
  std::string id;
  std::string label;
  std::string type;  // "blame", "decoration", "margin"
  std::string language_id;
  std::string plugin_id;
};

// Annotation registry: manages code annotation providers.
class AnnotationRegistry {
 public:
  AnnotationRegistry();
  ~AnnotationRegistry();

  void Register(const AnnotationProviderSpec& spec);
  const std::vector<AnnotationProviderSpec>& Specs() const { return specs_; }

 private:
  std::vector<AnnotationProviderSpec> specs_;
};

}  // namespace microide::workspace

#pragma once

#include <utility>
#include <vector>

namespace microide::workspace {

// Generic append-only registry of plugin-contributed provider specs.
//
// Every workspace provider registry (completion, code-action, formatter,
// annotation, tool, SCM, save-participant) is the same shape: a vector of spec
// structs that plugins append to, read back in order, and occasionally linear
// scan for the first match. This template captures that shape so each concrete
// registry is a one-line `using` alias plus its spec struct; the intent-named
// lookups (FindFormatter, FindTool, ...) live as inline free functions beside
// each alias.
//
// Plugin reload resets a registry by whole-object assignment
// (`formatter_registry_ = FormatterRegistry{};`), which this type supports
// trivially.
template <typename Spec>
class ProviderRegistry {
 public:
  void Register(Spec spec) { specs_.push_back(std::move(spec)); }

  const std::vector<Spec>& Specs() const { return specs_; }

  // First spec satisfying `pred`, or nullptr. Used by the intent-named free
  // functions to keep "first match wins" semantics in one place.
  template <typename Pred>
  const Spec* FindIf(Pred pred) const {
    for (const Spec& spec : specs_) {
      if (pred(spec)) {
        return &spec;
      }
    }
    return nullptr;
  }

 private:
  std::vector<Spec> specs_;
};

}  // namespace microide::workspace

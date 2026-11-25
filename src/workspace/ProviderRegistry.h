#pragma once

#include <utility>
#include <vector>

namespace microide::workspace {

template <typename Spec>
class ProviderRegistry {
 public:
  void Register(Spec spec) { specs_.push_back(std::move(spec)); }

  const std::vector<Spec>& Specs() const { return specs_; }

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

}

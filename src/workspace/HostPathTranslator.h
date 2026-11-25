#pragma once

#include "platform/ProcessLauncher.h"
#include "util/JsonValue.h"

namespace microide::workspace {

class HostPathTranslator {
 public:
  HostPathTranslator() = default;
  explicit HostPathTranslator(const platform::ProcessLauncher& launcher)
      : launcher_(launcher.is_local() ? nullptr : &launcher) {}

  bool active() const { return launcher_ != nullptr; }

  void ToHost(util::JsonValue& message) const;
  void FromHost(util::JsonValue& message) const;

 private:
  enum class Direction { kToHost, kFromHost };
  void Walk(util::JsonValue& value, Direction direction) const;
  void TranslateUri(std::string& uri, Direction direction) const;
  void TranslatePath(std::string& path, Direction direction) const;

  const platform::ProcessLauncher* launcher_ = nullptr;
};

}

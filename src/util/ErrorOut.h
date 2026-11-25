#pragma once

#include <string>
#include <utility>

namespace microide::util {

inline void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

[[nodiscard]] inline bool Fail(std::string* error, std::string message) {
  SetError(error, std::move(message));
  return false;
}

inline void ClearError(std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
}

}

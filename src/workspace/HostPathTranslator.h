#pragma once

#include "platform/ProcessLauncher.h"
#include "util/JsonValue.h"

namespace microide::workspace {

// Translates the paths inside a JSON-RPC message between the editor's tree and the
// tree a host process sees (dev-docs/design/remote-projects.md § 6.5). A language
// server or debug adapter runs where the project's launcher runs it; in a remote
// project that is a different machine, whose root is not the mirror's, so every
// path the editor sends must be the host's and every path the process answers
// with must come back as the editor's -- otherwise the server is told about files
// it cannot open, and go-to-definition opens the host's copy behind the mirror's
// back.
//
// One translator at the transport boundary, shared by the LSP and DAP clients,
// rather than a mapping call at each of the dozens of sites that build or read a
// URI: a site that forgets is a site that is right locally and wrong remotely, and
// nothing reports it. Only KNOWN path-bearing keys are rewritten -- never a
// string the user typed: a document's text may contain a file URI and must reach
// the server byte for byte.
//
// Inactive (a no-op, not even a walk) for a local launcher, so local projects pay
// nothing.
class HostPathTranslator {
 public:
  HostPathTranslator() = default;
  explicit HostPathTranslator(const platform::ProcessLauncher& launcher)
      : launcher_(launcher.is_local() ? nullptr : &launcher) {}

  bool active() const { return launcher_ != nullptr; }

  // Editor -> host, for an outgoing message.
  void ToHost(util::JsonValue& message) const;
  // Host -> editor, for an incoming message.
  void FromHost(util::JsonValue& message) const;

 private:
  enum class Direction { kToHost, kFromHost };
  void Walk(util::JsonValue& value, Direction direction) const;
  void TranslateUri(std::string& uri, Direction direction) const;
  void TranslatePath(std::string& path, Direction direction) const;

  const platform::ProcessLauncher* launcher_ = nullptr;
};

}  // namespace microide::workspace

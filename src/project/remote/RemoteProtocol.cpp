#include "project/remote/RemoteProtocol.h"

#include <utility>

namespace microide::project::remote {
namespace {

// Bounds on what a peer may send in the handshake. Generous for real values, small
// enough that a hostile peer cannot make the handshake allocate much.
constexpr std::size_t kMaxReleaseBytes = 64;
constexpr std::size_t kMaxRootBytes = 4096;
constexpr std::size_t kMaxCapabilities = 64;
constexpr std::size_t kMaxCapabilityBytes = 64;
constexpr std::size_t kMaxEpochBytes = 64;

bool ReadVersion(const util::JsonValue& json, std::string_view key, std::int64_t* out,
                 std::string* error) {
  const util::JsonValue& value = json[key];
  if (!value.IsInt() || value.AsInt() < 1 || value.AsInt() > 1'000'000) {
    *error = std::string(key) + " must be a positive integer";
    return false;
  }
  *out = value.AsInt();
  return true;
}

bool ReadString(const util::JsonValue& json, std::string_view key, std::size_t max_bytes,
                bool required, std::string* out, std::string* error) {
  const util::JsonValue& value = json[key];
  if (value.IsNull() && !required) {
    out->clear();
    return true;
  }
  if (!value.IsString() || value.AsString().size() > max_bytes) {
    *error = std::string(key) + " must be a string of at most " + std::to_string(max_bytes) +
             " bytes";
    return false;
  }
  *out = value.AsString();
  return true;
}

}  // namespace

util::JsonValue ToJson(const HelloRequest& hello) {
  util::JsonObject object;
  object["protocol"] = util::JsonValue(hello.protocol);
  object["min_protocol"] = util::JsonValue(hello.min_protocol);
  object["release"] = util::JsonValue(hello.release);
  object["root"] = util::JsonValue(hello.root);
  return util::JsonValue(std::move(object));
}

util::JsonValue ToJson(const HelloReply& hello) {
  util::JsonObject object;
  object["protocol"] = util::JsonValue(hello.protocol);
  object["min_protocol"] = util::JsonValue(hello.min_protocol);
  object["release"] = util::JsonValue(hello.release);
  object["daemon_epoch"] = util::JsonValue(hello.daemon_epoch);
  util::JsonArray capabilities;
  for (const std::string& capability : hello.capabilities) {
    capabilities.push_back(util::JsonValue(capability));
  }
  object["capabilities"] = util::JsonValue(std::move(capabilities));
  util::JsonObject survival;
  survival["kill_user_processes"] = util::JsonValue(hello.session_survival.kill_user_processes);
  survival["linger"] = util::JsonValue(hello.session_survival.linger);
  object["session_survival"] = util::JsonValue(std::move(survival));
  return util::JsonValue(std::move(object));
}

std::optional<HelloRequest> HelloRequestFromJson(const util::JsonValue& json, std::string* error) {
  if (!json.IsObject()) {
    *error = "hello must be an object";
    return std::nullopt;
  }
  HelloRequest hello;
  if (!ReadVersion(json, "protocol", &hello.protocol, error) ||
      !ReadVersion(json, "min_protocol", &hello.min_protocol, error) ||
      !ReadString(json, "release", kMaxReleaseBytes, true, &hello.release, error) ||
      !ReadString(json, "root", kMaxRootBytes, false, &hello.root, error)) {
    return std::nullopt;
  }
  if (hello.min_protocol > hello.protocol) {
    *error = "min_protocol is above protocol";
    return std::nullopt;
  }
  return hello;
}

std::optional<HelloReply> HelloReplyFromJson(const util::JsonValue& json, std::string* error) {
  if (!json.IsObject()) {
    *error = "hello reply must be an object";
    return std::nullopt;
  }
  HelloReply hello;
  if (!ReadVersion(json, "protocol", &hello.protocol, error) ||
      !ReadVersion(json, "min_protocol", &hello.min_protocol, error) ||
      !ReadString(json, "release", kMaxReleaseBytes, true, &hello.release, error) ||
      !ReadString(json, "daemon_epoch", kMaxEpochBytes, true, &hello.daemon_epoch, error)) {
    return std::nullopt;
  }
  if (hello.min_protocol > hello.protocol) {
    *error = "min_protocol is above protocol";
    return std::nullopt;
  }
  const util::JsonValue& capabilities = json["capabilities"];
  if (!capabilities.IsNull()) {
    if (!capabilities.IsArray() || capabilities.AsArray().size() > kMaxCapabilities) {
      *error = "capabilities must be an array of at most 64 strings";
      return std::nullopt;
    }
    for (const util::JsonValue& capability : capabilities.AsArray()) {
      if (!capability.IsString() || capability.AsString().size() > kMaxCapabilityBytes) {
        *error = "a capability must be a short string";
        return std::nullopt;
      }
      hello.capabilities.push_back(capability.AsString());
    }
  }
  const util::JsonValue& survival = json["session_survival"];
  if (!survival.IsNull()) {
    if (!survival.IsObject()) {
      *error = "session_survival must be an object";
      return std::nullopt;
    }
    hello.session_survival.kill_user_processes = survival["kill_user_processes"].AsBool(false);
    hello.session_survival.linger = survival["linger"].AsBool(false);
  }
  return hello;
}

std::optional<std::string> CheckProtocolCompatibility(std::int64_t local_protocol,
                                                      std::int64_t local_min_protocol,
                                                      std::string_view local_release,
                                                      std::int64_t remote_protocol,
                                                      std::int64_t remote_min_protocol,
                                                      std::string_view remote_release) {
  const auto describe = [](std::string_view release, std::int64_t protocol) {
    return std::string(release.empty() ? std::string_view("unknown") : release) +
           " (protocol " + std::to_string(protocol) + ")";
  };
  if (remote_protocol < local_min_protocol) {
    return "the host's microide-server " + describe(remote_release, remote_protocol) +
           " is too old for this microide " + describe(local_release, local_protocol);
  }
  if (local_protocol < remote_min_protocol) {
    return "this microide " + describe(local_release, local_protocol) +
           " is too old for the host's microide-server " +
           describe(remote_release, remote_protocol);
  }
  return std::nullopt;
}

util::JsonValue ToJson(const Precondition& precondition) {
  switch (precondition.kind) {
    case Precondition::Kind::Absent:
      return util::JsonValue(std::string("absent"));
    case Precondition::Kind::Any:
      return util::JsonValue(std::string("any"));
    case Precondition::Kind::Hash:
      break;
  }
  return util::JsonValue(precondition.hash.Hex());
}

std::optional<Precondition> PreconditionFromJson(const util::JsonValue& json) {
  if (!json.IsString()) {
    return std::nullopt;
  }
  const std::string& text = json.AsString();
  if (text == "absent") {
    return Precondition::NotThere();
  }
  if (text == "any") {
    return Precondition::Anything();
  }
  if (const auto hash = util::ContentHash::FromHex(text)) {
    return Precondition::Of(*hash);
  }
  return std::nullopt;
}

}  // namespace microide::project::remote

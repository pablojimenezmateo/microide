#include "workspace/WorkspacePersistenceBinaryInternal.h"

namespace microide::workspace {

namespace {

bool EncodeBreakpoint(const PersistedBreakpoint& breakpoint, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  return AppendRecord(BreakpointTag::Line,
                      [&](PrimitiveWriter& w) { return WriteSize(w, breakpoint.line); }, out) &&
         AppendRecord(BreakpointTag::Enabled,
                      [&](PrimitiveWriter& w) { return w.WriteBool(breakpoint.enabled); }, out) &&
         AppendRecord(BreakpointTag::Condition,
                      [&](PrimitiveWriter& w) {
                        return w.WriteOptional(breakpoint.condition,
                                               [](PrimitiveWriter& wr, const std::string& value) {
                                                 return wr.WriteString(value);
                                               });
                      },
                      out) &&
         AppendRecord(BreakpointTag::HitCondition,
                      [&](PrimitiveWriter& w) {
                        return w.WriteOptional(breakpoint.hit_condition,
                                               [](PrimitiveWriter& wr, const std::string& value) {
                                                 return wr.WriteString(value);
                                               });
                      },
                      out) &&
         AppendRecord(BreakpointTag::LogMessage,
                      [&](PrimitiveWriter& w) {
                        return w.WriteOptional(breakpoint.log_message,
                                               [](PrimitiveWriter& wr, const std::string& value) {
                                                 return wr.WriteString(value);
                                               });
                      },
                      out);
}

bool DecodeBreakpoint(std::span<const std::byte> input, PersistedBreakpoint* breakpoint) {
  if (breakpoint == nullptr) {
    return false;
  }
  *breakpoint = PersistedBreakpoint{};
  return ParseRecordStream<BreakpointTag>(
      input, [&](BreakpointTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case BreakpointTag::Line:
            return ReadSize(reader, &breakpoint->line) && reader.remaining() == 0;
          case BreakpointTag::Enabled:
            return reader.ReadBool(&breakpoint->enabled) && reader.remaining() == 0;
          case BreakpointTag::Condition:
            return reader.ReadOptional(&breakpoint->condition,
                                       [](PrimitiveReader& r, std::string* value) {
                                         return r.ReadString(value);
                                       }) &&
                   reader.remaining() == 0;
          case BreakpointTag::HitCondition:
            return reader.ReadOptional(&breakpoint->hit_condition,
                                       [](PrimitiveReader& r, std::string* value) {
                                         return r.ReadString(value);
                                       }) &&
                   reader.remaining() == 0;
          case BreakpointTag::LogMessage:
            return reader.ReadOptional(&breakpoint->log_message,
                                       [](PrimitiveReader& r, std::string* value) {
                                         return r.ReadString(value);
                                       }) &&
                   reader.remaining() == 0;
        }
        return true;
      });
}

bool EncodeFileBreakpoints(const PersistedFileBreakpoints& file, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(FileBreakpointsTag::Path,
                    [&](PrimitiveWriter& w) { return w.WritePath(file.path); }, out)) {
    return false;
  }
  for (const PersistedBreakpoint& breakpoint : file.breakpoints) {
    std::vector<std::byte> payload;
    if (!EncodeBreakpoint(breakpoint, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(FileBreakpointsTag::Breakpoint), payload,
                            out)) {
      return false;
    }
  }
  return true;
}

bool DecodeFileBreakpoints(std::span<const std::byte> input, PersistedFileBreakpoints* file) {
  if (file == nullptr) {
    return false;
  }
  *file = PersistedFileBreakpoints{};
  return ParseRecordStream<FileBreakpointsTag>(
      input, [&](FileBreakpointsTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case FileBreakpointsTag::Path:
            return reader.ReadPath(&file->path) && reader.remaining() == 0;
          case FileBreakpointsTag::Breakpoint: {
            PersistedBreakpoint breakpoint;
            if (!DecodeBreakpoint(payload, &breakpoint)) {
              return false;
            }
            file->breakpoints.push_back(std::move(breakpoint));
            return true;
          }
        }
        return true;
      });
}

bool EncodeLaunchConfig(const PersistedLaunchConfig& config, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  return AppendRecord(LaunchConfigTag::Name,
                      [&](PrimitiveWriter& w) { return w.WriteString(config.name); }, out) &&
         AppendRecord(LaunchConfigTag::Type,
                      [&](PrimitiveWriter& w) { return w.WriteString(config.type); }, out) &&
         AppendRecord(LaunchConfigTag::Request,
                      [&](PrimitiveWriter& w) { return w.WriteString(config.request); }, out) &&
         AppendRecord(LaunchConfigTag::ArgumentsJson,
                      [&](PrimitiveWriter& w) { return w.WriteString(config.arguments_json); }, out);
}

bool DecodeLaunchConfig(std::span<const std::byte> input, PersistedLaunchConfig* config) {
  if (config == nullptr) {
    return false;
  }
  *config = PersistedLaunchConfig{};
  return ParseRecordStream<LaunchConfigTag>(
      input, [&](LaunchConfigTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case LaunchConfigTag::Name:
            return reader.ReadString(&config->name) && reader.remaining() == 0;
          case LaunchConfigTag::Type:
            return reader.ReadString(&config->type) && reader.remaining() == 0;
          case LaunchConfigTag::Request:
            return reader.ReadString(&config->request) && reader.remaining() == 0;
          case LaunchConfigTag::ArgumentsJson:
            return reader.ReadString(&config->arguments_json) && reader.remaining() == 0;
        }
        return true;
      });
}

bool EncodeFunctionBreakpoint(const PersistedFunctionBreakpoint& breakpoint,
                              std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  return AppendRecord(FunctionBreakpointTag::Name,
                      [&](PrimitiveWriter& w) { return w.WriteString(breakpoint.name); }, out) &&
         AppendRecord(FunctionBreakpointTag::Enabled,
                      [&](PrimitiveWriter& w) { return w.WriteBool(breakpoint.enabled); }, out) &&
         AppendRecord(FunctionBreakpointTag::Condition,
                      [&](PrimitiveWriter& w) {
                        return w.WriteOptional(breakpoint.condition,
                                               [](PrimitiveWriter& wr, const std::string& value) {
                                                 return wr.WriteString(value);
                                               });
                      },
                      out) &&
         AppendRecord(FunctionBreakpointTag::HitCondition,
                      [&](PrimitiveWriter& w) {
                        return w.WriteOptional(breakpoint.hit_condition,
                                               [](PrimitiveWriter& wr, const std::string& value) {
                                                 return wr.WriteString(value);
                                               });
                      },
                      out);
}

bool DecodeFunctionBreakpoint(std::span<const std::byte> input,
                              PersistedFunctionBreakpoint* breakpoint) {
  if (breakpoint == nullptr) {
    return false;
  }
  *breakpoint = PersistedFunctionBreakpoint{};
  return ParseRecordStream<FunctionBreakpointTag>(
      input, [&](FunctionBreakpointTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case FunctionBreakpointTag::Name:
            return reader.ReadString(&breakpoint->name) && reader.remaining() == 0;
          case FunctionBreakpointTag::Enabled:
            return reader.ReadBool(&breakpoint->enabled) && reader.remaining() == 0;
          case FunctionBreakpointTag::Condition:
            return reader.ReadOptional(&breakpoint->condition,
                                       [](PrimitiveReader& r, std::string* value) {
                                         return r.ReadString(value);
                                       }) &&
                   reader.remaining() == 0;
          case FunctionBreakpointTag::HitCondition:
            return reader.ReadOptional(&breakpoint->hit_condition,
                                       [](PrimitiveReader& r, std::string* value) {
                                         return r.ReadString(value);
                                       }) &&
                   reader.remaining() == 0;
        }
        return true;
      });
}

bool EncodeExceptionFilterCondition(const std::string& filter_id, const std::string& condition,
                                    std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  return AppendRecord(ExceptionFilterConditionTag::FilterId,
                      [&](PrimitiveWriter& w) { return w.WriteString(filter_id); }, out) &&
         AppendRecord(ExceptionFilterConditionTag::Condition,
                      [&](PrimitiveWriter& w) { return w.WriteString(condition); }, out);
}

bool DecodeExceptionFilterCondition(std::span<const std::byte> input, std::string* filter_id,
                                    std::string* condition) {
  if (filter_id == nullptr || condition == nullptr) {
    return false;
  }
  filter_id->clear();
  condition->clear();
  return ParseRecordStream<ExceptionFilterConditionTag>(
      input, [&](ExceptionFilterConditionTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case ExceptionFilterConditionTag::FilterId:
            return reader.ReadString(filter_id) && reader.remaining() == 0;
          case ExceptionFilterConditionTag::Condition:
            return reader.ReadString(condition) && reader.remaining() == 0;
        }
        return true;
      });
}

}  // namespace

bool EncodeDebugStateRecord(const PersistedDebugState& state, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(DebugStateTag::Schema,
                    [&](PrimitiveWriter& w) { return w.WriteU32(kSchemaVersion); }, out) ||
      !AppendRecord(DebugStateTag::SelectedLaunchConfigIndex,
                    [&](PrimitiveWriter& w) {
                      return WriteSize(w, state.selected_launch_config_index);
                    },
                    out)) {
    return false;
  }
  for (const PersistedFileBreakpoints& file : state.files) {
    std::vector<std::byte> payload;
    if (!EncodeFileBreakpoints(file, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(DebugStateTag::FileBreakpoints), payload,
                            out)) {
      return false;
    }
  }
  for (const PersistedLaunchConfig& config : state.launch_configs) {
    std::vector<std::byte> payload;
    if (!EncodeLaunchConfig(config, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(DebugStateTag::LaunchConfig), payload, out)) {
      return false;
    }
  }
  for (const std::string& expression : state.watch_expressions) {
    if (!AppendRecord(DebugStateTag::WatchExpression,
                      [&](PrimitiveWriter& w) { return w.WriteString(expression); }, out)) {
      return false;
    }
  }
  for (const std::string& filter_id : state.enabled_exception_filters) {
    if (!AppendRecord(DebugStateTag::ExceptionFilter,
                      [&](PrimitiveWriter& w) { return w.WriteString(filter_id); }, out)) {
      return false;
    }
  }
  if (state.exception_filters_seeded &&
      !AppendRecord(DebugStateTag::ExceptionFiltersSeeded,
                    [&](PrimitiveWriter& w) { return w.WriteBool(true); }, out)) {
    return false;
  }
  for (const PersistedFunctionBreakpoint& breakpoint : state.function_breakpoints) {
    std::vector<std::byte> payload;
    if (!EncodeFunctionBreakpoint(breakpoint, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(DebugStateTag::FunctionBreakpoint), payload,
                            out)) {
      return false;
    }
  }
  for (const auto& [filter_id, condition] : state.exception_filter_conditions) {
    std::vector<std::byte> payload;
    if (!EncodeExceptionFilterCondition(filter_id, condition, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(DebugStateTag::ExceptionFilterCondition),
                            payload, out)) {
      return false;
    }
  }
  return true;
}

bool DecodeDebugStateRecord(std::span<const std::byte> input, PersistedDebugState* state) {
  if (state == nullptr) {
    return false;
  }
  *state = PersistedDebugState{};
  bool seen_schema = false;
  return ParseRecordStream<DebugStateTag>(
             input, [&](DebugStateTag tag, std::span<const std::byte> payload) {
               PrimitiveReader reader(payload);
               switch (tag) {
                 case DebugStateTag::Schema: {
                   std::uint32_t schema = 0;
                   if (!reader.ReadU32(&schema) || reader.remaining() != 0 ||
                       schema != kSchemaVersion) {
                     return false;
                   }
                   seen_schema = true;
                   return true;
                 }
                 case DebugStateTag::SelectedLaunchConfigIndex:
                   return ReadSize(reader, &state->selected_launch_config_index) &&
                          reader.remaining() == 0;
                 case DebugStateTag::FileBreakpoints: {
                   PersistedFileBreakpoints file;
                   if (!DecodeFileBreakpoints(payload, &file)) {
                     return false;
                   }
                   state->files.push_back(std::move(file));
                   return true;
                 }
                 case DebugStateTag::LaunchConfig: {
                   PersistedLaunchConfig config;
                   if (!DecodeLaunchConfig(payload, &config)) {
                     return false;
                   }
                   state->launch_configs.push_back(std::move(config));
                   return true;
                 }
                 case DebugStateTag::WatchExpression: {
                   std::string expression;
                   if (!reader.ReadString(&expression) || reader.remaining() != 0) {
                     return false;
                   }
                   state->watch_expressions.push_back(std::move(expression));
                   return true;
                 }
                 case DebugStateTag::ExceptionFilter: {
                   std::string filter_id;
                   if (!reader.ReadString(&filter_id) || reader.remaining() != 0) {
                     return false;
                   }
                   state->enabled_exception_filters.push_back(std::move(filter_id));
                   return true;
                 }
                 case DebugStateTag::ExceptionFiltersSeeded:
                   return reader.ReadBool(&state->exception_filters_seeded) &&
                          reader.remaining() == 0;
                 case DebugStateTag::FunctionBreakpoint: {
                   PersistedFunctionBreakpoint breakpoint;
                   if (!DecodeFunctionBreakpoint(payload, &breakpoint)) {
                     return false;
                   }
                   state->function_breakpoints.push_back(std::move(breakpoint));
                   return true;
                 }
                 case DebugStateTag::ExceptionFilterCondition: {
                   std::string filter_id;
                   std::string condition;
                   if (!DecodeExceptionFilterCondition(payload, &filter_id, &condition)) {
                     return false;
                   }
                   state->exception_filter_conditions[std::move(filter_id)] = std::move(condition);
                   return true;
                 }
               }
               return true;
             }) &&
         seen_schema;
}

}  // namespace microide::workspace

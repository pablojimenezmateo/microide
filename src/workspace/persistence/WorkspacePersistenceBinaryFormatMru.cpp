#include "workspace/persistence/WorkspacePersistenceBinaryInternal.h"
#include "workspace/persistence/RecentsService.h"

namespace microide::workspace {

namespace {

bool EncodeRecentFile(const PersistedRecentFile& file, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  return AppendRecord(RecentFileEntryTag::Path,
                      [&](PrimitiveWriter& w) { return w.WritePath(file.path); }, out) &&
         AppendRecord(RecentFileEntryTag::ProjectRoot,
                      [&](PrimitiveWriter& w) { return w.WritePath(file.project_root); }, out);
}

bool DecodeRecentFile(std::span<const std::byte> input, PersistedRecentFile* file) {
  if (file == nullptr) {
    return false;
  }
  *file = PersistedRecentFile{};
  return ParseRecordStream<RecentFileEntryTag>(
      input, [&](RecentFileEntryTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case RecentFileEntryTag::Path:
            return reader.ReadPath(&file->path) && reader.remaining() == 0;
          case RecentFileEntryTag::ProjectRoot:
            return reader.ReadPath(&file->project_root) && reader.remaining() == 0;
        }
        return true;
      });
}

}  // namespace

bool EncodeMruRecord(const PersistedMruState& state, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(MruStateTag::Schema,
                    [&](PrimitiveWriter& w) { return w.WriteU32(kSchemaVersion); }, out)) {
    return false;
  }
  for (const std::filesystem::path& root : state.recent_project_roots) {
    if (!AppendRecord(MruStateTag::RecentProjectRoot,
                      [&](PrimitiveWriter& w) { return w.WritePath(root); }, out)) {
      return false;
    }
  }
  for (const PersistedRecentFile& file : state.recent_files) {
    std::vector<std::byte> payload;
    if (!EncodeRecentFile(file, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(MruStateTag::RecentFile), payload, out)) {
      return false;
    }
  }
  return true;
}

bool DecodeMruRecord(std::span<const std::byte> input, PersistedMruState* state) {
  if (state == nullptr) {
    return false;
  }
  *state = PersistedMruState{};
  bool seen_schema = false;
  return ParseRecordStream<MruStateTag>(
             input, [&](MruStateTag tag, std::span<const std::byte> payload) {
               PrimitiveReader reader(payload);
               switch (tag) {
                 case MruStateTag::Schema: {
                   std::uint32_t schema = 0;
                   if (!reader.ReadU32(&schema) || reader.remaining() != 0 ||
                       schema != kSchemaVersion) {
                     return false;
                   }
                   seen_schema = true;
                   return true;
                 }
                 case MruStateTag::RecentProjectRoot: {
                   std::filesystem::path root;
                   if (!reader.ReadPath(&root) || reader.remaining() != 0) {
                     return false;
                   }
                   // Records are newest-first and RecentsService truncates to the
                   // front on load; drop anything past the cap here so a malformed
                   // file cannot make the decoder allocate entries that will be
                   // discarded immediately.
                   if (state->recent_project_roots.size() >= RecentsService::MaxProjects()) {
                     return true;
                   }
                   state->recent_project_roots.push_back(std::move(root));
                   return true;
                 }
                 case MruStateTag::RecentFile: {
                   if (state->recent_files.size() >= RecentsService::MaxFiles()) {
                     return true;
                   }
                   PersistedRecentFile file;
                   if (!DecodeRecentFile(payload, &file)) {
                     return false;
                   }
                   state->recent_files.push_back(std::move(file));
                   return true;
                 }
               }
               return true;
             }) &&
         seen_schema;
}

}  // namespace microide::workspace

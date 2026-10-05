#pragma once

#include <span>

#include "project/FileWriteGate.h"

namespace microide::project {

// The local gate's tree operations — LocalFileWriteGate().ApplyTreeOps. Kept out
// of FileWriteGate.cpp because the journal is most of the code; nothing outside
// the gate may call it (CheckProjectWritesGoThroughTheWriteGate).
FileWriteGate::TreeResult ApplyLocalTreeOps(std::span<const FileWriteGate::TreeOp> ops);

}  // namespace microide::project

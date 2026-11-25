#pragma once

#include <span>

#include "project/FileWriteGate.h"

namespace microide::project {

FileWriteGate::TreeResult ApplyLocalTreeOps(std::span<const FileWriteGate::TreeOp> ops);

}

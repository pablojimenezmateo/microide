#pragma once

#include "project/FileReadService.h"
#include "project/FileWriteService.h"

namespace microide::project {

struct FileIoServices {
  FileReadService reads;
  FileWriteService writes;
};

}

#include "project/ProjectBackgroundExecutor.h"

#include "util/BackgroundTaskCounter.h"

namespace microide::project {

ProjectBackgroundExecutor::ProjectBackgroundExecutor()
    : queue_(util::SerialWorkQueue::StartMode::kEager,
             util::SerialWorkQueue::Hooks{
                 .on_enqueue = []() { util::IncrementBackgroundTaskCount(); },
                 .on_complete = []() { util::DecrementBackgroundTaskCountAndWake(); },
             }) {}

}  // namespace microide::project

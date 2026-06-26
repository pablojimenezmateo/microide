#include "project/ProjectBackgroundExecutor.h"

#include "app/BackgroundTaskCounter.h"

namespace microide::project {

ProjectBackgroundExecutor::ProjectBackgroundExecutor()
    : queue_(util::SerialWorkQueue::StartMode::kEager,
             util::SerialWorkQueue::Hooks{
                 .on_enqueue = []() { app::IncrementBackgroundTaskCount(); },
                 .on_complete = []() { app::DecrementBackgroundTaskCountAndWake(); },
             }) {}

}  // namespace microide::project

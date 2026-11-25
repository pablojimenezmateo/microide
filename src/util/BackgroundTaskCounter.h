#pragma once

#include "util/Waker.h"

namespace microide::util {

void IncrementBackgroundTaskCount();
void DecrementBackgroundTaskCountAndWake();
int GetBackgroundTaskCount();

void SetBackgroundTaskWakeChannel(WakeChannel channel);

}

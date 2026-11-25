#include "plugin/PluginHostInternal.h"
#include "util/ErrorOut.h"

namespace microide::plugin {

thread_local PluginExecContext g_exec;

PluginHost::PluginHost() : impl_(std::make_unique<Impl>()) {}

PluginHost::~PluginHost() {
  Shutdown();
}

PluginHost::PluginHost(PluginHost&& other) noexcept = default;

PluginHost& PluginHost::operator=(PluginHost&& other) noexcept = default;

void PluginHost::SetCallbacks(Callbacks callbacks) {
  impl_->raw_callbacks = std::move(callbacks);
  impl_->callbacks = impl_->BuildRoutedCallbacks();
}

void PluginHost::SetWorker(PluginThread* worker) {
  impl_->worker_ = worker;
}

#include "plugin/PluginHostPublicApi.inc"

#include "fastfix/runtime/shard_poller.h"

namespace fastfix::runtime {

auto
ShardPoller::OpenWakeup() -> base::Status
{
  return wakeup_.Open();
}

auto
ShardPoller::CloseWakeup() -> void
{
  wakeup_.Close();
}

auto
ShardPoller::SignalWakeup() const -> void
{
  wakeup_.Signal();
}

auto
ShardPoller::DrainWakeup() -> void
{
  wakeup_.Drain();
}

auto
ShardPoller::InitBackend(IoBackend backend) -> base::Status
{
  io_poller_ = CreateIoPoller(backend);
  if (io_poller_ == nullptr) {
    return base::Status::InvalidArgument("unsupported io backend");
  }
  auto status = io_poller_->Init();
  if (!status.ok())
    return status;

  // Register wakeup fd with sentinel tag.
  if (wakeup_.valid()) {
    status = io_poller_->AddFd(wakeup_.read_fd(), kWakeupTag);
    if (!status.ok())
      return status;
  }
  return base::Status::Ok();
}

} // namespace fastfix::runtime
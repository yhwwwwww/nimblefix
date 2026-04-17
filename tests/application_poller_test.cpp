#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fastfix/runtime/application.h"

#include "test_support.h"

namespace {

auto
MakeEvent(std::uint64_t session_id, std::uint32_t worker_id, std::string text) -> fastfix::runtime::RuntimeEvent
{
  return fastfix::runtime::RuntimeEvent{
    .kind = fastfix::runtime::RuntimeEventKind::kApplicationMessage,
    .session_event = fastfix::runtime::SessionEventKind::kBound,
    .handle = fastfix::session::SessionHandle(session_id, worker_id),
    .session_key = fastfix::session::SessionKey{ "FIX.4.4", "BUY", "SELL" },
    .message = {},
    .text = std::move(text),
    .timestamp_ns = session_id,
  };
}

class CapturingHandler final : public fastfix::runtime::QueueApplicationEventHandler
{
public:
  auto OnRuntimeEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override
  {
    std::lock_guard lock(mutex_);
    seen_texts_.push_back(event.text);
    seen_workers_.push_back(event.handle.worker_id());
    return fastfix::base::Status::Ok();
  }

  [[nodiscard]] auto seen_texts() const -> std::vector<std::string>
  {
    std::lock_guard lock(mutex_);
    return seen_texts_;
  }

  [[nodiscard]] auto seen_workers() const -> std::vector<std::uint32_t>
  {
    std::lock_guard lock(mutex_);
    return seen_workers_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::string> seen_texts_;
  std::vector<std::uint32_t> seen_workers_;
};

struct SharedCaptureState
{
  mutable std::mutex mutex;
  std::vector<std::string> texts;
  std::vector<std::uint32_t> workers;
};

class SharedCapturingHandler final : public fastfix::runtime::QueueApplicationEventHandler
{
public:
  explicit SharedCapturingHandler(std::shared_ptr<SharedCaptureState> state)
    : state_(std::move(state))
  {
  }

  auto OnRuntimeEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override
  {
    std::lock_guard lock(state_->mutex);
    state_->texts.push_back(event.text);
    state_->workers.push_back(event.handle.worker_id());
    return fastfix::base::Status::Ok();
  }

private:
  std::shared_ptr<SharedCaptureState> state_;
};

class AffinityCapturingHandler final : public fastfix::runtime::QueueApplicationEventHandler
{
public:
  explicit AffinityCapturingHandler(std::uint32_t expected_worker_id)
    : expected_worker_id_(expected_worker_id)
  {
  }

  auto OnRuntimeEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override
  {
    std::lock_guard lock(mutex_);
    seen_workers_.push_back(event.handle.worker_id());
    seen_thread_ids_.push_back(std::this_thread::get_id());
    if (event.handle.worker_id() != expected_worker_id_) {
      return fastfix::base::Status::InvalidArgument("queue application runner delivered an event to the wrong worker "
                                                    "handler");
    }
    return fastfix::base::Status::Ok();
  }

  [[nodiscard]] auto seen_workers() const -> std::vector<std::uint32_t>
  {
    std::lock_guard lock(mutex_);
    return seen_workers_;
  }

  [[nodiscard]] auto seen_thread_ids() const -> std::vector<std::thread::id>
  {
    std::lock_guard lock(mutex_);
    return seen_thread_ids_;
  }

  [[nodiscard]] auto event_count() const -> std::size_t
  {
    std::lock_guard lock(mutex_);
    return seen_workers_.size();
  }

private:
  std::uint32_t expected_worker_id_{ 0U };
  mutable std::mutex mutex_;
  std::vector<std::uint32_t> seen_workers_;
  std::vector<std::thread::id> seen_thread_ids_;
};

} // namespace

TEST_CASE("application-poller", "[application-poller]")
{
  fastfix::runtime::QueueApplication queue(2U);
  CapturingHandler handler;
  fastfix::runtime::QueueApplicationPoller poller(&queue,
                                                  &handler,
                                                  fastfix::runtime::QueueApplicationPollerOptions{
                                                    .max_events_per_poll = 1U,
                                                    .yield_when_idle = false,
                                                  });

  REQUIRE(queue.OnAppMessage(MakeEvent(1001U, 0U, "w0-first")).ok());
  REQUIRE(queue.OnAppMessage(MakeEvent(1002U, 0U, "w0-second")).ok());
  REQUIRE(queue.OnAppMessage(MakeEvent(2001U, 1U, "w1-first")).ok());

  auto drained = poller.PollWorkerOnce(0U);
  REQUIRE(drained.ok());
  REQUIRE(drained.value() == 1U);

  auto texts = handler.seen_texts();
  REQUIRE(texts.size() == 1U);
  REQUIRE(texts[0] == "w0-first");

  auto all_workers = poller.PollAllOnce();
  REQUIRE(!all_workers.ok());
  REQUIRE(all_workers.status().code() == fastfix::base::ErrorCode::kInvalidArgument);
  REQUIRE(all_workers.status().message().find("single-worker applications") != std::string::npos);

  drained = poller.PollWorkerOnce(0U);
  REQUIRE(drained.ok());
  REQUIRE(drained.value() == 1U);

  drained = poller.PollWorkerOnce(1U);
  REQUIRE(drained.ok());
  REQUIRE(drained.value() == 1U);

  texts = handler.seen_texts();
  REQUIRE(texts.size() == 3U);
  REQUIRE(texts[1] == "w0-second");
  REQUIRE(texts[2] == "w1-first");

  auto workers = handler.seen_workers();
  REQUIRE(workers.size() == 3U);
  REQUIRE(workers[0] == 0U);
  REQUIRE(workers[1] == 0U);
  REQUIRE(workers[2] == 1U);

  REQUIRE(queue.OnAppMessage(MakeEvent(3000U, 1U, "run-all-denied")).ok());
  std::atomic<bool> stop_requested{ false };
  auto run_all_status = poller.RunAll(stop_requested);
  REQUIRE(!run_all_status.ok());
  REQUIRE(run_all_status.code() == fastfix::base::ErrorCode::kInvalidArgument);

  drained = poller.PollWorkerOnce(1U);
  REQUIRE(drained.ok());
  REQUIRE(drained.value() == 1U);

  REQUIRE(queue.OnAppMessage(MakeEvent(3001U, 1U, "run-worker")).ok());
  auto worker_future = std::async(std::launch::async, [&]() { return poller.RunWorker(1U, stop_requested); });

  while (handler.seen_texts().size() < 5U) {
    std::this_thread::yield();
  }
  stop_requested.store(true);
  REQUIRE(worker_future.get().ok());

  texts = handler.seen_texts();
  REQUIRE(texts[3] == "run-all-denied");
  REQUIRE(texts[4] == "run-worker");

  fastfix::runtime::QueueApplication single_worker_queue(1U);
  CapturingHandler single_worker_handler;
  fastfix::runtime::QueueApplicationPoller single_worker_poller(&single_worker_queue,
                                                                &single_worker_handler,
                                                                fastfix::runtime::QueueApplicationPollerOptions{
                                                                  .max_events_per_poll = 8U,
                                                                  .yield_when_idle = false,
                                                                });

  REQUIRE(single_worker_queue.OnAppMessage(MakeEvent(3501U, 0U, "single-all-1")).ok());
  REQUIRE(single_worker_queue.OnAppMessage(MakeEvent(3502U, 0U, "single-all-2")).ok());
  drained = single_worker_poller.PollAllOnce();
  REQUIRE(drained.ok());
  REQUIRE(drained.value() == 2U);

  std::atomic<bool> single_stop_requested{ false };
  REQUIRE(single_worker_queue.OnAppMessage(MakeEvent(3503U, 0U, "single-run-all")).ok());
  auto single_worker_future =
    std::async(std::launch::async, [&]() { return single_worker_poller.RunAll(single_stop_requested); });

  while (single_worker_handler.seen_texts().size() < 3U) {
    std::this_thread::yield();
  }
  single_stop_requested.store(true);
  REQUIRE(single_worker_future.get().ok());

  auto single_worker_texts = single_worker_handler.seen_texts();
  REQUIRE(single_worker_texts.size() == 3U);
  REQUIRE(single_worker_texts[0] == "single-all-1");
  REQUIRE(single_worker_texts[1] == "single-all-2");
  REQUIRE(single_worker_texts[2] == "single-run-all");

  auto shared_state = std::make_shared<SharedCaptureState>();
  std::vector<std::unique_ptr<fastfix::runtime::QueueApplicationEventHandler>> handlers;
  handlers.push_back(std::make_unique<SharedCapturingHandler>(shared_state));
  handlers.push_back(std::make_unique<SharedCapturingHandler>(shared_state));

  fastfix::runtime::QueueApplication runner_queue(2U);
  fastfix::runtime::QueueApplicationRunner runner(&runner_queue,
                                                  std::move(handlers),
                                                  fastfix::runtime::QueueApplicationPollerOptions{
                                                    .max_events_per_poll = 8U,
                                                    .yield_when_idle = true,
                                                  });
  REQUIRE(runner.Start().ok());
  REQUIRE(runner.running());

  REQUIRE(runner_queue.OnAppMessage(MakeEvent(4001U, 0U, "runner-w0")).ok());
  REQUIRE(runner_queue.OnAppMessage(MakeEvent(4002U, 1U, "runner-w1")).ok());

  for (std::size_t spin = 0; spin < 100000U; ++spin) {
    bool complete = false;
    {
      std::lock_guard lock(shared_state->mutex);
      complete = shared_state->texts.size() >= 2U;
    }
    if (complete) {
      break;
    }
    std::this_thread::yield();
  }

  REQUIRE(runner.Stop().ok());
  REQUIRE(!runner.running());

  {
    std::lock_guard lock(shared_state->mutex);
    REQUIRE(shared_state->texts.size() == 2U);
    REQUIRE(shared_state->workers.size() == 2U);
  }

  std::vector<AffinityCapturingHandler*> affinity_handler_ptrs;
  std::vector<std::unique_ptr<fastfix::runtime::QueueApplicationEventHandler>> affinity_handlers;
  affinity_handlers.reserve(2U);
  for (std::uint32_t worker_id = 0U; worker_id < 2U; ++worker_id) {
    auto handler_ptr = std::make_unique<AffinityCapturingHandler>(worker_id);
    affinity_handler_ptrs.push_back(handler_ptr.get());
    affinity_handlers.push_back(std::move(handler_ptr));
  }

  fastfix::runtime::QueueApplication affinity_queue(2U);
  fastfix::runtime::QueueApplicationRunner affinity_runner(&affinity_queue,
                                                           std::move(affinity_handlers),
                                                           fastfix::runtime::QueueApplicationPollerOptions{
                                                             .max_events_per_poll = 8U,
                                                             .yield_when_idle = true,
                                                           },
                                                           fastfix::runtime::QueueApplicationRunnerThreadOptions{
                                                             .cpu_affinity = {},
                                                             .thread_name_prefix = "ff-app-test-w",
                                                           });
  REQUIRE(affinity_runner.Start().ok());

  REQUIRE(affinity_queue.OnAppMessage(MakeEvent(5001U, 0U, "affinity-w0-first")).ok());
  REQUIRE(affinity_queue.OnAppMessage(MakeEvent(5002U, 1U, "affinity-w1-first")).ok());
  REQUIRE(affinity_queue.OnAppMessage(MakeEvent(5003U, 0U, "affinity-w0-second")).ok());
  REQUIRE(affinity_queue.OnAppMessage(MakeEvent(5004U, 1U, "affinity-w1-second")).ok());

  for (std::size_t spin = 0; spin < 100000U; ++spin) {
    if (affinity_handler_ptrs[0]->event_count() >= 2U && affinity_handler_ptrs[1]->event_count() >= 2U) {
      break;
    }
    std::this_thread::yield();
  }

  REQUIRE(affinity_runner.Stop().ok());
  const auto worker0_seen = affinity_handler_ptrs[0]->seen_workers();
  const auto worker1_seen = affinity_handler_ptrs[1]->seen_workers();
  REQUIRE(worker0_seen.size() == 2U);
  REQUIRE(worker1_seen.size() == 2U);
  REQUIRE(worker0_seen[0] == 0U);
  REQUIRE(worker0_seen[1] == 0U);
  REQUIRE(worker1_seen[0] == 1U);
  REQUIRE(worker1_seen[1] == 1U);

  const auto worker0_threads = affinity_handler_ptrs[0]->seen_thread_ids();
  const auto worker1_threads = affinity_handler_ptrs[1]->seen_thread_ids();
  REQUIRE(worker0_threads.size() == 2U);
  REQUIRE(worker1_threads.size() == 2U);
  REQUIRE(worker0_threads[0] == worker0_threads[1]);
  REQUIRE(worker1_threads[0] == worker1_threads[1]);
  REQUIRE(worker0_threads[0] != std::thread::id{});
  REQUIRE(worker1_threads[0] != std::thread::id{});
  REQUIRE(worker0_threads[0] != worker1_threads[0]);
}
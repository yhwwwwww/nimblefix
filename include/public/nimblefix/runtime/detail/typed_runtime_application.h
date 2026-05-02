#pragma once

#include <memory>
#include <utility>

#include "nimblefix/advanced/runtime_application.h"
#include "nimblefix/base/status.h"
#include "nimblefix/runtime/application.h"
#include "nimblefix/runtime/session.h"

namespace nimble::runtime {

template<class Profile>
class ProfileBinding;

} // namespace nimble::runtime

namespace nimble::runtime::detail {

template<class Profile, class ApplicationType>
class TypedRuntimeApplication final : public ApplicationCallbacks
{
public:
  TypedRuntimeApplication(nimble::runtime::ProfileBinding<Profile>* binding,
                          std::shared_ptr<ApplicationType> application)
    : binding_(binding)
    , application_(std::move(application))
  {
  }

  auto OnSessionEvent(const RuntimeEvent& event) -> base::Status override
  {
    if (application_ == nullptr) {
      return base::Status::InvalidArgument("application is null");
    }

    Session<Profile> session(event.handle);
    switch (event.session_event) {
      case SessionEventKind::kBound:
        return application_->OnSessionBound(session);
      case SessionEventKind::kActive:
        return application_->OnSessionActive(session);
      case SessionEventKind::kClosed:
        return application_->OnSessionClosed(session, event.text);
    }
    return base::Status::Ok();
  }

  auto OnAdminMessage(const RuntimeEvent& event) -> base::Status override { return Dispatch(event); }
  auto OnAppMessage(const RuntimeEvent& event) -> base::Status override { return Dispatch(event); }

private:
  auto Dispatch(const RuntimeEvent& event) -> base::Status
  {
    if (application_ == nullptr) {
      return base::Status::InvalidArgument("application is null");
    }
    if (binding_ == nullptr) {
      return base::Status::InvalidArgument("profile binding is null");
    }

    InlineSession<Profile> session(event.handle, event.is_warmup);
    return binding_->dispatcher().Dispatch(session, event.message_view(), *application_);
  }

  nimble::runtime::ProfileBinding<Profile>* binding_{ nullptr };
  std::shared_ptr<ApplicationType> application_;
};

} // namespace nimble::runtime::detail

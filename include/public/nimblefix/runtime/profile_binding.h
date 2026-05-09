#pragma once

#include <cstdint>
#include <utility>

#include "nimblefix/generated/detail/message_shape.h"
#include "nimblefix/profile/normalized_dictionary.h"

namespace nimble::runtime {

template<class Profile>
class ProfileBinding
{
public:
  explicit ProfileBinding(profile::NormalizedDictionaryView dictionary,
                          const generated::detail::MessageShape* const* usage_shapes = nullptr,
                          std::uint32_t usage_shape_count = 0U)
    : dictionary_(std::move(dictionary))
    , dispatcher_(MakeDispatcher(usage_shapes, usage_shape_count))
  {
  }

  [[nodiscard]] auto dictionary() const -> const profile::NormalizedDictionaryView& { return dictionary_; }
  [[nodiscard]] auto dispatcher() const -> const typename Profile::Dispatcher& { return dispatcher_; }
  [[nodiscard]] auto profile_id() const -> std::uint64_t { return Profile::kProfileId; }
  [[nodiscard]] auto schema_hash() const -> std::uint64_t { return Profile::kSchemaHash; }

private:
  static auto MakeDispatcher(const generated::detail::MessageShape* const* usage_shapes,
                             std::uint32_t usage_shape_count) -> typename Profile::Dispatcher
  {
    if constexpr (requires { typename Profile::Dispatcher(usage_shapes, usage_shape_count); }) {
      return typename Profile::Dispatcher(usage_shapes, usage_shape_count);
    } else {
      (void)usage_shapes;
      (void)usage_shape_count;
      return typename Profile::Dispatcher{};
    }
  }

  profile::NormalizedDictionaryView dictionary_;
  typename Profile::Dispatcher dispatcher_ = MakeDispatcher(nullptr, 0U);
};

} // namespace nimble::runtime

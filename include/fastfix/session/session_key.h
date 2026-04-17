#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace fastfix::session {

struct SessionKey
{
  std::string begin_string;
  std::string sender_comp_id;
  std::string target_comp_id;
  std::optional<std::string> sender_location_id;
  std::optional<std::string> target_location_id;
  std::optional<std::string> session_qualifier;
};

struct SessionKeyHash
{
  [[nodiscard]] auto operator()(const SessionKey& key) const -> std::size_t;
};

} // namespace fastfix::session

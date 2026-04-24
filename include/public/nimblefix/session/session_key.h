#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace nimble::session {

struct SessionKey
{
  // Transport BeginString(8). For initiators this is the outbound value; for
  // acceptor matching and SessionFactory callbacks it is normalized to the
  // local engine's perspective.
  std::string begin_string;
  // Local CompID from this engine's perspective.
  std::string sender_comp_id;
  // Remote counterparty CompID from this engine's perspective.
  std::string target_comp_id;
  // Optional location identifiers carried in SenderLocationID(142) /
  // TargetLocationID(143).
  std::optional<std::string> sender_location_id;
  std::optional<std::string> target_location_id;
  // Optional FIX session qualifier when multiple logical sessions share the
  // same CompIDs.
  std::optional<std::string> session_qualifier;
};

struct SessionKeyHash
{
  [[nodiscard]] auto operator()(const SessionKey& key) const -> std::size_t;
};

} // namespace nimble::session

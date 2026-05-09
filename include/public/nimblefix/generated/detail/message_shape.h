#pragma once

#include <cstdint>
#include <string_view>

namespace nimble::generated::detail {

enum class ScalarKind : std::uint8_t
{
  kString,
  kInt,
  kChar,
  kFloat,
  kBool,
};

enum class Presence : std::uint8_t
{
  kAlways,
  kOptional,
};

struct BodyNode
{
  enum class Kind : std::uint8_t
  {
    kScalar,
    kGroup,
  };

  Kind kind{};
  std::uint32_t tag{};
  ScalarKind scalar_kind{};
  Presence presence{};
  std::uint32_t delimiter_tag{};
  const BodyNode* entry_data{};
  std::uint32_t entry_count{};

  constexpr BodyNode() = default;

  constexpr BodyNode(Kind node_kind,
                     std::uint32_t node_tag,
                     ScalarKind node_scalar_kind,
                     Presence node_presence,
                     std::uint32_t node_delimiter_tag,
                     const BodyNode* node_entry_data,
                     std::uint32_t node_entry_count)
    : kind(node_kind)
    , tag(node_tag)
    , scalar_kind(node_scalar_kind)
    , presence(node_presence)
    , delimiter_tag(node_delimiter_tag)
    , entry_data(node_entry_data)
    , entry_count(node_entry_count)
  {
  }

  [[nodiscard]] constexpr auto is_scalar() const -> bool { return kind == Kind::kScalar; }
  [[nodiscard]] constexpr auto is_group() const -> bool { return kind == Kind::kGroup; }
};

struct RawStaticField
{
  std::uint32_t tag{};
  ScalarKind kind{};
  Presence presence{};
};

struct MessageShape
{
  std::uint64_t schema_hash{};
  std::string_view msg_type{};
  const BodyNode* body_data{};
  std::uint32_t body_count{};
  const RawStaticField* raw_extras_data{};
  std::uint32_t raw_extras_count{};
};

} // namespace nimble::generated::detail

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace nimble::session {

inline constexpr std::size_t kEncodedFrameInlineCapacity = 768U;

/// Storage for one fully finalized outbound FIX frame.
///
/// Design intent: keep the common case inline and allow replay paths to splice
/// a separately stored application body between a copied header and trailer.
///
/// Boundary condition: when `external_body` is non-empty, `view()` returns only
/// the owned contiguous prefix/suffix buffer. The full wire payload is the
/// splice described by `body_splice_offset`.
struct EncodedFrameBytes
{
  std::array<std::byte, kEncodedFrameInlineCapacity> inline_storage{};
  std::size_t inline_size{ 0U };
  std::vector<std::byte> overflow_storage;

  // Scatter-gather replay support: when external_body is non-empty, the owned
  // buffer contains [header | trailer] and external_body is spliced between
  // them on the wire. Wire order: buf[0..body_splice_offset] + external_body +
  // buf[body_splice_offset..end].
  std::span<const std::byte> external_body;
  std::size_t body_splice_offset{ 0U };

  /// Copy a contiguous finalized frame into local storage.
  ///
  /// Any prior replay splice state is cleared.
  ///
  /// \param bytes Contiguous frame bytes.
  auto assign(std::span<const std::byte> bytes) -> void
  {
    external_body = {};
    body_splice_offset = 0U;
    if (bytes.size() <= kEncodedFrameInlineCapacity) {
      inline_size = bytes.size();
      overflow_storage.clear();
      std::copy(bytes.begin(), bytes.end(), inline_storage.begin());
      return;
    }

    inline_size = 0U;
    overflow_storage.assign(bytes.begin(), bytes.end());
  }

  /// Return the contiguous owned portion of the frame.
  ///
  /// When `external_body` is set, this omits the spliced body bytes.
  ///
  /// \return Borrowed span over inline or overflow storage.
  [[nodiscard]] auto view() const -> std::span<const std::byte>
  {
    if (!overflow_storage.empty()) {
      return std::span<const std::byte>(overflow_storage.data(), overflow_storage.size());
    }
    return std::span<const std::byte>(inline_storage.data(), inline_size);
  }

  /// Return the full wire size including any spliced external body.
  [[nodiscard]] auto size() const -> std::size_t
  {
    return (overflow_storage.empty() ? inline_size : overflow_storage.size()) + external_body.size();
  }

  [[nodiscard]] auto empty() const -> bool { return size() == 0U; }

  operator std::span<const std::byte>() const { return view(); }
};

/// One finalized outbound FIX frame plus lightweight routing metadata.
struct EncodedFrame
{
  EncodedFrameBytes bytes;
  std::string msg_type;
  bool admin{ false };
};

} // namespace nimble::session

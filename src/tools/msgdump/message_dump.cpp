#include "nimblefix/tools/message_dump.h"

#include <charconv>
#include <cstddef>
#include <limits>
#include <string>

#include "nimblefix/codec/fix_tags.h"

namespace nimble::tools {

namespace {

using nimble::codec::tags::kMsgSeqNum;
using nimble::codec::tags::kMsgType;

constexpr char kSoh = '\x01';
constexpr char kReadableDelimiter = '|';
constexpr char kJsonHexDigits[] = "0123456789abcdef";

struct FixFieldView
{
  std::uint32_t tag{ 0 };
  std::string_view value;
};

[[nodiscard]] auto
TagName(std::uint32_t tag) -> std::string_view
{
  switch (tag) {
    case 1U:
      return "Account";
    case 7U:
      return "BeginSeqNo";
    case 8U:
      return "BeginString";
    case 9U:
      return "BodyLength";
    case 10U:
      return "CheckSum";
    case 11U:
      return "ClOrdID";
    case 16U:
      return "EndSeqNo";
    case 34U:
      return "MsgSeqNum";
    case 35U:
      return "MsgType";
    case 36U:
      return "NewSeqNo";
    case 38U:
      return "OrderQty";
    case 40U:
      return "OrdType";
    case 43U:
      return "PossDupFlag";
    case 44U:
      return "Price";
    case 45U:
      return "RefSeqNum";
    case 49U:
      return "SenderCompID";
    case 50U:
      return "SenderSubID";
    case 52U:
      return "SendingTime";
    case 54U:
      return "Side";
    case 55U:
      return "Symbol";
    case 56U:
      return "TargetCompID";
    case 57U:
      return "TargetSubID";
    case 58U:
      return "Text";
    case 60U:
      return "TransactTime";
    case 89U:
      return "Signature";
    case 90U:
      return "SecureDataLen";
    case 91U:
      return "SecureData";
    case 97U:
      return "PossResend";
    case 98U:
      return "EncryptMethod";
    case 108U:
      return "HeartBtInt";
    case 112U:
      return "TestReqID";
    case 115U:
      return "OnBehalfOfCompID";
    case 122U:
      return "OrigSendingTime";
    case 123U:
      return "GapFillFlag";
    case 128U:
      return "DeliverToCompID";
    case 141U:
      return "ResetSeqNumFlag";
    case 371U:
      return "RefTagID";
    case 372U:
      return "RefMsgType";
    case 373U:
      return "RejectReason";
    case 447U:
      return "PartyIDSource";
    case 448U:
      return "PartyID";
    case 452U:
      return "PartyRole";
    case 453U:
      return "NoPartyIDs";
    case 552U:
      return "NoSides";
    case 555U:
      return "NoLegs";
    case 600U:
      return "LegSymbol";
    case 601U:
      return "LegTransactTime";
    case 789U:
      return "NextExpectedMsgSeqNum";
    case 1128U:
      return "ApplVerID";
    case 1137U:
      return "DefaultApplVerID";
    default:
      return {};
  }
}

[[nodiscard]] auto
ParseUnsigned(std::string_view value, std::uint64_t* parsed) -> bool
{
  if (parsed == nullptr || value.empty()) {
    return false;
  }
  std::uint64_t result = 0U;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto [ptr, error] = std::from_chars(begin, end, result);
  if (error != std::errc() || ptr != end) {
    return false;
  }
  *parsed = result;
  return true;
}

[[nodiscard]] auto
ParseTag(std::string_view token, std::uint32_t* tag) -> bool
{
  std::uint64_t parsed = 0U;
  if (!ParseUnsigned(token, &parsed) || parsed > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  *tag = static_cast<std::uint32_t>(parsed);
  return true;
}

template<typename Fn>
auto
VisitFixFields(std::string_view raw_fix, Fn&& fn) -> void
{
  std::size_t begin = 0U;
  while (begin < raw_fix.size()) {
    std::size_t end = begin;
    while (end < raw_fix.size() && raw_fix[end] != kSoh && raw_fix[end] != kReadableDelimiter) {
      ++end;
    }

    const auto field = raw_fix.substr(begin, end - begin);
    const auto equals = field.find('=');
    if (equals != std::string_view::npos && equals > 0U) {
      std::uint32_t tag = 0U;
      if (ParseTag(field.substr(0U, equals), &tag)) {
        fn(FixFieldView{ .tag = tag, .value = field.substr(equals + 1U) });
      }
    }

    if (end == raw_fix.size()) {
      break;
    }
    begin = end + 1U;
  }
}

[[nodiscard]] auto
FindField(std::string_view raw_fix, std::uint32_t wanted_tag) -> std::optional<std::string_view>
{
  std::optional<std::string_view> found;
  VisitFixFields(raw_fix, [&](const FixFieldView field) {
    if (!found.has_value() && field.tag == wanted_tag) {
      found = field.value;
    }
  });
  return found;
}

auto
AppendJsonString(std::string& out, std::string_view value) -> void
{
  out.push_back('"');
  for (const auto raw_ch : value) {
    const auto ch = static_cast<unsigned char>(raw_ch);
    switch (ch) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (ch < 0x20U) {
          out.append("\\u00");
          out.push_back(kJsonHexDigits[(ch >> 4U) & 0x0FU]);
          out.push_back(kJsonHexDigits[ch & 0x0FU]);
        } else {
          out.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  out.push_back('"');
}

} // namespace

auto
FormatFixReadable(std::string_view raw_fix) -> std::string
{
  std::string out(raw_fix);
  for (auto& ch : out) {
    if (ch == kSoh) {
      ch = kReadableDelimiter;
    }
  }
  return out;
}

auto
FormatFixJson(std::string_view raw_fix) -> std::string
{
  std::string out;
  out.reserve(raw_fix.size() + 32U);
  out.push_back('{');

  bool first = true;
  VisitFixFields(raw_fix, [&](const FixFieldView field) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    const auto name = TagName(field.tag);
    if (!name.empty()) {
      AppendJsonString(out, name);
    } else {
      AppendJsonString(out, std::to_string(field.tag));
    }
    out.push_back(':');
    AppendJsonString(out, field.value);
  });

  out.push_back('}');
  return out;
}

auto
FormatFixMessage(std::string_view raw_fix, DumpFormat format) -> std::string
{
  switch (format) {
    case DumpFormat::kFixReadable:
      return FormatFixReadable(raw_fix);
    case DumpFormat::kJson:
      return FormatFixJson(raw_fix);
  }
  return FormatFixReadable(raw_fix);
}

auto
MatchesFilter(std::string_view raw_fix, const DumpFilter& filter) -> bool
{
  if (filter.msg_type.has_value()) {
    const auto msg_type = FindField(raw_fix, kMsgType);
    if (!msg_type.has_value() || msg_type.value() != filter.msg_type.value()) {
      return false;
    }
  }

  if (filter.seq_from.has_value() || filter.seq_to.has_value()) {
    const auto seq_num = FindField(raw_fix, kMsgSeqNum);
    std::uint64_t parsed = 0U;
    if (!seq_num.has_value() || !ParseUnsigned(seq_num.value(), &parsed) ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    if (filter.seq_from.has_value() && parsed < filter.seq_from.value()) {
      return false;
    }
    if (filter.seq_to.has_value() && parsed > filter.seq_to.value()) {
      return false;
    }
  }

  if (filter.tag_value.has_value() && !filter.tag.has_value()) {
    return false;
  }

  if (filter.tag.has_value()) {
    bool matched_tag = false;
    VisitFixFields(raw_fix, [&](const FixFieldView field) {
      if (matched_tag || field.tag != filter.tag.value()) {
        return;
      }
      matched_tag = !filter.tag_value.has_value() || field.value == filter.tag_value.value();
    });
    if (!matched_tag) {
      return false;
    }
  }

  return true;
}

} // namespace nimble::tools

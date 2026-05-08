#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nimblefix/generated/detail/message_shape.h"
#include "nimblefix/profile/profile_loader.h"

namespace {

using ScalarKind = nimble::generated::detail::ScalarKind;
using Presence = nimble::generated::detail::Presence;

struct Options
{
  std::vector<std::filesystem::path> input_paths;
  std::filesystem::path profile_path;
  std::filesystem::path output_path;
  std::string namespace_name;
};

struct SourceFile
{
  std::filesystem::path path;
  std::string text;
  std::vector<std::size_t> line_starts{ 0U };

  [[nodiscard]] auto line_for_offset(std::size_t offset) const -> std::uint32_t
  {
    const auto it = std::upper_bound(line_starts.begin(), line_starts.end(), offset);
    return static_cast<std::uint32_t>(std::distance(line_starts.begin(), it));
  }
};

struct DetectedSetter
{
  std::string field_method;
  Presence presence{ Presence::kAlways };
  std::filesystem::path source_path;
  std::uint32_t line{ 0U };
};

struct DetectedRawStaticField
{
  std::uint32_t tag{ 0U };
  ScalarKind kind{ ScalarKind::kString };
  Presence presence{ Presence::kAlways };
  std::filesystem::path source_path;
  std::uint32_t line{ 0U };
};

struct UsageSite
{
  std::string message_name;
  std::filesystem::path source_path;
  std::uint32_t line{ 0U };
  std::vector<DetectedSetter> setters;
  std::vector<DetectedRawStaticField> raw_static_fields;
};

struct FieldInfo
{
  std::uint32_t tag{ 0U };
  std::string name;
  std::string method_name;
  ScalarKind scalar_kind{ ScalarKind::kString };
};

struct MessageInfo
{
  const nimble::profile::MessageDefRecord* record{ nullptr };
  std::string name;
  std::string msg_type;
};

struct SchemaIndex
{
  std::vector<FieldInfo> fields;
  std::vector<MessageInfo> messages;
  std::unordered_map<std::string, std::size_t> field_by_alias;
  std::unordered_map<std::uint32_t, std::size_t> field_by_tag;
  std::unordered_map<std::string, std::size_t> message_by_name;
};

struct ShapeBodyNode
{
  std::uint32_t tag{ 0U };
  ScalarKind scalar_kind{ ScalarKind::kString };
  Presence presence{ Presence::kAlways };
};

struct CanonicalShape
{
  std::uint64_t schema_hash{ 0U };
  std::string message_name;
  std::string msg_type;
  std::filesystem::path first_source_path;
  std::uint32_t first_source_line{ 0U };
  std::uint64_t key_hash{ 0U };
  std::uint32_t usage_count{ 1U };
  std::vector<ShapeBodyNode> body_nodes;
  std::vector<DetectedRawStaticField> raw_static_fields;
};

auto
PrintUsage() -> void
{
  std::cout << "usage: nimblefix-usagegen --input <source.cpp> [--input <source.cpp> ...]\n"
               "                           --profile <profile.nfa>\n"
               "                           --output <shapes.h>\n"
               "                           [--namespace <namespace_name>]\n";
}

[[nodiscard]] auto
ResolveProjectPath(const std::filesystem::path& path) -> std::filesystem::path
{
  if (path.empty() || path.is_absolute()) {
    return path;
  }
  return std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / path;
}

[[nodiscard]] auto
Trim(std::string_view text) -> std::string_view
{
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1U);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1U);
  }
  return text;
}

[[nodiscard]] auto
LowercaseWord(std::string_view word) -> std::string
{
  std::string result;
  result.reserve(word.size());
  for (const auto ch : word) {
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return result;
}

[[nodiscard]] auto
TokenizeIdentifierWords(std::string_view text) -> std::vector<std::string>
{
  auto count_lowercase_run = [](std::string_view value, std::size_t offset) {
    std::size_t count = 0U;
    while (offset + count < value.size() && std::islower(static_cast<unsigned char>(value[offset + count])) != 0) {
      ++count;
    }
    return count;
  };

  std::vector<std::string> words;
  std::string current;
  current.reserve(text.size());

  const auto flush_current = [&]() {
    if (!current.empty()) {
      words.push_back(current);
      current.clear();
    }
  };

  for (std::size_t index = 0U; index < text.size(); ++index) {
    const auto ch = text[index];
    if (std::isalnum(static_cast<unsigned char>(ch)) == 0) {
      flush_current();
      continue;
    }

    if (!current.empty()) {
      const auto prev = current.back();
      bool boundary = false;
      if (std::islower(static_cast<unsigned char>(prev)) != 0 && std::isupper(static_cast<unsigned char>(ch)) != 0) {
        boundary = true;
      } else if (std::isdigit(static_cast<unsigned char>(prev)) != 0 &&
                 std::isalpha(static_cast<unsigned char>(ch)) != 0) {
        boundary = true;
      } else if (std::isupper(static_cast<unsigned char>(prev)) != 0 &&
                 std::isupper(static_cast<unsigned char>(ch)) != 0 && index + 1U < text.size() &&
                 std::islower(static_cast<unsigned char>(text[index + 1U])) != 0) {
        const auto lower_run = count_lowercase_run(text, index + 1U);
        const bool pluralized_acronym =
          lower_run == 1U && std::tolower(static_cast<unsigned char>(text[index + 1U])) == 's';
        boundary = !pluralized_acronym;
      }
      if (boundary) {
        flush_current();
      }
    }

    current.push_back(ch);
  }

  flush_current();
  return words;
}

[[nodiscard]] auto
SnakeCaseIdentifierFromWords(const std::vector<std::string>& words, std::string_view fallback) -> std::string
{
  std::string result;
  for (const auto& word : words) {
    if (word.empty()) {
      continue;
    }
    if (!result.empty()) {
      result.push_back('_');
    }
    result.append(LowercaseWord(word));
  }

  if (result.empty()) {
    result = std::string(fallback);
  }
  if (!result.empty() && std::isdigit(static_cast<unsigned char>(result.front())) != 0) {
    result.insert(result.begin(), '_');
  }
  return result;
}

[[nodiscard]] auto
FieldMethodName(std::string_view name, nimble::profile::ValueType value_type) -> std::string
{
  auto words = TokenizeIdentifierWords(name);
  if (value_type == nimble::profile::ValueType::kBoolean && !words.empty() && LowercaseWord(words.back()) == "flag") {
    words.pop_back();
  }
  return SnakeCaseIdentifierFromWords(words, "field");
}

[[nodiscard]] auto
FieldFullSnakeName(std::string_view name) -> std::string
{
  return SnakeCaseIdentifierFromWords(TokenizeIdentifierWords(name), "field");
}

[[nodiscard]] auto
IsValidIdentifier(std::string_view text) -> bool
{
  if (text.empty()) {
    return false;
  }
  if (std::isalpha(static_cast<unsigned char>(text.front())) == 0 && text.front() != '_') {
    return false;
  }
  return std::all_of(text.begin() + 1, text.end(), [](char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
  });
}

[[nodiscard]] auto
IsValidNamespaceName(std::string_view text) -> bool
{
  auto remaining = Trim(text);
  if (remaining.empty()) {
    return false;
  }

  while (!remaining.empty()) {
    const auto delimiter = remaining.find("::");
    const auto part = delimiter == std::string_view::npos ? remaining : remaining.substr(0U, delimiter);
    if (!IsValidIdentifier(part)) {
      return false;
    }
    if (delimiter == std::string_view::npos) {
      break;
    }
    if (delimiter + 2U == remaining.size()) {
      return false;
    }
    remaining.remove_prefix(delimiter + 2U);
  }
  return true;
}

[[nodiscard]] auto
ParseU32(std::string_view token) -> std::optional<std::uint32_t>
{
  try {
    std::size_t consumed = 0U;
    const auto value = std::stoull(std::string(token), &consumed, 10);
    if (consumed != token.size() || value > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
  } catch (...) {
    return std::nullopt;
  }
}

[[nodiscard]] auto
ParseOptions(int argc, char** argv, Options* options, bool* help_requested) -> bool
{
  *help_requested = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--help" || arg == "-h") {
      *help_requested = true;
      return true;
    }
    if (arg == "--input" && index + 1 < argc) {
      options->input_paths.emplace_back(argv[++index]);
      continue;
    }
    if (arg == "--profile" && index + 1 < argc) {
      options->profile_path = argv[++index];
      continue;
    }
    if (arg == "--output" && index + 1 < argc) {
      options->output_path = argv[++index];
      continue;
    }
    if (arg == "--namespace" && index + 1 < argc) {
      options->namespace_name = argv[++index];
      continue;
    }
    return false;
  }

  if (options->input_paths.empty() || options->profile_path.empty() || options->output_path.empty()) {
    return false;
  }
  if (!options->namespace_name.empty() && !IsValidNamespaceName(options->namespace_name)) {
    return false;
  }
  return true;
}

[[nodiscard]] auto
ReadSourceFile(const std::filesystem::path& path) -> std::optional<SourceFile>
{
  std::ifstream input(path);
  if (!input) {
    std::cerr << "unable to open input source: " << path << '\n';
    return std::nullopt;
  }

  SourceFile source;
  source.path = path;
  std::string line;
  while (std::getline(input, line)) {
    source.text.append(line);
    source.text.push_back('\n');
    source.line_starts.push_back(source.text.size());
  }
  return source;
}

[[nodiscard]] auto
SkipWhitespace(std::string_view text, std::size_t offset) -> std::size_t
{
  while (offset < text.size() && std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
    ++offset;
  }
  return offset;
}

[[nodiscard]] auto
FindMatchingDelimiter(std::string_view text, std::size_t open_offset, char open_delimiter, char close_delimiter)
  -> std::optional<std::size_t>
{
  if (open_offset >= text.size() || text[open_offset] != open_delimiter) {
    return std::nullopt;
  }

  std::uint32_t depth = 0U;
  bool in_line_comment = false;
  bool in_block_comment = false;
  bool in_string = false;
  bool in_char = false;
  bool escaped = false;

  for (std::size_t index = open_offset; index < text.size(); ++index) {
    const auto ch = text[index];
    const auto next = index + 1U < text.size() ? text[index + 1U] : '\0';

    if (in_line_comment) {
      if (ch == '\n') {
        in_line_comment = false;
      }
      continue;
    }
    if (in_block_comment) {
      if (ch == '*' && next == '/') {
        in_block_comment = false;
        ++index;
      }
      continue;
    }
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (in_char) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '\'') {
        in_char = false;
      }
      continue;
    }

    if (ch == '/' && next == '/') {
      in_line_comment = true;
      ++index;
      continue;
    }
    if (ch == '/' && next == '*') {
      in_block_comment = true;
      ++index;
      continue;
    }
    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == '\'') {
      in_char = true;
      continue;
    }

    if (ch == open_delimiter) {
      ++depth;
      continue;
    }
    if (ch == close_delimiter) {
      if (depth == 0U) {
        return std::nullopt;
      }
      --depth;
      if (depth == 0U) {
        return index;
      }
    }
  }
  return std::nullopt;
}

[[nodiscard]] auto
ExtractShortTypeName(std::string_view type_text) -> std::string
{
  auto trimmed = Trim(type_text);
  constexpr std::string_view kTypenamePrefix = "typename ";
  if (trimmed.starts_with(kTypenamePrefix)) {
    trimmed.remove_prefix(kTypenamePrefix.size());
    trimmed = Trim(trimmed);
  }

  std::size_t end = trimmed.size();
  while (end > 0U) {
    const auto ch = trimmed[end - 1U];
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_') {
      break;
    }
    --end;
  }

  std::size_t begin = end;
  while (begin > 0U) {
    const auto ch = trimmed[begin - 1U];
    if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '_') {
      break;
    }
    --begin;
  }
  return std::string(trimmed.substr(begin, end - begin));
}

[[nodiscard]] auto
ExtractLastIdentifier(std::string_view text) -> std::optional<std::string>
{
  auto trimmed = Trim(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  if (const auto default_value = trimmed.find('='); default_value != std::string_view::npos) {
    trimmed = Trim(trimmed.substr(0U, default_value));
  }

  std::size_t end = trimmed.size();
  while (end > 0U) {
    const auto ch = trimmed[end - 1U];
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_') {
      break;
    }
    --end;
  }

  std::size_t begin = end;
  while (begin > 0U) {
    const auto ch = trimmed[begin - 1U];
    if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '_') {
      break;
    }
    --begin;
  }

  if (begin == end) {
    return std::nullopt;
  }
  auto identifier = std::string(trimmed.substr(begin, end - begin));
  if (!IsValidIdentifier(identifier) || identifier == "auto" || identifier == "const" || identifier == "volatile") {
    return std::nullopt;
  }
  return identifier;
}

[[nodiscard]] auto
ExtractLambdaParameterName(std::string_view signature) -> std::optional<std::string>
{
  const auto paren_open = signature.find('(');
  if (paren_open == std::string_view::npos) {
    return std::nullopt;
  }
  const auto paren_close = FindMatchingDelimiter(signature, paren_open, '(', ')');
  if (!paren_close.has_value()) {
    return std::nullopt;
  }

  auto parameters = Trim(signature.substr(paren_open + 1U, *paren_close - paren_open - 1U));
  if (parameters.empty()) {
    return std::nullopt;
  }

  if (const auto comma = parameters.find(','); comma != std::string_view::npos) {
    parameters = Trim(parameters.substr(0U, comma));
  }
  return ExtractLastIdentifier(parameters);
}

[[nodiscard]] auto
FindStatementStart(std::string_view body, std::size_t offset) -> std::size_t
{
  std::size_t start = offset;
  while (start > 0U) {
    const auto ch = body[start - 1U];
    if (ch == ';' || ch == '{' || ch == '}') {
      break;
    }
    --start;
  }
  return start;
}

[[nodiscard]] auto
StatementIsRootedAt(std::string_view body, std::size_t call_offset, std::string_view root_name) -> bool
{
  if (root_name.empty()) {
    return true;
  }

  auto start = FindStatementStart(body, call_offset);
  start = SkipWhitespace(body, start);
  if (start + root_name.size() > body.size()) {
    return false;
  }
  if (body.substr(start, root_name.size()) != root_name) {
    return false;
  }
  const auto next = start + root_name.size();
  return next == body.size() || (std::isalnum(static_cast<unsigned char>(body[next])) == 0 && body[next] != '_');
}

[[nodiscard]] auto
ComputeIfBodyRanges(std::string_view body) -> std::vector<std::pair<std::size_t, std::size_t>>
{
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  const std::regex if_regex(R"(\bif\s*(?:constexpr\s*)?\()", std::regex::ECMAScript);
  for (auto it = std::cregex_iterator(body.begin(), body.end(), if_regex); it != std::cregex_iterator(); ++it) {
    const auto paren_open = static_cast<std::size_t>(it->position() + it->length() - 1);
    const auto paren_close = FindMatchingDelimiter(body, paren_open, '(', ')');
    if (!paren_close.has_value()) {
      continue;
    }
    const auto brace_open = SkipWhitespace(body, *paren_close + 1U);
    if (brace_open >= body.size() || body[brace_open] != '{') {
      continue;
    }
    const auto brace_close = FindMatchingDelimiter(body, brace_open, '{', '}');
    if (brace_close.has_value()) {
      ranges.emplace_back(brace_open, *brace_close);
    }
  }
  return ranges;
}

[[nodiscard]] auto
PresenceAt(std::size_t offset, const std::vector<std::pair<std::size_t, std::size_t>>& if_ranges) -> Presence
{
  const auto inside_if = std::any_of(if_ranges.begin(), if_ranges.end(), [offset](const auto& range) {
    return offset > range.first && offset < range.second;
  });
  return inside_if ? Presence::kOptional : Presence::kAlways;
}

[[nodiscard]] auto
InferRawScalarKind(std::string_view argument) -> ScalarKind
{
  const auto trimmed = Trim(argument);
  const auto lower = LowercaseWord(trimmed);
  if (lower.find("string_view") != std::string::npos || lower.find("std::string") != std::string::npos ||
      trimmed.starts_with('"')) {
    return ScalarKind::kString;
  }
  if (trimmed.starts_with('\'')) {
    return ScalarKind::kChar;
  }
  if (lower == "true" || lower == "false" || lower.find("bool") != std::string::npos) {
    return ScalarKind::kBool;
  }
  if (lower.find("double") != std::string::npos || lower.find("float") != std::string::npos ||
      trimmed.find('.') != std::string_view::npos) {
    return ScalarKind::kFloat;
  }
  return ScalarKind::kInt;
}

auto
AnalyzeLambdaBody(const SourceFile& source,
                  std::size_t body_begin,
                  std::string_view body,
                  std::string_view builder_parameter,
                  UsageSite* site) -> void
{
  const auto if_ranges = ComputeIfBodyRanges(body);

  const std::regex setter_regex(
    R"((?:^|[^A-Za-z0-9_:])(?:[A-Za-z_][A-Za-z0-9_]*\s*)?\.\s*([A-Za-z_][A-Za-z0-9_]*)\s*\()", std::regex::ECMAScript);
  for (auto it = std::cregex_iterator(body.begin(), body.end(), setter_regex); it != std::cregex_iterator(); ++it) {
    const auto method_offset = static_cast<std::size_t>(it->position(1));
    const auto call_offset = static_cast<std::size_t>(it->position());
    if (!StatementIsRootedAt(body, call_offset, builder_parameter)) {
      continue;
    }
    const auto method = (*it)[1].str();
    if (method == "set_tag" || method == "template") {
      continue;
    }
    site->setters.push_back(DetectedSetter{
      .field_method = method,
      .presence = PresenceAt(method_offset, if_ranges),
      .source_path = source.path,
      .line = source.line_for_offset(body_begin + method_offset),
    });
  }

  const std::regex raw_regex(R"((?:^|[^A-Za-z0-9_])(?:template\s+)?set_tag\s*<\s*([0-9]+)[uUlL]*\s*>\s*\()",
                             std::regex::ECMAScript);
  for (auto it = std::cregex_iterator(body.begin(), body.end(), raw_regex); it != std::cregex_iterator(); ++it) {
    const auto call_offset = static_cast<std::size_t>(it->position());
    if (!StatementIsRootedAt(body, call_offset, builder_parameter)) {
      continue;
    }

    const auto tag = ParseU32((*it)[1].str());
    if (!tag.has_value()) {
      continue;
    }

    const auto open_paren = static_cast<std::size_t>(it->position() + it->length() - 1);
    const auto close_paren = FindMatchingDelimiter(body, open_paren, '(', ')');
    const auto argument =
      close_paren.has_value() ? body.substr(open_paren + 1U, *close_paren - open_paren - 1U) : std::string_view();
    const auto tag_offset = static_cast<std::size_t>(it->position(1));
    site->raw_static_fields.push_back(DetectedRawStaticField{
      .tag = *tag,
      .kind = InferRawScalarKind(argument),
      .presence = PresenceAt(tag_offset, if_ranges),
      .source_path = source.path,
      .line = source.line_for_offset(body_begin + tag_offset),
    });
  }
}

auto
ScanSource(const SourceFile& source, std::vector<UsageSite>* sites) -> bool
{
  const std::regex send_regex(R"(\bsend\s*<\s*([^>]*)\s*>\s*\()", std::regex::ECMAScript);
  for (auto it = std::sregex_iterator(source.text.begin(), source.text.end(), send_regex); it != std::sregex_iterator();
       ++it) {
    const auto send_offset = static_cast<std::size_t>(it->position());
    const auto send_line = source.line_for_offset(send_offset);
    const auto message_name = ExtractShortTypeName((*it)[1].str());
    if (message_name.empty()) {
      std::cerr << source.path << ':' << send_line << ": warning: unable to extract send<> message type\n";
      continue;
    }

    const auto call_arg_offset = static_cast<std::size_t>(it->position() + it->length());
    const auto lambda_start = SkipWhitespace(source.text, call_arg_offset);
    if (lambda_start >= source.text.size() || source.text[lambda_start] != '[') {
      std::cerr << source.path << ':' << send_line << ": warning: send<" << message_name
                << "> does not start with a lambda; helper expansion is out of scope\n";
      continue;
    }

    const auto capture_close = FindMatchingDelimiter(source.text, lambda_start, '[', ']');
    if (!capture_close.has_value()) {
      std::cerr << source.path << ':' << send_line << ": warning: unable to parse lambda capture for send<"
                << message_name << ">\n";
      continue;
    }

    const auto body_open = source.text.find('{', *capture_close + 1U);
    if (body_open == std::string::npos) {
      std::cerr << source.path << ':' << send_line << ": warning: unable to find lambda body for send<" << message_name
                << ">\n";
      continue;
    }

    const auto body_close = FindMatchingDelimiter(source.text, body_open, '{', '}');
    if (!body_close.has_value()) {
      std::cerr << source.path << ':' << send_line << ": warning: unable to match lambda body braces for send<"
                << message_name << ">\n";
      continue;
    }

    UsageSite site;
    site.message_name = message_name;
    site.source_path = source.path;
    site.line = send_line;
    const auto builder_parameter = ExtractLambdaParameterName(
      std::string_view(source.text).substr(*capture_close + 1U, body_open - *capture_close - 1U));
    if (!builder_parameter.has_value()) {
      std::cerr << source.path << ':' << send_line << ": warning: unable to extract lambda builder parameter for send<"
                << message_name << ">; scanning all setter-like calls in the lambda body\n";
    }
    AnalyzeLambdaBody(source,
                      body_open + 1U,
                      std::string_view(source.text).substr(body_open + 1U, *body_close - body_open - 1U),
                      builder_parameter.value_or(std::string()),
                      &site);
    sites->push_back(std::move(site));
  }
  return true;
}

[[nodiscard]] auto
ScalarKindFromValueType(nimble::profile::ValueType value_type) -> ScalarKind
{
  switch (value_type) {
    case nimble::profile::ValueType::kInt:
      return ScalarKind::kInt;
    case nimble::profile::ValueType::kChar:
      return ScalarKind::kChar;
    case nimble::profile::ValueType::kFloat:
      return ScalarKind::kFloat;
    case nimble::profile::ValueType::kBoolean:
      return ScalarKind::kBool;
    case nimble::profile::ValueType::kString:
    case nimble::profile::ValueType::kTimestamp:
    case nimble::profile::ValueType::kUnknown:
      return ScalarKind::kString;
  }
  return ScalarKind::kString;
}

auto
AddFieldAlias(SchemaIndex* index, std::string alias, std::size_t field_index) -> void
{
  if (alias.empty()) {
    return;
  }
  index->field_by_alias.emplace(std::move(alias), field_index);
}

[[nodiscard]] auto
BuildSchemaIndex(const nimble::profile::NormalizedDictionaryView& dictionary) -> SchemaIndex
{
  SchemaIndex index;
  index.fields.reserve(dictionary.field_count());
  for (const auto& record : dictionary.fields()) {
    const auto name = dictionary.field_name(record);
    if (!name.has_value()) {
      continue;
    }

    const auto value_type = static_cast<nimble::profile::ValueType>(record.value_type);
    FieldInfo field{
      .tag = record.tag,
      .name = std::string(*name),
      .method_name = FieldMethodName(*name, value_type),
      .scalar_kind = ScalarKindFromValueType(value_type),
    };
    const auto field_index = index.fields.size();
    index.fields.push_back(std::move(field));
    index.field_by_tag.emplace(record.tag, field_index);
    AddFieldAlias(&index, index.fields.back().method_name, field_index);
    AddFieldAlias(&index, FieldFullSnakeName(*name), field_index);
    AddFieldAlias(&index, std::string(*name), field_index);
  }

  index.messages.reserve(dictionary.message_count());
  for (const auto& record : dictionary.messages()) {
    const auto name = dictionary.message_name(record);
    const auto msg_type = dictionary.message_type(record);
    if (!name.has_value() || !msg_type.has_value()) {
      continue;
    }

    const auto message_index = index.messages.size();
    index.messages.push_back(MessageInfo{
      .record = &record,
      .name = std::string(*name),
      .msg_type = std::string(*msg_type),
    });
    index.message_by_name.emplace(index.messages.back().name, message_index);
  }
  return index;
}

[[nodiscard]] auto
MergePresence(Presence lhs, Presence rhs) -> Presence
{
  return lhs == Presence::kAlways || rhs == Presence::kAlways ? Presence::kAlways : Presence::kOptional;
}

[[nodiscard]] auto
CorrelateUsageSite(const UsageSite& site,
                   const nimble::profile::NormalizedDictionaryView& dictionary,
                   const SchemaIndex& schema) -> std::optional<CanonicalShape>
{
  const auto message_it = schema.message_by_name.find(site.message_name);
  if (message_it == schema.message_by_name.end()) {
    std::cerr << site.source_path << ':' << site.line << ": warning: message '" << site.message_name
              << "' was not found in profile\n";
    return std::nullopt;
  }

  const auto& message = schema.messages[message_it->second];
  std::unordered_map<std::uint32_t, Presence> detected_fields;
  for (const auto& setter : site.setters) {
    const auto field_it = schema.field_by_alias.find(setter.field_method);
    if (field_it == schema.field_by_alias.end()) {
      std::cerr << setter.source_path << ':' << setter.line << ": warning: setter '." << setter.field_method
                << "(...)' does not map to a profile field\n";
      continue;
    }

    const auto& field = schema.fields[field_it->second];
    if (!dictionary.message_rule_allows_tag(*message.record, field.tag)) {
      std::cerr << setter.source_path << ':' << setter.line << ": warning: field '" << field.name << "' (tag "
                << field.tag << ") is not declared on message '" << message.name << "'\n";
      continue;
    }

    auto [present_it, inserted] = detected_fields.emplace(field.tag, setter.presence);
    if (!inserted) {
      present_it->second = MergePresence(present_it->second, setter.presence);
    }
  }

  CanonicalShape shape;
  shape.schema_hash = dictionary.profile().schema_hash();
  shape.message_name = message.name;
  shape.msg_type = message.msg_type;
  shape.first_source_path = site.source_path;
  shape.first_source_line = site.line;
  shape.raw_static_fields = site.raw_static_fields;

  for (const auto& rule : dictionary.message_field_rules(*message.record)) {
    const auto detected_it = detected_fields.find(rule.tag);
    if (detected_it == detected_fields.end()) {
      continue;
    }

    const auto field_it = schema.field_by_tag.find(rule.tag);
    if (field_it == schema.field_by_tag.end()) {
      continue;
    }
    const auto& field = schema.fields[field_it->second];
    shape.body_nodes.push_back(ShapeBodyNode{
      .tag = field.tag,
      .scalar_kind = field.scalar_kind,
      .presence = detected_it->second,
    });
  }

  return shape;
}

auto
AppendHashByte(std::uint64_t* hash, std::uint8_t byte) -> void
{
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  *hash ^= byte;
  *hash *= kPrime;
}

auto
AppendHashU64(std::uint64_t* hash, std::uint64_t value) -> void
{
  for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
    AppendHashByte(hash, static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

auto
AppendHashString(std::uint64_t* hash, std::string_view value) -> void
{
  for (const auto ch : value) {
    AppendHashByte(hash, static_cast<std::uint8_t>(ch));
  }
  AppendHashByte(hash, 0U);
}

[[nodiscard]] auto
BuildShapeKey(const CanonicalShape& shape) -> std::string
{
  std::ostringstream key;
  key << shape.schema_hash << "|" << shape.msg_type << '|';
  for (const auto& node : shape.body_nodes) {
    key << "b:" << node.tag << ':' << static_cast<int>(node.scalar_kind) << ':' << static_cast<int>(node.presence)
        << ';';
  }
  key << '|';
  for (const auto& raw : shape.raw_static_fields) {
    key << "r:" << raw.tag << ':' << static_cast<int>(raw.kind) << ':' << static_cast<int>(raw.presence) << ';';
  }
  return key.str();
}

[[nodiscard]] auto
HashShapeKey(const CanonicalShape& shape) -> std::uint64_t
{
  constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
  std::uint64_t hash = kOffsetBasis;
  AppendHashU64(&hash, shape.schema_hash);
  AppendHashString(&hash, shape.msg_type);
  for (const auto& node : shape.body_nodes) {
    AppendHashU64(&hash, node.tag);
    AppendHashByte(&hash, static_cast<std::uint8_t>(node.scalar_kind));
    AppendHashByte(&hash, static_cast<std::uint8_t>(node.presence));
  }
  AppendHashByte(&hash, 0xFFU);
  for (const auto& raw : shape.raw_static_fields) {
    AppendHashU64(&hash, raw.tag);
    AppendHashByte(&hash, static_cast<std::uint8_t>(raw.kind));
    AppendHashByte(&hash, static_cast<std::uint8_t>(raw.presence));
  }
  return hash;
}

[[nodiscard]] auto
DeduplicateShapes(std::vector<CanonicalShape> shapes) -> std::vector<CanonicalShape>
{
  std::vector<CanonicalShape> unique;
  std::unordered_map<std::string, std::size_t> shape_by_key;
  for (auto& shape : shapes) {
    shape.key_hash = HashShapeKey(shape);
    const auto key = BuildShapeKey(shape);
    const auto found = shape_by_key.find(key);
    if (found != shape_by_key.end()) {
      ++unique[found->second].usage_count;
      continue;
    }
    shape_by_key.emplace(key, unique.size());
    unique.push_back(std::move(shape));
  }
  return unique;
}

[[nodiscard]] auto
CppStringLiteral(std::string_view value) -> std::string
{
  std::string out;
  out.reserve(value.size() + 8U);
  for (const auto ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

[[nodiscard]] auto
ScalarKindLiteral(ScalarKind kind) -> std::string_view
{
  switch (kind) {
    case ScalarKind::kInt:
      return "detail::ScalarKind::kInt";
    case ScalarKind::kChar:
      return "detail::ScalarKind::kChar";
    case ScalarKind::kFloat:
      return "detail::ScalarKind::kFloat";
    case ScalarKind::kBool:
      return "detail::ScalarKind::kBool";
    case ScalarKind::kString:
      return "detail::ScalarKind::kString";
  }
  return "detail::ScalarKind::kString";
}

[[nodiscard]] auto
PresenceLiteral(Presence presence) -> std::string_view
{
  switch (presence) {
    case Presence::kAlways:
      return "detail::Presence::kAlways";
    case Presence::kOptional:
      return "detail::Presence::kOptional";
  }
  return "detail::Presence::kAlways";
}

auto
EmitShapeHeader(std::ostream& out, std::string_view namespace_name, const std::vector<CanonicalShape>& shapes) -> void
{
  out << "#pragma once\n\n"
      << "#include <cstdint>\n\n"
      << "#include \"nimblefix/generated/detail/message_shape.h\"\n\n"
      << "// Generated by nimblefix-usagegen. Do not edit.\n"
      << "// The source scanner is regex-based and intentionally best-effort.\n\n"
      << "namespace " << namespace_name << " {\n\n"
      << "namespace detail = ::nimble::generated::detail;\n\n"
      << "namespace usage_shape_data {\n\n";

  for (std::size_t index = 0U; index < shapes.size(); ++index) {
    const auto& shape = shapes[index];
    const auto prefix = std::string("kUsageShape") + std::to_string(index);
    out << "// " << prefix << ": " << shape.message_name << " from " << shape.first_source_path.string() << ':'
        << shape.first_source_line << ", uses=" << shape.usage_count << ", key_hash=0x" << std::hex << shape.key_hash
        << std::dec << "\n";

    if (!shape.body_nodes.empty()) {
      out << "inline constexpr detail::BodyNode " << prefix << "Body[] = {\n";
      for (const auto& node : shape.body_nodes) {
        out << "  { detail::BodyNode::Kind::kScalar, " << node.tag << "U, " << ScalarKindLiteral(node.scalar_kind)
            << ", " << PresenceLiteral(node.presence) << ", 0U, nullptr, 0U },\n";
      }
      out << "};\n";
    }

    if (!shape.raw_static_fields.empty()) {
      out << "inline constexpr detail::RawStaticField " << prefix << "RawExtras[] = {\n";
      for (const auto& raw : shape.raw_static_fields) {
        out << "  { " << raw.tag << "U, " << ScalarKindLiteral(raw.kind) << ", " << PresenceLiteral(raw.presence)
            << " },\n";
      }
      out << "};\n";
    }

    out << "inline constexpr detail::MessageShape " << prefix << " = {\n"
        << "  " << shape.schema_hash << "ULL,\n"
        << "  \"" << CppStringLiteral(shape.msg_type) << "\",\n";
    if (shape.body_nodes.empty()) {
      out << "  nullptr,\n"
          << "  0U,\n";
    } else {
      out << "  " << prefix << "Body,\n"
          << "  " << shape.body_nodes.size() << "U,\n";
    }
    if (shape.raw_static_fields.empty()) {
      out << "  nullptr,\n"
          << "  0U,\n";
    } else {
      out << "  " << prefix << "RawExtras,\n"
          << "  " << shape.raw_static_fields.size() << "U,\n";
    }
    out << "};\n\n";
  }

  if (shapes.empty()) {
    out << "inline constexpr const detail::MessageShape* const* kUsageShapes = nullptr;\n";
  } else {
    out << "inline constexpr const detail::MessageShape* kUsageShapes[] = {\n";
    for (std::size_t index = 0U; index < shapes.size(); ++index) {
      out << "  &kUsageShape" << index << ",\n";
    }
    out << "};\n";
  }
  out << "inline constexpr std::uint32_t kUsageShapeCount = " << shapes.size() << "U;\n\n"
      << "} // namespace usage_shape_data\n\n"
      << "} // namespace " << namespace_name << "\n";
}

[[nodiscard]] auto
WriteShapeHeader(const std::filesystem::path& path,
                 std::string_view namespace_name,
                 const std::vector<CanonicalShape>& shapes) -> bool
{
  if (const auto parent = path.parent_path(); !parent.empty()) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
      std::cerr << "unable to create output directory '" << parent << "': " << error.message() << '\n';
      return false;
    }
  }

  std::ofstream output(path);
  if (!output) {
    std::cerr << "unable to open output header: " << path << '\n';
    return false;
  }
  EmitShapeHeader(output, namespace_name, shapes);
  return true;
}

} // namespace

int
main(int argc, char** argv)
{
  Options options;
  bool help_requested = false;
  if (!ParseOptions(argc, argv, &options, &help_requested)) {
    PrintUsage();
    return 1;
  }
  if (help_requested) {
    PrintUsage();
    return 0;
  }

  for (auto& path : options.input_paths) {
    path = ResolveProjectPath(path);
  }
  options.profile_path = ResolveProjectPath(options.profile_path);
  options.output_path = ResolveProjectPath(options.output_path);

  auto loaded_profile = nimble::profile::LoadProfileArtifact(options.profile_path);
  if (!loaded_profile.ok()) {
    std::cerr << loaded_profile.status().message() << '\n';
    return 1;
  }
  auto dictionary = nimble::profile::NormalizedDictionaryView::FromProfile(std::move(loaded_profile).value());
  if (!dictionary.ok()) {
    std::cerr << dictionary.status().message() << '\n';
    return 1;
  }

  if (options.namespace_name.empty()) {
    options.namespace_name = "nimble::generated::profile_" + std::to_string(dictionary.value().profile().profile_id());
  }

  std::vector<UsageSite> sites;
  for (const auto& path : options.input_paths) {
    auto source = ReadSourceFile(path);
    if (!source.has_value()) {
      return 1;
    }
    if (!ScanSource(*source, &sites)) {
      return 1;
    }
  }

  const auto schema = BuildSchemaIndex(dictionary.value());
  std::vector<CanonicalShape> correlated_shapes;
  correlated_shapes.reserve(sites.size());
  for (const auto& site : sites) {
    auto shape = CorrelateUsageSite(site, dictionary.value(), schema);
    if (shape.has_value()) {
      correlated_shapes.push_back(std::move(*shape));
    }
  }
  auto unique_shapes = DeduplicateShapes(std::move(correlated_shapes));

  if (!WriteShapeHeader(options.output_path, options.namespace_name, unique_shapes)) {
    return 1;
  }

  std::cout << "scanned " << options.input_paths.size() << " input file(s), found " << sites.size()
            << " send site(s), generated " << unique_shapes.size() << " unique shape(s) at " << options.output_path
            << '\n';
  return 0;
}

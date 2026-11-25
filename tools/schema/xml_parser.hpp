#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nimblefix::codegen {

inline constexpr std::size_t kMaxInputBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaxTotalInputBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaxLogBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaxInputFiles = 32U;
inline constexpr std::size_t kMaxXmlDepth = 64U;
inline constexpr std::size_t kMaxXmlItems = 50'000U;
inline constexpr std::uint32_t kMaxFixTag = 999'999'999U;

struct Location {
    std::string source;
    std::size_t line{1};
    std::size_t column{1};

    [[nodiscard]] std::string format() const {
        return source + ":" + std::to_string(line) + ":" + std::to_string(column);
    }

    auto operator<=>(const Location&) const = default;
};

class SchemaError : public std::runtime_error {
public:
    explicit SchemaError(std::string msg)
        : std::runtime_error(msg), message_(std::move(msg)) {}

    SchemaError(std::string msg, Location loc)
        : std::runtime_error(loc.format() + ": " + msg),
          message_(std::move(msg)),
          has_location_(true),
          location_(std::move(loc)) {}

    [[nodiscard]] const std::string& raw_message() const noexcept { return message_; }
    [[nodiscard]] bool has_location() const noexcept { return has_location_; }
    [[nodiscard]] const Location& location() const noexcept { return location_; }

private:
    std::string message_;
    bool has_location_{false};
    Location location_{};
};

struct XmlNode {
    std::string name;
    std::map<std::string, std::string> attrs;
    Location location;
    std::vector<XmlNode> children;
    std::string text;
};

// Self-contained FIPS 180-4 SHA-256 (zero external crypto dependencies).
class Sha256 {
public:
    void update(const std::uint8_t* data, std::size_t len) noexcept {
        for (std::size_t i = 0; i < len; ++i) {
            block_[block_len_++] = data[i];
            if (block_len_ == 64) {
                compress(block_);
                total_bits_ += 512;
                block_len_ = 0;
            }
        }
    }

    void update(std::string_view s) noexcept {
        update(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    }

    void update_byte(std::uint8_t b) noexcept {
        update(&b, 1);
    }

    [[nodiscard]] std::string hex_digest() {
        total_bits_ += static_cast<std::uint64_t>(block_len_) * 8ULL;
        block_[block_len_++] = 0x80U;
        if (block_len_ > 56) {
            while (block_len_ < 64) block_[block_len_++] = 0;
            compress(block_);
            block_len_ = 0;
        }
        while (block_len_ < 56) block_[block_len_++] = 0;
        for (int i = 7; i >= 0; --i) {
            block_[block_len_++] = static_cast<std::uint8_t>((total_bits_ >> (i * 8)) & 0xFFU);
        }
        compress(block_);
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (std::uint32_t w : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                out.push_back(kHex[(w >> shift) & 0xFU]);
            }
        }
        return out;
    }

private:
    static constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept {
        return (x >> n) | (x << (32U - n));
    }

    void compress(const std::uint8_t blk[64]) noexcept {
        static constexpr std::array<std::uint32_t, 64> kK = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        };
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(blk[i * 4]) << 24U) |
                   (static_cast<std::uint32_t>(blk[i * 4 + 1]) << 16U) |
                   (static_cast<std::uint32_t>(blk[i * 4 + 2]) << 8U) |
                   static_cast<std::uint32_t>(blk[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t t1 = h + S1 + ch + kK[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    std::uint8_t block_[64]{};
    std::size_t block_len_{0};
    std::uint64_t total_bits_{0};
};

// Strict, zero-dependency XML parser for FIX dictionaries.
// Rejects DOCTYPE, ENTITY, external entities, XML namespaces, unexpected text, and excessive depth/size.
class StrictXmlParser {
public:
    static XmlNode parse(std::string_view data, const std::string& source) {
        StrictXmlParser p{data, source};
        return p.run();
    }

private:
    StrictXmlParser(std::string_view data, const std::string& source)
        : data_(data), source_(source) {}

    [[nodiscard]] Location loc() const { return Location{source_, line_, col_}; }

    [[nodiscard]] bool eof() const noexcept { return pos_ >= data_.size(); }
    [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept {
        return (pos_ + ahead < data_.size()) ? data_[pos_ + ahead] : '\0';
    }

    char get() noexcept {
        if (pos_ >= data_.size()) return '\0';
        const char ch = data_[pos_++];
        if (ch == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        return ch;
    }

    bool starts_with(std::string_view prefix) const noexcept {
        return data_.substr(pos_).starts_with(prefix);
    }

    void advance(std::size_t n) noexcept {
        for (std::size_t i = 0; i < n; ++i) (void)get();
    }

    static bool is_ws(char c) noexcept {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    static bool is_name_start(unsigned char c) noexcept {
        return std::isalpha(c) != 0 || c == '_' || c == ':';
    }

    static bool is_name_char(unsigned char c) noexcept {
        return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.' || c == ':';
    }

    void skip_ws() noexcept {
        while (!eof() && is_ws(peek())) (void)get();
    }

    std::string parse_name() {
        if (eof() || !is_name_start(static_cast<unsigned char>(peek()))) {
            throw SchemaError("malformed XML: expected name", loc());
        }
        std::string out;
        while (!eof() && is_name_char(static_cast<unsigned char>(peek()))) {
            out.push_back(get());
        }
        return out;
    }

    void decode_entity(std::string& out) {
        const Location start_loc = loc();
        if (get() != '&') throw SchemaError("malformed XML: expected '&'", start_loc);
        std::string ref;
        while (!eof() && peek() != ';') {
            ref.push_back(get());
            if (ref.size() > 32) throw SchemaError("malformed XML: unterminated entity reference", start_loc);
        }
        if (eof() || get() != ';') throw SchemaError("malformed XML: unterminated entity reference", start_loc);
        if (ref == "amp") out.push_back('&');
        else if (ref == "lt") out.push_back('<');
        else if (ref == "gt") out.push_back('>');
        else if (ref == "quot") out.push_back('"');
        else if (ref == "apos") out.push_back('\'');
        else if (!ref.empty() && ref[0] == '#') {
            std::uint32_t cp = 0;
            try {
                if (ref.size() > 2 && (ref[1] == 'x' || ref[1] == 'X')) {
                    cp = static_cast<std::uint32_t>(std::stoul(ref.substr(2), nullptr, 16));
                } else {
                    cp = static_cast<std::uint32_t>(std::stoul(ref.substr(1), nullptr, 10));
                }
            } catch (...) {
                throw SchemaError("malformed XML: invalid character reference", start_loc);
            }
            if (cp == 0 || cp > 0x10FFFFU) throw SchemaError("malformed XML: invalid codepoint", start_loc);
            if (cp < 0x80U) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800U) {
                out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
                out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
            } else if (cp < 0x10000U) {
                out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
                out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
            } else {
                out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
                out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
            }
        } else {
            throw SchemaError("entity declarations are not allowed", start_loc);
        }
    }

    std::string parse_attr_value() {
        const Location q_loc = loc();
        const char quote = get();
        if (quote != '"' && quote != '\'') {
            throw SchemaError("malformed XML: expected quoted attribute value", q_loc);
        }
        std::string out;
        while (!eof() && peek() != quote) {
            const char c = peek();
            if (c == '<') throw SchemaError("malformed XML: '<' in attribute value", loc());
            if (c == '&') {
                decode_entity(out);
            } else {
                out.push_back(get());
            }
        }
        if (eof() || get() != quote) {
            throw SchemaError("malformed XML: unterminated attribute value", q_loc);
        }
        return out;
    }

    static bool has_non_ws(std::string_view s) noexcept {
        for (char c : s) if (!is_ws(c)) return true;
        return false;
    }

    XmlNode run() {
        // Strip optional UTF-8 BOM.
        if (data_.size() >= 3 &&
            static_cast<unsigned char>(data_[0]) == 0xEF &&
            static_cast<unsigned char>(data_[1]) == 0xBB &&
            static_cast<unsigned char>(data_[2]) == 0xBF) {
            pos_ = 3;
        }

        std::vector<XmlNode> stack;
        bool has_root = false;
        XmlNode root{};
        std::size_t item_count = 0;

        while (!eof()) {
            if (peek() != '<') {
                const Location tloc = loc();
                std::string chunk;
                while (!eof() && peek() != '<') {
                    if (peek() == '&') decode_entity(chunk);
                    else chunk.push_back(get());
                }
                if (!stack.empty()) {
                    stack.back().text.append(chunk);
                } else if (has_non_ws(chunk)) {
                    throw SchemaError("text outside XML root element", tloc);
                }
                continue;
            }

            if (starts_with("<!--")) {
                const Location cloc = loc();
                advance(4);
                bool closed = false;
                while (!eof()) {
                    if (starts_with("-->")) {
                        advance(3);
                        closed = true;
                        break;
                    }
                    (void)get();
                }
                if (!closed) throw SchemaError("malformed XML: unterminated comment", cloc);
                continue;
            }

            if (starts_with("<?")) {
                const Location ploc = loc();
                advance(2);
                bool closed = false;
                while (!eof()) {
                    if (starts_with("?>")) {
                        advance(2);
                        closed = true;
                        break;
                    }
                    (void)get();
                }
                if (!closed) throw SchemaError("malformed XML: unterminated processing instruction", ploc);
                continue;
            }

            if (starts_with("<!DOCTYPE")) {
                throw SchemaError("DOCTYPE declarations are not allowed", loc());
            }
            if (starts_with("<!ENTITY")) {
                throw SchemaError("entity declarations are not allowed", loc());
            }
            if (starts_with("<![CDATA[")) {
                const Location cdloc = loc();
                advance(9);
                std::string cdata;
                bool closed = false;
                while (!eof()) {
                    if (starts_with("]]>")) {
                        advance(3);
                        closed = true;
                        break;
                    }
                    cdata.push_back(get());
                }
                if (!closed) throw SchemaError("malformed XML: unterminated CDATA", cdloc);
                if (!stack.empty()) stack.back().text.append(cdata);
                else if (has_non_ws(cdata)) throw SchemaError("text outside XML root element", cdloc);
                continue;
            }

            if (starts_with("</")) {
                const Location eloc = loc();
                advance(2);
                const std::string close_name = parse_name();
                skip_ws();
                if (eof() || get() != '>') throw SchemaError("malformed XML: expected '>' in closing tag", eloc);
                if (stack.empty()) throw SchemaError("unexpected closing element", eloc);
                if (stack.back().name != close_name) {
                    throw SchemaError("malformed XML: mismatched closing tag </" + close_name + ">", eloc);
                }
                XmlNode finished = std::move(stack.back());
                stack.pop_back();
                if (has_non_ws(finished.text)) {
                    throw SchemaError("unexpected text inside <" + finished.name + ">", finished.location);
                }
                if (!stack.empty()) {
                    stack.back().children.push_back(std::move(finished));
                } else {
                    root = std::move(finished);
                    has_root = true;
                }
                continue;
            }

            // Opening tag
            const Location tag_loc = loc();
            (void)get(); // '<'
            const std::string tag_name = parse_name();
            if (tag_name.find(':') != std::string::npos) {
                throw SchemaError("XML namespaces are not supported by this generator", tag_loc);
            }
            std::map<std::string, std::string> attrs;
            bool self_closing = false;
            for (;;) {
                const bool had_ws = !eof() && is_ws(peek());
                skip_ws();
                if (eof()) throw SchemaError("malformed XML: unexpected EOF inside start tag", tag_loc);
                if (starts_with("/>")) {
                    advance(2);
                    self_closing = true;
                    break;
                }
                if (peek() == '>') {
                    (void)get();
                    break;
                }
                if (!attrs.empty() && !had_ws) {
                    throw SchemaError("malformed XML: expected whitespace between attributes", loc());
                }
                const Location aloc = loc();
                const std::string key = parse_name();
                if (key == "xmlns" || key.find(':') != std::string::npos) {
                    throw SchemaError("XML namespaces are not supported by this generator", aloc);
                }
                skip_ws();
                if (eof() || get() != '=') {
                    throw SchemaError("malformed XML: expected '=' after attribute name", aloc);
                }
                skip_ws();
                const std::string val = parse_attr_value();
                if (!attrs.emplace(key, val).second) {
                    throw SchemaError("malformed XML: duplicate attribute '" + key + "'", aloc);
                }
            }

            ++item_count;
            if (item_count > kMaxXmlItems) {
                throw SchemaError("XML item count exceeds " + std::to_string(kMaxXmlItems) + " limit", tag_loc);
            }
            if (stack.size() + 1 > kMaxXmlDepth) {
                throw SchemaError("XML nesting depth exceeds " + std::to_string(kMaxXmlDepth) + " limit", tag_loc);
            }
            if (stack.empty() && has_root) {
                throw SchemaError("multiple XML root elements", tag_loc);
            }

            XmlNode node{};
            node.name = tag_name;
            node.attrs = std::move(attrs);
            node.location = tag_loc;

            if (self_closing) {
                if (!stack.empty()) {
                    stack.back().children.push_back(std::move(node));
                } else {
                    root = std::move(node);
                    has_root = true;
                }
            } else {
                stack.push_back(std::move(node));
            }
        }

        if (!stack.empty()) {
            throw SchemaError("incomplete XML document", stack.back().location);
        }
        if (!has_root) {
            throw SchemaError("XML document is empty", Location{source_, 1, 1});
        }
        return root;
    }

    std::string_view data_;
    const std::string& source_;
    std::size_t pos_{0};
    std::size_t line_{1};
    std::size_t col_{1};
};

} // namespace nimblefix::codegen

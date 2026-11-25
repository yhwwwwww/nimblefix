#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

#include <nimblefix/schema.hpp>
#include <nimblefix/group.hpp>

namespace {

using namespace nimblefix;

int failures = 0;

#define CHECK(condition) do { if (!(condition)) { ++failures; std::cerr << "line " << __LINE__ << ": " << #condition << "\n"; } } while (false)

constexpr std::array<EnumValueDescriptor, 2> multi_char_values{{
    {106, "0", "zero"}, {106, "1", "one"}}};
constexpr std::array<FieldDescriptor, 7> fields{{
    {100, "OuterCount", "NUMINGROUP", FieldClass::unsigned_integer, WireType::integer},
    {101, "InnerCount", "NUMINGROUP", FieldClass::unsigned_integer, WireType::integer},
    {102, "InnerDelim", "STRING", FieldClass::text, WireType::string},
    {103, "OuterValue", "STRING", FieldClass::text, WireType::string},
    {104, "TradeDate", "UTCDATEONLY", FieldClass::utc_date, WireType::string},
    {105, "TagNumber", "TAGNUM", FieldClass::tag_number, WireType::integer},
    {106, "Directions", "MULTIPLECHARVALUE", FieldClass::multiple_char_value,
        WireType::string, multi_char_values}}};
constexpr std::array<MessageMemberDescriptor, 2> outer_members{{
    {MemberKind::group, 0, 1, false}, {MemberKind::field, 3, 0, false}}};
constexpr std::array<MessageMemberDescriptor, 1> nested_members{{
    {MemberKind::field, 2, 0, true}}};
constexpr std::array<MessageMemberDescriptor, 4> message_members{{
    {MemberKind::group, 0, 0, true}, {MemberKind::field, 4, 0, true},
    {MemberKind::field, 5, 0, false}, {MemberKind::field, 6, 0, false}}};
constexpr std::array<GroupDescriptor, 2> groups{{
    {"OuterCount", 100, 101, outer_members},
    {"InnerCount", 101, 102, nested_members}}};
constexpr std::array<MessageDescriptor, 1> messages{{
    {"NestedMessage", "Z", "app", message_members}}};
constexpr SchemaDescriptor schema{"FIX", "4.4", "schema-codec-test", fields, groups, {}, messages};

struct TestSchema {
    [[nodiscard]] static constexpr const SchemaDescriptor& descriptor() noexcept { return schema; }
};

} // namespace

namespace nimblefix {
template <> struct SchemaField<TestSchema, 102> : std::integral_constant<FieldClass, FieldClass::text> {
    static constexpr FieldClass field_class = FieldClass::text;
};
template <> struct SchemaField<TestSchema, 103> : std::integral_constant<FieldClass, FieldClass::text> {
    static constexpr FieldClass field_class = FieldClass::text;
};
} // namespace nimblefix

namespace {

std::string make_frame(std::string_view body) {
    std::string wire = "8=FIX.4.4\0019=" + std::to_string(body.size()) + "\001" + std::string(body);
    unsigned checksum = 0;
    for (const unsigned char byte : wire) checksum = (checksum + byte) & 0xffU;
    wire += "10=";
    wire += static_cast<char>('0' + checksum / 100U);
    wire += static_cast<char>('0' + (checksum / 10U) % 10U);
    wire += static_cast<char>('0' + checksum % 10U);
    wire += '\001';
    return wire;
}

Status parse_wire(std::string_view wire, std::array<FieldView, 24>& scratch,
    FlatMessageView& flat) {
    FrameView frame{};
    if (const auto status = inspect_frame(wire, frame); !status.ok()) return status;
    return parse_flat(frame, scratch, flat);
}

void test_scalar_validation() {
    CHECK(detail::is_valid_utc_timestamp("20261031-12:00:00"));
    CHECK(detail::is_valid_utc_timestamp("20261031-12:00:00.123"));
    CHECK(detail::is_valid_utc_timestamp("20261031-12:00:00.123456"));
    CHECK(detail::is_valid_utc_timestamp("20261031-12:00:00.123456789"));
    CHECK(!detail::is_valid_utc_timestamp("20261031-12:00:00.12"));
    CHECK(detail::valid_time("12:00:00.123456789"));
    CHECK(detail::valid_month_year("202610"));
    CHECK(detail::valid_month_year("20261031"));
    CHECK(detail::valid_month_year("202610w5"));
    CHECK(!detail::valid_month_year("202613"));
    CHECK(!detail::valid_month_year("20260230"));
    CHECK(!detail::valid_month_year("202610w0"));

    CHECK(is_valid_schema_field_value(fields[5], "900001"));
    CHECK(!is_valid_schema_field_value(fields[5], "0"));
    CHECK(!is_valid_schema_field_value(fields[5], "1000000000"));
    CHECK(is_valid_schema_field_value(fields[6], "0 1"));
    CHECK(!is_valid_schema_field_value(fields[6], "0 X"));
    CHECK(!is_valid_schema_field_value(fields[6], "01"));
}

void test_nested_group_projection_and_validation_levels() {
    const auto good_wire = make_frame(
        "35=Z\001100=1\001101=1\001102=inner\001103=outer\001"
        "104=20261031\001105=900001\001106=0 1\001");
    std::array<FieldView, 24> scratch{};
    FlatMessageView flat{};
    CHECK(parse_wire(good_wire, scratch, flat).ok());
    CHECK(validate_schema_message(schema, flat, ValidationLevel::strict).ok());

    TypedMessageView<TestSchema> typed(flat, messages[0]);
    CHECK(typed.get<102>().status().code == ErrorCode::missing_field);
    const auto outer = typed.group<100>();
    CHECK(outer.ok() && outer.value().size() == 1);
    if (!outer.ok()) return;
    const auto entry = outer.value().at(0);
    CHECK(entry.status().ok());
    const auto nested = entry.group<101>();
    CHECK(nested.ok() && nested.value().size() == 1);
    if (!nested.ok()) return;
    const auto nested_delimiter = nested.value().at(0).get<102>();
    const auto outer_value = entry.get<103>();
    CHECK(nested_delimiter.ok() && nested_delimiter.value() == "inner");
    CHECK(outer_value.ok() && outer_value.value() == "outer");

    const auto invalid_enum_wire = make_frame(
        "35=Z\001100=1\001101=1\001102=inner\001103=outer\001"
        "104=20261031\001105=900001\001106=0 2\001");
    CHECK(parse_wire(invalid_enum_wire, scratch, flat).ok());
    CHECK(validate_schema_message(schema, flat, ValidationLevel::required).ok());
    CHECK(validate_schema_message(schema, flat, ValidationLevel::strict).code == ErrorCode::invalid_value);

    const auto unknown_field_wire = make_frame(
        "35=Z\001100=1\001101=1\001102=inner\001103=outer\001"
        "104=20261031\001106=0\001999=extension\001");
    CHECK(parse_wire(unknown_field_wire, scratch, flat).ok());
    CHECK(validate_schema_message(schema, flat, ValidationLevel::required).ok());
    CHECK(validate_schema_message(schema, flat, ValidationLevel::strict).code == ErrorCode::invalid_value);

    const auto missing_group_wire = make_frame("35=Z\001104=20261031\001");
    CHECK(parse_wire(missing_group_wire, scratch, flat).ok());
    CHECK(validate_schema_message(schema, flat, ValidationLevel::structure).ok());
    CHECK(validate_schema_message(schema, flat, ValidationLevel::required).code == ErrorCode::required_field_missing);
}

void test_wide_group_dictionary_and_error_order() {
    // A valid custom message may have many independent groups. Its final
    // group and the ordering of malformed-group errors must remain visible.
    constexpr std::size_t count = 20;
    std::array<FieldDescriptor, 2 + count * 2> wide_fields{};
    std::array<std::array<MessageMemberDescriptor, 1>, count> entry_members{};
    std::array<GroupDescriptor, count> wide_groups{};
    std::array<MessageMemberDescriptor, count + 1> wide_members{};
    wide_fields[0] = {100, "Root", "CHAR", FieldClass::character, WireType::character};
    wide_members[0] = {MemberKind::field, 0, 0, true};
    for (std::size_t i = 0; i < count; ++i) {
        const auto count_tag = static_cast<std::uint32_t>(1000 + i * 2);
        wide_fields[1 + i * 2] = {count_tag, "Count", "NUMINGROUP",
            FieldClass::unsigned_integer, WireType::integer};
        wide_fields[2 + i * 2] = {count_tag + 1, "Entry", "STRING",
            FieldClass::text, WireType::string};
        entry_members[i][0] = {MemberKind::field, static_cast<std::uint32_t>(2 + i * 2), 0, true};
        wide_groups[i] = {"Group", count_tag, count_tag + 1, entry_members[i]};
        wide_members[i + 1] = {MemberKind::group, 0, static_cast<std::uint32_t>(i), false};
    }
    wide_fields.back() = {5000, "Signature", "STRING", FieldClass::text, WireType::string};
    const std::array<MessageMemberDescriptor, 1> trailer_members{{
        {MemberKind::field, static_cast<std::uint32_t>(wide_fields.size() - 1), 0, false}}};
    const std::array<ComponentDescriptor, 1> wide_components{{{"Trailer", trailer_members}}};
    const std::array<MessageDescriptor, 1> wide_messages{{{"Wide", "W", "app", wide_members}}};
    const SchemaDescriptor wide_schema{"FIX", "4.4", "wide-test", wide_fields,
        wide_groups, wide_components, wide_messages};
    const auto make_body = [&](std::string_view root, bool missing_root, bool malformed_last) {
        std::string body = "35=W\001";
        if (!missing_root) body += "100=" + std::string{root} + '\001';
        for (std::size_t i = 0; i < count; ++i) {
            const auto tag = 1000 + i * 2;
            body += std::to_string(tag) + "=" + (malformed_last && i + 1 == count ? "2" : "1") + '\001';
            body += std::to_string(tag + 1) + "=entry\001";
        }
        return body;
    };
    std::array<FieldView, 128> scratch{};
    FlatMessageView flat{};
    const auto parse = [&](const std::string& wire) {
        FrameView frame{};
        CHECK(inspect_frame(wire, frame).ok());
        CHECK(parse_flat(frame, scratch, flat).ok());
    };
    auto wire = make_frame(make_body("R", false, false));
    parse(wire);
    CHECK(validate_schema_message(wide_schema, flat).ok());
    const auto last = GroupView<TestSchema>::find(wide_schema, wide_members, flat.fields(), 1038);
    CHECK(last.ok() && last.value().size() == 1 && last.value().delimiter_tag() == 1039);
    if (last.ok()) CHECK(last.value().at(0).status().ok());
    Status lookup_status{};
    CHECK(detail::find_top_level_field(wide_schema, wide_messages[0], flat, 1039, lookup_status) == nullptr);
    CHECK(lookup_status.code == ErrorCode::missing_field);

    wire = make_frame(make_body("R", false, false) + "5000=signature\001");
    parse(wire);
    CHECK(validate_schema_message(wide_schema, flat).ok());

    wire = make_frame(make_body("BAD", false, false));
    parse(wire);
    auto status = validate_schema_message(wide_schema, flat);
    CHECK(status.code == ErrorCode::invalid_value && status.tag == 100);
    wire = make_frame(make_body("R", true, false));
    parse(wire);
    status = validate_schema_message(wide_schema, flat);
    CHECK(status.code == ErrorCode::required_field_missing && status.tag == 100);
    for (const bool missing_root : {false, true}) {
        wire = make_frame(make_body("BAD", missing_root, true));
        parse(wire);
        status = validate_schema_message(wide_schema, flat);
        CHECK(status.code == ErrorCode::invalid_value && status.tag == 1039);
    }
    wire = make_frame(make_body("R", false, false) + "100=S\001");
    parse(wire);
    status = validate_schema_message(wide_schema, flat);
    CHECK(status.code == ErrorCode::duplicate_field && status.tag == 100);
}

void test_bounded_group_builder_payload() {
    using namespace nimblefix;
    using PartyEntry = FixedMessage<"G", Fields<Field<448, FieldKind::string>>, 32>;
    GroupBuilder<453, 448, PartyEntry, 1> group;
    PartyEntry entry_msg;
    std::string transient = "PARTY-OWNED";
    CHECK(entry_msg.set<448>(std::string_view{transient}).ok());
    transient.assign("destroyed");
    CHECK(group.add(std::move(entry_msg)).ok());
    const auto payload = group.view();
    CHECK(payload.entry_count == 1);
    if (payload.entry_count != 1) return;
    const auto& entry = payload.entries[0];
    CHECK(entry.field_count == 1);
    CHECK(entry.value_bytes != nullptr);
    if (entry.value_bytes != nullptr)
        CHECK((std::string_view{entry.value_bytes, entry.value_byte_count} == "PARTY-OWNED"));
    PartyEntry second_msg;
    CHECK(second_msg.set<448>("SECOND").ok());
    CHECK(group.add(std::move(second_msg)).code == ErrorCode::capacity_exceeded);
}

} // namespace

int main() {
    test_scalar_validation();
    test_nested_group_projection_and_validation_levels();
    test_bounded_group_builder_payload();
    test_wide_group_dictionary_and_error_order();
    return failures == 0 ? 0 : 1;
}

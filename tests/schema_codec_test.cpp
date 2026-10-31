#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <nimblefix/schema.hpp>
#include <nimblefix/group.hpp>

namespace {

using namespace nimblefix;

int failures = 0;

#define CHECK(condition) do { if (!(condition)) ++failures; } while (false)

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

void test_bounded_owned_group_payload() {
    using namespace nimblefix;
    OwnedGroupBuilder<453, 448, 1> group;
    std::string transient = "PARTY-OWNED";
    const auto added = group.add(message<"G">().set<448>(std::string_view{transient}));
    CHECK(added.ok());
    transient.assign("destroyed");
    const auto payload = group.view();
    CHECK(payload.entry_count == 1);
    if (payload.entry_count != 1) return;
    const auto& entry = payload.entries[0];
    CHECK(entry.field_count == 1);
    CHECK(entry.value_bytes != nullptr);
    if (entry.value_bytes != nullptr)
        CHECK((std::string_view{entry.value_bytes, entry.value_byte_count} == "PARTY-OWNED"));
    CHECK(group.add(message<"G">().set<448>("SECOND")).code == ErrorCode::capacity_exceeded);
}

} // namespace

int main() {
    test_scalar_validation();
    test_nested_group_projection_and_validation_levels();
    test_bounded_owned_group_payload();
    return failures == 0 ? 0 : 1;
}

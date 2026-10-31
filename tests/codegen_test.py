import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "nimblefix_schema_codegen", ROOT / "tools" / "schema" / "generate.py"
)
codegen = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = codegen
SPEC.loader.exec_module(codegen)


class SchemaCodegenTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def write(self, name, text):
        path = self.directory / name
        path.write_text(text, encoding="utf-8")
        return path

    def schema_xml(self, fields, messages, root_attrs='type="FIX" major="4" minor="4"'):
        return (
            f"<fix {root_attrs}>"
            f"{fields}{messages}"
            "</fix>"
        )

    def test_sample_output_is_deterministic_and_has_expected_metadata(self):
        path = ROOT / "schemas" / "fix44-sample.xml"
        schema = codegen.load_schema([path])
        first = codegen.render_header(schema, "nimblefix::fix44_sample")
        second = codegen.render_header(codegen.load_schema([path]), "nimblefix::fix44_sample")

        self.assertEqual(first, second)
        self.assertIn("struct NewOrderSingle", first)
        self.assertIn("class NewOrderSingleBuilder", first)
        self.assertIn("struct Schema", first)
        self.assertIn('version = "4.4.0"', first)
        self.assertIn("cl_ord_id(V&& value)", first)
        self.assertIn("set<11>(std::forward<V>(value))", first)
        self.assertIn("WireRank<nimblefix::fix44_sample::NewOrderSinglePolicy, 11>", first)
        self.assertIn("FieldWireType<nimblefix::fix44_sample::NewOrderSinglePolicy, 60>", first)
        self.assertIn("WireType::utc_timestamp", first)
        self.assertIn("NewOrderSingle_required_tags", first)
        self.assertIn("TypedMessageView<Schema>", first)
        self.assertIn("explicit NewOrderSingleView(const ::nimblefix::FlatMessageView& flat)", first)
        self.assertIn("enum_value_allowed", first)
        self.assertIn('"1"', first)
        self.assertIn('"2"', first)
        self.assertIn("EnumValueDescriptor", first)
        self.assertNotIn(str(path.resolve()), first)

    def test_multiple_inputs_merge_independent_of_input_order(self):
        fields = self.write(
            "fields.xml",
            self.schema_xml(
                '<fields><field number="11" name="ClOrdID" type="STRING"/></fields>',
                "",
            ),
        )
        messages = self.write(
            "messages.xml",
            self.schema_xml(
                "",
                '<messages><message name="NewOrderSingle" msgtype="D">'
                '<field name="ClOrdID" required="Y"/></message></messages>',
            ),
        )
        forward = codegen.render_header(codegen.load_schema([fields, messages]), "custom::schema")
        reverse = codegen.render_header(codegen.load_schema([messages, fields]), "custom::schema")
        self.assertEqual(forward, reverse)

    def test_identical_definitions_are_deduplicated(self):
        one = self.write(
            "one.xml",
            self.schema_xml(
                '<fields><field number="11" name="ClOrdID" type="STRING"/></fields>',
                '<messages><message name="NewOrderSingle" msgtype="D">'
                '<field name="ClOrdID" required="Y"/></message></messages>',
            ),
        )
        two = self.write(
            "two.xml",
            self.schema_xml(
                '<fields><field number="11" name="ClOrdID" type="STRING"/></fields>',
                '<messages><message name="NewOrderSingle" msgtype="D">'
                '<field name="ClOrdID" required="Y"/></message></messages>',
            ),
        )
        schema = codegen.load_schema([one, two])
        self.assertEqual(1, len(schema.fields_by_name))
        self.assertEqual(1, len(schema.messages_by_name))
        forward = codegen.render_header(schema, "custom::schema")
        reverse = codegen.render_header(codegen.load_schema([two, one]), "custom::schema")
        self.assertEqual(forward, reverse)

    def test_conflicting_field_reports_both_locations(self):
        one = self.write(
            "one.xml",
            self.schema_xml('<fields><field number="11" name="ClOrdID" type="STRING"/></fields>', ""),
        )
        two = self.write(
            "two.xml",
            self.schema_xml('<fields><field number="11" name="ClOrdID" type="CHAR"/></fields>', ""),
        )
        with self.assertRaises(codegen.SchemaError) as caught:
            codegen.load_schema([two, one])
        error = str(caught.exception)
        self.assertIn("conflicting field definition", error)
        self.assertIn("one.xml:1:", error)
        self.assertIn("two.xml:1:", error)

    def test_conflicting_message_reports_both_locations(self):
        field = '<fields><field number="11" name="ClOrdID" type="STRING"/></fields>'
        one = self.write(
            "one.xml",
            self.schema_xml(field, '<messages><message name="NewOrderSingle" msgtype="D">'
                            '<field name="ClOrdID" required="Y"/></message></messages>'),
        )
        two = self.write(
            "two.xml",
            self.schema_xml(field, '<messages><message name="NewOrderSingle" msgtype="D">'
                            '<field name="ClOrdID" required="N"/></message></messages>'),
        )
        with self.assertRaises(codegen.SchemaError) as caught:
            codegen.load_schema([one, two])
        self.assertIn("conflicting message definition", str(caught.exception))
        self.assertIn("one.xml:1:", str(caught.exception))
        self.assertIn("two.xml:1:", str(caught.exception))

    def test_unknown_field_reference_is_rejected_at_reference_location(self):
        path = self.write(
            "unknown.xml",
            self.schema_xml(
                "<fields/>",
                '<messages><message name="NewOrderSingle" msgtype="D">'
                '<field name="NoSuchField" required="Y"/></message></messages>',
            ),
        )
        with self.assertRaises(codegen.SchemaError) as caught:
            codegen.load_schema([path])
        self.assertIn("unknown field reference 'NoSuchField'", str(caught.exception))
        self.assertIn("unknown.xml:1:", str(caught.exception))

    def test_fix_tag_limits_and_managed_message_tags_are_rejected(self):
        out_of_range = self.write(
            "out-of-range-tag.xml",
            self.schema_xml('<fields><field number="1000000000" name="CustomField" type="STRING"/></fields>', ""),
        )
        with self.assertRaises(codegen.SchemaError) as caught:
            codegen.load_schema([out_of_range])
        self.assertIn("field number must be between 1 and 999999999", str(caught.exception))

        managed = self.write(
            "managed-tag.xml",
            self.schema_xml(
                '<fields><field number="34" name="MsgSeqNum" type="INT"/></fields>',
                '<messages><message name="InvalidAppMessage" msgtype="Z">'
                '<field name="MsgSeqNum" required="Y"/></message></messages>',
            ),
        )
        with self.assertRaises(codegen.SchemaError) as caught:
            codegen.load_schema([managed])
        self.assertIn("engine-managed FIX tag 34", str(caught.exception))

    def test_dtd_entities_and_external_resources_are_rejected(self):
        inputs = [
            '<!DOCTYPE fix [<!ENTITY x "bad">]><fix><fields/><messages/></fix>',
            '<!DOCTYPE fix SYSTEM "https://invalid.example/schema.dtd"><fix><fields/><messages/></fix>',
        ]
        for index, xml in enumerate(inputs):
            with self.subTest(index=index):
                path = self.write(f"unsafe-{index}.xml", xml)
                with self.assertRaises(codegen.SchemaError) as caught:
                    codegen.load_schema([path])
                self.assertRegex(str(caught.exception), "DOCTYPE|entity|external")

    def test_namespace_is_rejected(self):
        path = self.write("namespaced.xml", '<fix xmlns="urn:fix"><fields/><messages/></fix>')
        with self.assertRaises(codegen.SchemaError) as caught:
            codegen.load_schema([path])
        self.assertIn("XML namespaces are not supported", str(caught.exception))

    def test_groups_generate_bounded_shape_local_group_api(self):
        path = self.write(
            "group.xml",
            self.schema_xml(
                '<fields><field number="11" name="ClOrdID" type="STRING"/>'
                '<field number="453" name="NoPartyIDs" type="NUMINGROUP"/>'
                '<field number="448" name="PartyID" type="STRING"/>'
                '<field number="802" name="NoPartySubIDs" type="NUMINGROUP"/>'
                '<field number="523" name="PartySubID" type="STRING"/></fields>',
                '<messages><message name="NewOrderSingle" msgtype="D">'
                '<field name="ClOrdID" required="Y"/><group name="NoPartyIDs" required="N">'
                '<field name="PartyID" required="Y"/><group name="NoPartySubIDs" required="N">'
                '<field name="PartySubID" required="Y"/></group></group>'
                '</message></messages>',
            ),
        )
        output = codegen.render_header(codegen.load_schema([path]), "nimblefix::fixture")
        self.assertIn("GroupDescriptor", output)
        self.assertIn("OwnedGroupBuilder<453u, 448u", output)
        self.assertIn("OwnedGroupBuilder<802u, 523u", output)
        self.assertIn("static_assert(std::remove_cvref_t<G>::count_tag == 453u", output)
        self.assertIn("GroupCountTag<nimblefix::fixture::NewOrderSinglePolicy, 453>", output)

    def test_data_field_type_generates_binary_data_metadata(self):
        path = self.write(
            "unsupported-type.xml",
            self.schema_xml(
                '<fields><field number="89" name="RawDataLength" type="LENGTH"/>'
                '<field number="90" name="RawData" type="DATA"/></fields>',
                '<messages><message name="RawMessage" msgtype="Z">'
                '<field name="RawData" required="N"/></message></messages>',
            ),
        )
        output = codegen.render_header(codegen.load_schema([path]), "nimblefix::fixture")
        self.assertIn("FieldClass::binary_data", output)
        self.assertIn('"RawData", "DATA"', output)

    def test_legacy_and_extended_scalar_types_and_nested_delimiter(self):
        path = self.write(
            "legacy-types.xml",
            self.schema_xml(
                '<fields>'
                '<field number="90" name="RawData" type="DATA"/>'
                '<field number="91" name="RawDataLength" type="LENGTH"/>'
                '<field number="212" name="XmlDataLen" type="LENGTH"/>'
                '<field number="213" name="XmlData" type="XMLDATA"/>'
                '<field number="100" name="OuterCount" type="INT"/>'
                '<field number="101" name="InnerCount" type="NUMINGROUP"/>'
                '<field number="102" name="InnerDelim" type="STRING"/>'
                '<field number="103" name="OuterValue" type="STRING"/>'
                '<field number="104" name="OldDate" type="UTCDATE"/>'
                '<field number="105" name="TagNumber" type="TAGNUM"/>'
                '<field number="106" name="Directions" type="MULTIPLECHARVALUE">'
                '<value enum="0"/><value enum="1"/></field>'
                '</fields>',
                '<messages><message name="LegacyMessage" msgtype="Z">'
                '<field name="RawData" required="N"/><field name="XmlData" required="N"/>'
                '<group name="OuterCount" required="N">'
                '<group name="InnerCount" required="N"><field name="InnerDelim" required="Y"/></group>'
                '<field name="OuterValue" required="N"/></group>'
                '<field name="OldDate" required="N"/><field name="TagNumber" required="N"/>'
                '<field name="Directions" required="N"/></message></messages>',
                root_attrs='type="FIX" major="4" minor="2"',
            ),
        )
        output = codegen.render_header(codegen.load_schema([path]), "nimblefix::legacy")
        self.assertIn("FieldClass::tag_number", output)
        self.assertIn("FieldClass::multiple_char_value", output)
        self.assertIn("FieldClass::utc_date", output)
        self.assertIn("WireType::binary_data", output)
        self.assertIn('{"OuterCount", 100u, 101u,', output)
        self.assertIn("LengthDataPair, 2", output)

    def test_all_vendored_standard_dictionaries_are_parseable(self):
        standard = ROOT / "schemas" / "standard"
        expected = {"FIX42.xml", "FIX43.xml", "FIX44.xml", "FIXT11.xml", "FIX50SP2.xml"}
        self.assertEqual(expected, {path.name for path in standard.glob("*.xml")})
        for name in sorted(expected):
            with self.subTest(name=name):
                schema = codegen.load_schema([standard / name])
                self.assertGreater(len(schema.fields_by_name), 0)
                self.assertGreater(len(schema.messages_by_name), 0)

    def test_fix43_duplicate_identical_field_reference_normalizes_once(self):
        schema = codegen.load_schema([ROOT / "schemas" / "standard" / "FIX43.xml"])
        members = schema._expanded_messages["RegistrationInstructions"]
        self.assertEqual(1, sum(member.field_name == "OwnershipType" for member in members))

    def test_supported_int_type_emits_integer_wire_metadata(self):
        path = self.write(
            "integer-type.xml",
            self.schema_xml(
                '<fields><field number="21" name="OrderRef" type="INT"/></fields>',
                '<messages><message name="OrderReference" msgtype="R">'
                '<field name="OrderRef" required="Y"/></message></messages>',
            ),
        )
        output = codegen.render_header(codegen.load_schema([path]), "nimblefix::fixture")
        self.assertIn("WireType::integer", output)

    def test_invalid_identifiers_and_namespaces_are_rejected(self):
        bad_message = self.write(
            "bad-name.xml",
            self.schema_xml(
                '<fields><field number="11" name="ClOrdID" type="STRING"/></fields>',
                '<messages><message name="class" msgtype="D">'
                '<field name="ClOrdID" required="Y"/></message></messages>',
            ),
        )
        with self.assertRaises(codegen.SchemaError):
            codegen.load_schema([bad_message])
        with self.assertRaises(codegen.SchemaError):
            codegen.validate_namespace("okay::class")

    def test_reserved_symbols_setters_and_double_underscores_are_rejected(self):
        field = '<fields><field number="11" name="ClOrdID" type="STRING"/></fields>'
        for index, name in enumerate(("FieldDescriptor", "EnumValueDescriptor", "fields", "enum_values", "Schema")):
            path = self.write(
                f"reserved-message-{index}.xml",
                self.schema_xml(
                    field,
                    f'<messages><message name="{name}" msgtype="D">'
                    '<field name="ClOrdID" required="Y"/></message></messages>',
                ),
            )
            with self.subTest(name=name), self.assertRaises(codegen.SchemaError) as caught:
                codegen.load_schema([path])
            self.assertIn("collides with generated C++ symbol", str(caught.exception))

        for index, field_name in enumerate(("View", "Status", "Reset", "MsgType", "SlotCount", "PlanType")):
            reserved_setter = self.write(
                f"reserved-setter-{index}.xml",
                self.schema_xml(
                    f'<fields><field number="58" name="{field_name}" type="STRING"/></fields>',
                    '<messages><message name="CustomMessage" msgtype="Z">'
                    f'<field name="{field_name}" required="N"/></message></messages>',
                ),
            )
            with self.subTest(field=field_name), self.assertRaises(codegen.SchemaError) as caught:
                codegen.load_schema([reserved_setter])
            self.assertIn("reserved builder setter", str(caught.exception))

        double_underscore = self.write(
            "double-underscore.xml",
            self.schema_xml(field, '<messages><message name="Bad__Name" msgtype="D">'
                            '<field name="ClOrdID" required="Y"/></message></messages>'),
        )
        with self.assertRaises(codegen.SchemaError) as caught:
            codegen.load_schema([double_underscore])
        self.assertIn("non-reserved ASCII identifier", str(caught.exception))
        with self.assertRaises(codegen.SchemaError):
            codegen.validate_namespace("okay::__detail")
        with self.assertRaises(codegen.SchemaError):
            codegen.validate_namespace("std::generated")

        core_collision = self.write(
            "core-collision.xml",
            self.schema_xml(
                '<fields><field number="11" name="ClOrdID" type="STRING"/></fields>',
                '<messages><message name="MessageBuilder" msgtype="D">'
                '<field name="ClOrdID" required="Y"/></message></messages>',
            ),
        )
        schema = codegen.load_schema([core_collision])
        with self.assertRaises(codegen.SchemaError) as caught:
            codegen.render_header(schema, "nimblefix")
        self.assertIn("collides with nimblefix core symbol", str(caught.exception))

    def test_cpp_string_escapes_utf8_without_surrogate_ucns(self):
        literal = codegen._cpp_string('quote " slash \\ emoji 😀')
        self.assertIn('\\"', literal)
        self.assertIn("\\\\", literal)
        self.assertIn("\\360\\237\\230\\200", literal)
        self.assertNotIn("\\ud83d", literal.lower())

    def test_depth_limit_is_enforced(self):
        nested = "<a>" * (codegen.MAX_XML_DEPTH + 2) + "</a>" * (codegen.MAX_XML_DEPTH + 2)
        path = self.write("deep.xml", f"<fix>{nested}</fix>")
        with self.assertRaises(codegen.SchemaError) as caught:
            codegen.load_schema([path])
        self.assertIn("nesting depth exceeds", str(caught.exception))

    def test_item_and_byte_limits_are_enforced(self):
        item_path = self.write(
            "items.xml",
            self.schema_xml("<fields><field number='11' name='ClOrdID' type='STRING'/></fields>", ""),
        )
        original_items = codegen.MAX_XML_ITEMS
        try:
            codegen.MAX_XML_ITEMS = 2
            with self.assertRaises(codegen.SchemaError) as caught:
                codegen.load_schema([item_path])
            self.assertIn("item count exceeds", str(caught.exception))
        finally:
            codegen.MAX_XML_ITEMS = original_items

        original_bytes = codegen.MAX_INPUT_BYTES
        oversized = self.write("oversized.xml", "<fix>" + (" " * 40) + "</fix>")
        try:
            codegen.MAX_INPUT_BYTES = 16
            with self.assertRaises(codegen.SchemaError) as caught:
                codegen.load_schema([oversized])
            self.assertIn("byte limit", str(caught.exception))
        finally:
            codegen.MAX_INPUT_BYTES = original_bytes

    def test_unsupported_nested_content_is_rejected(self):
        path = self.write(
            "nested.xml",
            self.schema_xml(
                '<fields><field number="11" name="ClOrdID" type="STRING"/></fields>',
                '<messages><message name="NewOrderSingle" msgtype="D">'
                '<field name="ClOrdID" required="Y"><extension/></field>'
                '</message></messages>',
            ),
        )
        with self.assertRaises(codegen.SchemaError) as caught:
            codegen.load_schema([path])
        self.assertIn("unexpected <extension>", str(caught.exception))

    def test_write_if_changed_preserves_existing_file(self):
        path = self.directory / "generated.hpp"
        self.assertTrue(codegen.write_if_changed(path, "first\n"))
        before = path.stat()
        self.assertFalse(codegen.write_if_changed(path, "first\n"))
        after = path.stat()
        self.assertEqual(before.st_mtime_ns, after.st_mtime_ns)
        self.assertTrue(codegen.write_if_changed(path, "second\n"))
        self.assertEqual("second\n", path.read_text(encoding="utf-8"))

    def test_cli_generates_to_requested_build_path_without_rewriting(self):
        source = ROOT / "schemas" / "fix44-sample.xml"
        output = self.directory / "build" / "generated" / "schema.hpp"
        args = [
            "--output",
            str(output),
            "--namespace",
            "nimblefix::fixture",
            str(source),
        ]
        self.assertEqual(0, codegen.main(args))
        first = output.stat()
        self.assertEqual(0, codegen.main(args))
        second = output.stat()
        self.assertEqual(first.st_mtime_ns, second.st_mtime_ns)
        self.assertNotIn(str(source.resolve()), output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()

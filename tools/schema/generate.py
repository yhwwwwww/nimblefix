#!/usr/bin/env python3
"""Generate small, header-only typed message wrappers from FIX XML.

The input language is intentionally a bounded subset of the FIX XML format:
field definitions and flat application messages. Supported FIX field types
are STRING, CHAR, INT, PRICE, QTY and UTCTIMESTAMP. Components, groups, other field
types and external XML resources are rejected rather than silently ignored.
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import re
import sys
import tempfile
import xml.parsers.expat as expat
from pathlib import Path
from typing import Iterable


MAX_INPUT_BYTES = 4 * 1024 * 1024
MAX_TOTAL_INPUT_BYTES = 16 * 1024 * 1024
MAX_INPUT_FILES = 32
MAX_XML_DEPTH = 64
MAX_XML_ITEMS = 50_000
MAX_FIX_TAG = 999_999_999
MANAGED_TAGS = frozenset({8, 9, 10, 34, 35, 49, 52, 56})

FIX_WIRE_TYPES = {
    "STRING": "string",
    "CHAR": "character",
    "INT": "integer",
    "PRICE": "decimal",
    "QTY": "decimal",
    "UTCTIMESTAMP": "utc_timestamp",
}

_IDENTIFIER = re.compile(r"[A-Za-z][A-Za-z0-9_]*\Z")
_CPP_KEYWORDS = frozenset(
    "alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept "
    "auto bitand bitor bool break case catch char char8_t char16_t char32_t "
    "class compl concept const consteval constexpr constinit const_cast continue "
    "co_await co_return co_yield decltype default delete do double dynamic_cast "
    "else enum explicit export extern false float for friend goto if inline int "
    "long mutable namespace new noexcept not not_eq nullptr operator or or_eq "
    "private protected public register reinterpret_cast requires return short "
    "signed sizeof static static_assert static_cast struct switch synchronized "
    "template this thread_local throw true try typedef typeid typename union "
    "unsigned using virtual void volatile wchar_t while xor xor_eq"
    .split()
)
_BUILDER_RESERVED_SETTERS = frozenset(
    {
        "make",
        "set",
        "status",
        "view",
        "reset",
        "msg_type",
        "slot_count",
        "slot_storage_bytes",
        "wire_order",
        "encoding_plan",
        "physical_slot_map",
        "shape_type",
        "policy_type",
        "canonical_shape_type",
        "plan_type",
    }
)
_NIMBLEFIX_ROOT_SYMBOLS = frozenset(
    {
        "Decimal",
        "ErrorCode",
        "Field",
        "FieldKind",
        "FieldList",
        "FieldWireType",
        "Fields",
        "FixedMessage",
        "FixedShapeMessage",
        "FixedString",
        "FlatMessagePolicy",
        "MessageBuilder",
        "MessageLimits",
        "Result",
        "StaticMessage",
        "Status",
        "WireRank",
        "WireType",
        "detail",
        "soh",
    }
)


@dataclasses.dataclass(frozen=True, order=True)
class Location:
    source: str
    line: int
    column: int

    def format(self) -> str:
        return f"{self.source}:{self.line}:{self.column}"


class SchemaError(Exception):
    """A user-facing input or generation error with optional XML location."""

    def __init__(self, message: str, location: Location | None = None):
        self.message = message
        self.location = location
        super().__init__(self.__str__())

    def __str__(self) -> str:
        if self.location is None:
            return self.message
        return f"{self.location.format()}: {self.message}"


@dataclasses.dataclass
class XmlNode:
    name: str
    attrs: dict[str, str]
    location: Location
    children: list["XmlNode"] = dataclasses.field(default_factory=list)
    text: list[str] = dataclasses.field(default_factory=list)


@dataclasses.dataclass(frozen=True)
class FieldDef:
    number: int
    name: str
    field_type: str
    enum_values: tuple[tuple[str, str], ...]
    location: Location

    def signature(self) -> tuple[object, ...]:
        return (self.number, self.name, self.field_type, self.enum_values)


@dataclasses.dataclass(frozen=True)
class MemberDef:
    field_name: str
    required: bool
    location: Location


@dataclasses.dataclass(frozen=True)
class MessageDef:
    name: str
    msg_type: str
    category: str
    members: tuple[MemberDef, ...]
    location: Location

    def signature(self) -> tuple[object, ...]:
        return (
            self.name,
            self.msg_type,
            self.category,
            tuple((member.field_name, member.required) for member in self.members),
        )


@dataclasses.dataclass
class ParsedDocument:
    source: str
    root_location: Location
    identity: tuple[tuple[str, str], ...]
    fields: list[FieldDef]
    messages: list[MessageDef]


@dataclasses.dataclass
class Schema:
    identity: tuple[tuple[str, str], ...]
    fields_by_name: dict[str, FieldDef]
    messages_by_name: dict[str, MessageDef]


def _read_bounded(path: Path) -> bytes:
    try:
        with path.open("rb") as stream:
            data = stream.read(MAX_INPUT_BYTES + 1)
    except OSError as exc:
        raise SchemaError(f"cannot read XML input: {exc.strerror or exc}") from exc
    if len(data) > MAX_INPUT_BYTES:
        raise SchemaError(f"XML input exceeds {MAX_INPUT_BYTES} byte limit")
    return data


def _parse_xml(data: bytes, source: str) -> XmlNode:
    stack: list[XmlNode] = []
    root: XmlNode | None = None
    item_count = 0

    def located(message: str) -> SchemaError:
        return SchemaError(
            message,
            Location(source, parser.CurrentLineNumber, parser.CurrentColumnNumber + 1),
        )

    def start_element(name: str, attrs: dict[str, str]) -> None:
        nonlocal root, item_count
        if ":" in name or "xmlns" in attrs or any(":" in key for key in attrs):
            raise located("XML namespaces are not supported by this generator")
        item_count += 1
        if item_count > MAX_XML_ITEMS:
            raise located(f"XML item count exceeds {MAX_XML_ITEMS} limit")
        if len(stack) + 1 > MAX_XML_DEPTH:
            raise located(f"XML nesting depth exceeds {MAX_XML_DEPTH} limit")
        node = XmlNode(
            name=name,
            attrs=dict(attrs),
            location=Location(
                source,
                parser.CurrentLineNumber,
                parser.CurrentColumnNumber + 1,
            ),
        )
        if stack:
            stack[-1].children.append(node)
        elif root is None:
            root = node
        else:
            raise located("multiple XML root elements")
        stack.append(node)

    def end_element(_name: str) -> None:
        if not stack:
            raise located("unexpected closing element")
        node = stack.pop()
        if "".join(node.text).strip():
            raise SchemaError(
                f"unexpected text inside <{node.name}>", node.location
            )

    def character_data(value: str) -> None:
        if stack:
            stack[-1].text.append(value)
        elif value.strip():
            raise located("text outside XML root element")

    def reject_doctype(*_args: object) -> None:
        raise located("DOCTYPE declarations are not allowed")

    def reject_entity(*_args: object) -> None:
        raise located("entity declarations are not allowed")

    def reject_external_entity(*_args: object) -> int:
        raise located("external XML resources are not allowed")

    parser = expat.ParserCreate()
    parser.StartElementHandler = start_element
    parser.EndElementHandler = end_element
    parser.CharacterDataHandler = character_data
    parser.StartDoctypeDeclHandler = reject_doctype
    parser.EntityDeclHandler = reject_entity
    parser.UnparsedEntityDeclHandler = reject_entity
    parser.ExternalEntityRefHandler = reject_external_entity
    parser.SetParamEntityParsing(expat.XML_PARAM_ENTITY_PARSING_NEVER)

    try:
        parser.Parse(data, True)
    except SchemaError:
        raise
    except expat.ExpatError as exc:
        raise SchemaError(
            f"malformed XML: {expat.ErrorString(exc.code)}",
            Location(source, exc.lineno, exc.offset + 1),
        ) from exc
    if root is None:
        raise SchemaError("XML document is empty", Location(source, 1, 1))
    if stack:
        raise SchemaError("incomplete XML document", stack[-1].location)
    return root


def _attrs(node: XmlNode, allowed: set[str], required: set[str] = frozenset()) -> dict[str, str]:
    unknown = set(node.attrs) - allowed
    missing = required - set(node.attrs)
    if unknown:
        key = sorted(unknown)[0]
        raise SchemaError(f"unexpected attribute {key!r} on <{node.name}>", node.location)
    if missing:
        key = sorted(missing)[0]
        raise SchemaError(f"missing required attribute {key!r} on <{node.name}>", node.location)
    return node.attrs


def _children(node: XmlNode, expected: str) -> list[XmlNode]:
    result = []
    for child in node.children:
        if child.name != expected:
            if child.name == "group":
                reason = "repeating groups are not supported in stage1 XML codegen"
            elif child.name == "component":
                reason = "components are not supported in stage1 XML codegen"
            else:
                reason = f"unsupported <{child.name}> structure under <{node.name}>"
            raise SchemaError(reason, child.location)
        result.append(child)
    return result


def _require_leaf(node: XmlNode) -> None:
    if not node.children:
        return
    child = node.children[0]
    if child.name == "group":
        reason = "repeating groups are not supported in stage1 XML codegen"
    elif child.name == "component":
        reason = "components are not supported in stage1 XML codegen"
    else:
        reason = f"unsupported <{child.name}> structure under <{node.name}>"
    raise SchemaError(reason, child.location)


def _validate_xml_name(value: str, what: str, location: Location) -> None:
    if not _IDENTIFIER.fullmatch(value) or value in _CPP_KEYWORDS or "__" in value:
        raise SchemaError(
            f"invalid C++ {what} identifier {value!r}; expected a non-reserved ASCII identifier",
            location,
        )


def _snake_case(name: str) -> str:
    value = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", name)
    value = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", value)
    return value.lower()


def _parse_document(data: bytes, source: str) -> ParsedDocument:
    root = _parse_xml(data, source)
    if root.name != "fix":
        raise SchemaError(f"expected <fix> root, found <{root.name}>", root.location)

    root_attrs = _attrs(root, {"type", "major", "minor", "servicepack"})
    if root_attrs.get("type", "FIX") != "FIX":
        raise SchemaError("only FIX XML dictionaries are supported", root.location)
    for key in ("major", "minor", "servicepack"):
        if key in root_attrs and not re.fullmatch(r"[0-9]+", root_attrs[key]):
            raise SchemaError(f"invalid FIX schema version attribute {key}={root_attrs[key]!r}", root.location)
    identity = tuple(sorted(root_attrs.items()))
    for part in root.children:
        if part.name not in {"fields", "messages"}:
            if part.name in {"header", "trailer", "components"}:
                reason = f"<{part.name}> is outside the supported stage1 schema subset"
            else:
                reason = f"unsupported <{part.name}> section"
            raise SchemaError(reason, part.location)
    field_sections = [part for part in root.children if part.name == "fields"]
    message_sections = [part for part in root.children if part.name == "messages"]
    if len(field_sections) > 1:
        raise SchemaError("only one <fields> section is allowed per XML document", field_sections[1].location)
    if len(message_sections) > 1:
        raise SchemaError("only one <messages> section is allowed per XML document", message_sections[1].location)

    fields: list[FieldDef] = []
    if field_sections:
        section = field_sections[0]
        _attrs(section, set())
        for node in _children(section, "field"):
            attrs = _attrs(node, {"number", "name", "type"}, {"number", "name", "type"})
            if not re.fullmatch(r"[0-9]+", attrs["number"]):
                raise SchemaError(f"invalid field number {attrs['number']!r}", node.location)
            try:
                number = int(attrs["number"], 10)
            except ValueError as exc:
                raise SchemaError(f"invalid field number {attrs['number']!r}", node.location) from exc
            if number <= 0 or number > MAX_FIX_TAG:
                raise SchemaError(f"field number must be between 1 and {MAX_FIX_TAG}", node.location)
            _validate_xml_name(attrs["name"], "field", node.location)
            if not re.fullmatch(r"[A-Z][A-Z0-9_]*", attrs["type"]):
                raise SchemaError(f"invalid FIX field type {attrs['type']!r}", node.location)
            if attrs["type"] not in FIX_WIRE_TYPES:
                supported = ", ".join(sorted(FIX_WIRE_TYPES))
                raise SchemaError(
                    f"unsupported FIX field type {attrs['type']!r}; supported flat types: {supported}",
                    node.location,
                )
            enum_values: list[tuple[str, str]] = []
            seen_enum: set[str] = set()
            for value_node in _children(node, "value"):
                _require_leaf(value_node)
                value_attrs = _attrs(value_node, {"enum", "description"}, {"enum"})
                enum = value_attrs["enum"]
                if not enum or "\x01" in enum or "\n" in enum or "\r" in enum:
                    raise SchemaError("enum value must be non-empty and contain no control delimiter", value_node.location)
                if enum in seen_enum:
                    raise SchemaError(f"duplicate enum value {enum!r}", value_node.location)
                seen_enum.add(enum)
                enum_values.append((enum, value_attrs.get("description", "")))
            fields.append(
                FieldDef(
                    number,
                    attrs["name"],
                    attrs["type"],
                    tuple(sorted(enum_values)),
                    node.location,
                )
            )

    messages: list[MessageDef] = []
    if message_sections:
        section = message_sections[0]
        _attrs(section, set())
        for node in _children(section, "message"):
            attrs = _attrs(node, {"name", "msgtype", "msgcat"}, {"name", "msgtype"})
            _validate_xml_name(attrs["name"], "message", node.location)
            msg_type = attrs["msgtype"]
            if not msg_type or len(msg_type) > 16 or any(
                ord(ch) < 0x21 or ord(ch) > 0x7E or ch == "\x01" for ch in msg_type
            ):
                raise SchemaError("msgtype must be 1 to 16 printable ASCII characters", node.location)
            category = attrs.get("msgcat", "")
            if category and category not in {"admin", "app"}:
                raise SchemaError("msgcat must be 'admin' or 'app' when provided", node.location)
            members: list[MemberDef] = []
            seen_member_names: set[str] = set()
            for member_node in _children(node, "field"):
                _require_leaf(member_node)
                member_attrs = _attrs(member_node, {"name", "required"}, {"name"})
                _validate_xml_name(member_attrs["name"], "field reference", member_node.location)
                if member_attrs["name"] in seen_member_names:
                    raise SchemaError(f"duplicate field {member_attrs['name']!r} in message", member_node.location)
                seen_member_names.add(member_attrs["name"])
                required = member_attrs.get("required", "N")
                if required not in {"Y", "N"}:
                    raise SchemaError("required must be 'Y' or 'N'", member_node.location)
                members.append(MemberDef(member_attrs["name"], required == "Y", member_node.location))
            if not members:
                raise SchemaError("message must declare at least one field", node.location)
            messages.append(MessageDef(attrs["name"], msg_type, category, tuple(members), node.location))

    if not field_sections and not message_sections:
        raise SchemaError("document must contain <fields> and/or <messages>", root.location)
    return ParsedDocument(source, root.location, identity, fields, messages)


def _locations(locations: Iterable[Location]) -> str:
    return " and ".join(loc.format() for loc in sorted(locations))


def load_schema(paths: list[Path]) -> Schema:
    if not paths:
        raise SchemaError("at least one XML input is required")
    if len(paths) > MAX_INPUT_FILES:
        raise SchemaError(f"XML input count exceeds {MAX_INPUT_FILES} limit")
    docs: list[ParsedDocument] = []
    total = 0
    for path in paths:
        data = _read_bounded(path)
        total += len(data)
        if total > MAX_TOTAL_INPUT_BYTES:
            raise SchemaError(f"combined XML inputs exceed {MAX_TOTAL_INPUT_BYTES} byte limit")
        # The displayed source is exactly the supplied spelling, never an absolute path
        # inserted by the generator into generated output.
        docs.append(_parse_document(data, os.fspath(path)))

    identities = {doc.identity for doc in docs}
    if len(identities) > 1:
        locations = _locations(doc.root_location for doc in docs)
        raise SchemaError(
            "conflicting FIX schema identity/version across XML inputs: "
            + locations
        )

    fields_by_name: dict[str, FieldDef] = {}
    fields_by_number: dict[int, FieldDef] = {}
    for doc in docs:
        for field in doc.fields:
            previous_name = fields_by_name.get(field.name)
            previous_number = fields_by_number.get(field.number)
            previous = previous_name or previous_number
            if previous is not None and previous.signature() != field.signature():
                raise SchemaError(
                    f"conflicting field definition for {field.name!r} (tag {field.number}); "
                    f"definitions at {_locations((previous.location, field.location))}"
                )
            if previous is None:
                fields_by_name[field.name] = field
                fields_by_number[field.number] = field
            else:
                fields_by_name.setdefault(field.name, previous)
                fields_by_number.setdefault(field.number, previous)

    messages_by_name: dict[str, MessageDef] = {}
    messages_by_type: dict[str, MessageDef] = {}
    for doc in docs:
        for message in doc.messages:
            previous_name = messages_by_name.get(message.name)
            previous_type = messages_by_type.get(message.msg_type)
            previous = previous_name or previous_type
            if previous is not None and previous.signature() != message.signature():
                raise SchemaError(
                    f"conflicting message definition for {message.name!r} (MsgType {message.msg_type!r}); "
                    f"definitions at {_locations((previous.location, message.location))}"
                )
            if previous is None:
                messages_by_name[message.name] = message
                messages_by_type[message.msg_type] = message
            else:
                messages_by_name.setdefault(message.name, previous)
                messages_by_type.setdefault(message.msg_type, previous)

    if not messages_by_name:
        raise SchemaError("merged XML inputs contain no <message> definitions")

    for message in messages_by_name.values():
        for member in message.members:
            field = fields_by_name.get(member.field_name)
            if field is None:
                raise SchemaError(
                    f"unknown field reference {member.field_name!r} in message {message.name!r}",
                    member.location,
                )
            if field.number in MANAGED_TAGS:
                raise SchemaError(
                    f"message {message.name!r} references engine-managed FIX tag {field.number}",
                    member.location,
                )

    # Detect names that map to the same generated snake_case setter.
    for message in messages_by_name.values():
        setters: dict[str, MemberDef] = {}
        for member in message.members:
            setter = _snake_case(member.field_name)
            if setter in _BUILDER_RESERVED_SETTERS:
                raise SchemaError(
                    f"field {member.field_name!r} generates reserved builder setter {setter!r}",
                    member.location,
                )
            if setter in setters:
                other = setters[setter]
                raise SchemaError(
                    f"field names {other.field_name!r} and {member.field_name!r} both generate setter {setter!r}",
                    member.location,
                )
            _validate_xml_name(setter, "setter", member.location)
            setters[setter] = member

    generated_symbols = {
        "FieldDescriptor",
        "EnumValueDescriptor",
        "fields",
        "enum_values",
        "Schema",
    }
    for message in messages_by_name.values():
        for symbol in (
            message.name,
            f"{message.name}Policy",
            f"{message.name}Builder",
            f"{message.name}_allowed_tags",
            f"{message.name}_required_tags",
        ):
            if symbol in generated_symbols:
                raise SchemaError(
                    f"message {message.name!r} collides with generated C++ symbol {symbol!r}",
                    message.location,
                )
            generated_symbols.add(symbol)

    return Schema(next(iter(identities)), fields_by_name, messages_by_name)


def _cpp_string(value: str) -> str:
    escaped = ['"']
    for byte in value.encode("utf-8"):
        if byte == 0x22:
            escaped.append('\\"')
        elif byte == 0x5C:
            escaped.append("\\\\")
        elif byte == 0x0A:
            escaped.append("\\n")
        elif byte == 0x0D:
            escaped.append("\\r")
        elif byte == 0x09:
            escaped.append("\\t")
        elif 0x20 <= byte <= 0x7E:
            escaped.append(chr(byte))
        else:
            # Exactly three octal digits prevent a following digit from extending
            # the escape. This preserves UTF-8 bytes without surrogate UCNs.
            escaped.append(f"\\{byte:03o}")
    escaped.append('"')
    return "".join(escaped)


def validate_namespace(value: str) -> tuple[str, ...]:
    parts = value.split("::")
    if not parts or any(
        not _IDENTIFIER.fullmatch(part) or part in _CPP_KEYWORDS or "__" in part
        for part in parts
    ):
        raise SchemaError(f"invalid C++ namespace {value!r}; expected identifiers separated by '::'")
    if parts[0] == "std":
        raise SchemaError("generated types cannot be added to the reserved std namespace")
    return tuple(parts)


def render_header(schema: Schema, namespace: str) -> str:
    """Render a stable C++ header; final C++ spellings track message.hpp."""
    ns_parts = validate_namespace(namespace)
    ns = "::".join(ns_parts)
    if ns_parts == ("nimblefix",):
        collisions = sorted(
            message.name
            for message in schema.messages_by_name.values()
            if message.name in _NIMBLEFIX_ROOT_SYMBOLS
        )
        if collisions:
            raise SchemaError(
                "generated message name collides with nimblefix core symbol(s): "
                + ", ".join(collisions)
            )
    major = dict(schema.identity).get("major", "?")
    minor = dict(schema.identity).get("minor", "?")
    lines = [
        "// Generated by tools/schema/generate.py. Do not edit.",
        f"// FIX schema version {major}.{minor}; contains only the supplied XML subset.",
        "// Supported XML field types: STRING, CHAR, INT, PRICE, QTY, UTCTIMESTAMP.",
        "// Stage 1 emits typed send builders plus required/enum metadata and enforcement; typed receive projection is not generated.",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <string_view>",
        "#include <type_traits>",
        "#include <utility>",
        "#include <nimblefix/message.hpp>",
        "",
        f"namespace {ns} {{",
        "",
        "struct FieldDescriptor {",
        "    std::uint32_t tag;",
        "    std::string_view name;",
        "    std::string_view type;",
        "};",
        "",
        "struct EnumValueDescriptor {",
        "    std::uint32_t tag;",
        "    std::string_view value;",
        "    std::string_view description;",
        "};",
        "",
        f"inline constexpr std::array<FieldDescriptor, {len(schema.fields_by_name)}> fields{{{{",
    ]
    for field in sorted(schema.fields_by_name.values(), key=lambda value: value.number):
        lines.append(
            f"    {{{field.number}u, {_cpp_string(field.name)}, {_cpp_string(field.field_type)}}},"
        )
    version_parts = [dict(schema.identity).get(key, "") for key in ("major", "minor", "servicepack")]
    version = ".".join(part for part in version_parts if part)
    enum_values = [
        (field.number, value, description)
        for field in sorted(schema.fields_by_name.values(), key=lambda item: item.number)
        for value, description in field.enum_values
    ]
    lines += [
        "}};",
        "",
        f"inline constexpr std::array<EnumValueDescriptor, {len(enum_values)}> enum_values{{{{",
    ]
    for tag, value, description in enum_values:
        lines.append(f"    {{{tag}u, {_cpp_string(value)}, {_cpp_string(description)}}},")
    lines += [
        "}};",
        "",
        "struct Schema {",
        f"    static constexpr std::string_view type = {_cpp_string(dict(schema.identity).get('type', 'FIX'))};",
        f"    static constexpr std::string_view version = {_cpp_string(version)};",
        f"    static constexpr std::size_t field_count = {len(schema.fields_by_name)};",
        f"    static constexpr std::size_t enum_value_count = {len(enum_values)};",
        f"    static constexpr std::size_t message_count = {len(schema.messages_by_name)};",
        f"    inline static constexpr auto fields = ::{ns}::fields;",
        f"    inline static constexpr auto enum_values = ::{ns}::enum_values;",
        "};",
        "",
    ]

    messages = sorted(schema.messages_by_name.values(), key=lambda value: (value.name, value.msg_type))
    for message in messages:
        name = message.name
        policy = f"{name}Policy"
        builder = f"{name}Builder"
        allowed_tags = [schema.fields_by_name[member.field_name].number for member in message.members]
        required_tags = [
            schema.fields_by_name[member.field_name].number
            for member in message.members
            if member.required
        ]
        lines += [
            f"template<class... Slots> class {builder};",
            f"struct {policy} {{",
            "    template<class... Slots>",
            f"    using rebind = {builder}<Slots...>;",
            f"    static constexpr std::string_view msg_type = {_cpp_string(message.msg_type)};",
            f"    inline static constexpr std::array<std::uint32_t, {len(allowed_tags)}> allowed_tags{{{{",
        ]
        for tag in allowed_tags:
            lines.append(f"        {tag}u,")
        lines += [
            "    }};",
            f"    inline static constexpr std::array<std::uint32_t, {len(required_tags)}> required_tags{{{{",
        ]
        for tag in required_tags:
            lines.append(f"        {tag}u,")
        lines += [
            "    }};",
            "    static constexpr bool enum_value_allowed(std::uint32_t tag, std::string_view value) noexcept {",
            "        switch (tag) {",
        ]
        for member in message.members:
            field = schema.fields_by_name[member.field_name]
            if field.enum_values:
                acceptable = " || ".join(
                    f"value == {_cpp_string(enum_value)}"
                    for enum_value, _description in field.enum_values
                )
                lines += [
                    f"        case {field.number}u:",
                    f"            return {acceptable};",
                ]
        lines += [
            "        default:",
            "            return true;",
            "        }",
            "    }",
            "};",
            "",
            f"struct {name} {{",
            f"    using policy_type = {policy};",
            f"    static constexpr std::string_view msg_type = {_cpp_string(message.msg_type)};",
            f"    static auto make(::nimblefix::MessageLimits limits = {{}});",
            "};",
            "",
            "template<class... Slots>",
            f"class {builder} : public ::nimblefix::MessageBuilder<{policy}, { _cpp_string(message.msg_type) }, ::nimblefix::FieldList<Slots...>> {{",
            f"    using Base = ::nimblefix::MessageBuilder<{policy}, { _cpp_string(message.msg_type) }, ::nimblefix::FieldList<Slots...>>;",
            "public:",
            "    using Base::Base;",
        ]
        for member in message.members:
            field = schema.fields_by_name[member.field_name]
            setter = _snake_case(field.name)
            lines += [
                "    template<class V>",
                f"    auto {setter}(V&& value) && {{",
                f"        return std::move(*this).template set<{field.number}>(std::forward<V>(value));",
                "    }",
            ]
        lines += ["};", f"inline auto {name}::make(::nimblefix::MessageLimits limits) {{ return {builder}<>{{limits}}; }}", ""]

        # The codec defines WireRank in namespace nimblefix. Specializations
        # are emitted below after closing the user namespace.

    lines += [f"}} // namespace {ns}", ""]
    lines += ["namespace nimblefix {"]
    for message in messages:
        policy = f"{ns}::{message.name}Policy"
        for rank, member in enumerate(message.members):
            field = schema.fields_by_name[member.field_name]
            lines += [
                "template<>",
                f"struct WireRank<{policy}, {field.number}> : std::integral_constant<std::size_t, {rank}> {{}};",
                "template<>",
                f"struct FieldWireType<{policy}, {field.number}> : std::integral_constant<WireType, WireType::{FIX_WIRE_TYPES[field.field_type]}> {{}};",
            ]
    lines += ["} // namespace nimblefix", ""]

    # Public validation metadata is kept separate from shape-local instances.
    lines += [f"namespace {ns} {{"]
    for message in messages:
        ident = message.name
        lines += [
            f"inline constexpr std::array<std::uint32_t, {len(message.members)}> {ident}_allowed_tags{{{{",
        ]
        for member in message.members:
            lines.append(f"    {schema.fields_by_name[member.field_name].number}u,")
        lines += ["}};", f"inline constexpr std::array<std::uint32_t, {sum(m.required for m in message.members)}> {ident}_required_tags{{{{"]
        for member in message.members:
            if member.required:
                lines.append(f"    {schema.fields_by_name[member.field_name].number}u,")
        lines += ["}};", ""]
    lines += [f"}} // namespace {ns}", ""]
    return "\n".join(lines)


def write_if_changed(output: Path, content: str) -> bool:
    data = content.encode("utf-8")
    try:
        if output.read_bytes() == data:
            return False
    except FileNotFoundError:
        pass
    except OSError as exc:
        raise SchemaError(f"cannot read output {output}: {exc.strerror or exc}") from exc
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary = tempfile.mkstemp(prefix=f".{output.name}.", dir=output.parent)
        try:
            with os.fdopen(fd, "wb") as stream:
                os.fchmod(stream.fileno(), 0o644)
                stream.write(data)
                stream.flush()
            os.replace(temporary, output)
        except BaseException:
            try:
                os.unlink(temporary)
            except OSError:
                pass
            raise
    except OSError as exc:
        raise SchemaError(f"cannot write output {output}: {exc.strerror or exc}") from exc
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="generated C++ header path")
    parser.add_argument("--namespace", required=True, help="C++ namespace for generated types")
    parser.add_argument("xml", nargs="+", type=Path, help="FIX XML schema input(s)")
    args = parser.parse_args(argv)
    try:
        schema = load_schema(args.xml)
        header = render_header(schema, args.namespace)
        write_if_changed(args.output, header)
    except SchemaError as exc:
        print(f"schema codegen: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

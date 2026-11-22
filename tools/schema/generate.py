#!/usr/bin/env python3
"""Generate bounded, header-only typed message wrappers from FIX XML."""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
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
MANAGED_TAGS = frozenset({8, 9, 10, 34, 35, 43, 49, 52, 56, 122, 369})

FIX_WIRE_TYPES = {
    "STRING": "string",
    "COUNTRY": "string",
    "CURRENCY": "string",
    "EXCHANGE": "string",
    "MULTIPLEVALUESTRING": "string",
    "MULTIPLECHARVALUE": "string",
    "MULTIPLESTRINGVALUE": "string",
    "LANGUAGE": "string",
    "LOCALMKTTIME": "string",
    "TZTIMEONLY": "string",
    "TZTIMESTAMP": "string",
    "XID": "string",
    "XIDREF": "string",
    "XMLDATA": "binary_data",
    "CHAR": "character",
    "BOOLEAN": "character",
    "INT": "integer",
    "LENGTH": "integer",
    "NUMINGROUP": "integer",
    "SEQNUM": "integer",
    "DAYOFMONTH": "integer",
    "TAGNUM": "integer",
    "AMT": "decimal",
    "FLOAT": "decimal",
    "PERCENTAGE": "decimal",
    "PRICE": "decimal",
    "PRICEOFFSET": "decimal",
    "QTY": "decimal",
    "UTCTIMESTAMP": "utc_timestamp",
    "UTCDATEONLY": "string",
    "UTCDATE": "string",
    "LOCALMKTDATE": "string",
    "UTCTIMEONLY": "string",
    "MONTHYEAR": "string",
    "DATA": "binary_data",
}

FIX_FIELD_CLASSES = {
    "STRING": "text", "COUNTRY": "text", "CURRENCY": "text", "EXCHANGE": "text",
    "LANGUAGE": "text", "LOCALMKTTIME": "text", "TZTIMEONLY": "text", "TZTIMESTAMP": "text",
    "XID": "text", "XIDREF": "text", "MULTIPLEVALUESTRING": "multiple_value_string",
    "MULTIPLECHARVALUE": "multiple_char_value", "MULTIPLESTRINGVALUE": "multiple_string_value",
    "CHAR": "character", "BOOLEAN": "boolean",
    "INT": "signed_integer", "LENGTH": "unsigned_integer", "NUMINGROUP": "unsigned_integer",
    "SEQNUM": "unsigned_integer", "TAGNUM": "tag_number", "DAYOFMONTH": "day_of_month", "AMT": "decimal",
    "FLOAT": "decimal", "PERCENTAGE": "decimal", "PRICE": "decimal", "PRICEOFFSET": "decimal",
    "QTY": "decimal", "UTCTIMESTAMP": "utc_timestamp", "UTCDATE": "utc_date", "UTCDATEONLY": "utc_date",
    "LOCALMKTDATE": "local_market_date", "UTCTIMEONLY": "utc_time", "MONTHYEAR": "month_year",
    "DATA": "binary_data", "XMLDATA": "binary_data",
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
    kind: str = "field"
    members: tuple["MemberDef", ...] = ()

    def signature(self) -> tuple[object, ...]:
        return (self.kind, self.field_name,
                tuple(member.signature() for member in self.members), self.required)


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
            tuple(member.signature() for member in self.members),
        )


@dataclasses.dataclass(frozen=True)
class ComponentDef:
    name: str
    members: tuple[MemberDef, ...]
    location: Location

    def signature(self) -> tuple[object, ...]:
        return (self.name, tuple(member.signature() for member in self.members))


@dataclasses.dataclass
class ParsedDocument:
    source: str
    root_location: Location
    identity: tuple[tuple[str, str], ...]
    fields: list[FieldDef]
    messages: list[MessageDef]
    components: list[ComponentDef]


@dataclasses.dataclass
class Schema:
    identity: tuple[tuple[str, str], ...]
    fields_by_name: dict[str, FieldDef]
    messages_by_name: dict[str, MessageDef]
    components_by_name: dict[str, ComponentDef]


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
    if root_attrs.get("type", "FIX") not in {"FIX", "FIXT"}:
        raise SchemaError("only FIX and FIXT XML dictionaries are supported", root.location)
    for key in ("major", "minor", "servicepack"):
        if key in root_attrs and not re.fullmatch(r"[0-9]+", root_attrs[key]):
            raise SchemaError(f"invalid FIX schema version attribute {key}={root_attrs[key]!r}", root.location)
    identity = tuple(sorted(root_attrs.items()))
    field_sections = [part for part in root.children if part.name == "fields"]
    message_sections = [part for part in root.children if part.name == "messages"]
    component_sections = [part for part in root.children if part.name == "components"]
    headers = [part for part in root.children if part.name == "header"]
    trailers = [part for part in root.children if part.name == "trailer"]
    known_sections = {"fields", "messages", "components", "header", "trailer"}
    for part in root.children:
        if part.name not in known_sections:
            raise SchemaError(f"unsupported <{part.name}> section", part.location)
    if len(field_sections) > 1:
        raise SchemaError("only one <fields> section is allowed per XML document", field_sections[1].location)
    if len(message_sections) > 1:
        raise SchemaError("only one <messages> section is allowed per XML document", message_sections[1].location)
    if len(component_sections) > 1:
        raise SchemaError("only one <components> section is allowed per XML document", component_sections[1].location)
    if len(headers) > 1 or len(trailers) > 1:
        raise SchemaError("only one <header> and <trailer> section is allowed per XML document", root.location)

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
                    f"unsupported FIX field type {attrs['type']!r}; supported scalar types: {supported}",
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

    components: list[ComponentDef] = []
    if component_sections:
        section = component_sections[0]
        _attrs(section, set())
        for node in _children(section, "component"):
            attrs = _attrs(node, {"name"}, {"name"})
            _validate_xml_name(attrs["name"], "component", node.location)
            components.append(ComponentDef(attrs["name"], _parse_members(node), node.location))
    for section, name in ((headers[0], "Header") if headers else (None, "Header"),
                          (trailers[0], "Trailer") if trailers else (None, "Trailer")):
        if section is not None:
            _attrs(section, set())
            components.append(ComponentDef(name, _parse_members(section), section.location))

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
            members = _parse_members(node)
            messages.append(MessageDef(attrs["name"], msg_type, category, tuple(members), node.location))

    if not field_sections and not message_sections and not component_sections:
        raise SchemaError("document must contain <fields>, <components> and/or <messages>", root.location)
    return ParsedDocument(source, root.location, identity, fields, messages, components)


def _parse_members(parent: XmlNode) -> list[MemberDef]:
    members: list[MemberDef] = []
    seen: dict[tuple[str, str], MemberDef] = {}
    for node in parent.children:
        if node.name not in {"field", "component", "group"}:
            raise SchemaError(f"unsupported <{node.name}> structure under <{parent.name}>", node.location)
        attrs = _attrs(node, {"name", "required"}, {"name"})
        _validate_xml_name(attrs["name"], f"{node.name} reference", node.location)
        required = attrs.get("required", "N")
        if required not in {"Y", "N"}:
            raise SchemaError("required must be 'Y' or 'N'", node.location)
        kind = "component" if node.name == "component" else "group" if node.name == "group" else "field"
        key = (kind, attrs["name"])
        children = tuple(_parse_members(node)) if kind == "group" else ()
        if kind != "group" and node.children:
            child = node.children[0]
            raise SchemaError(f"unexpected <{child.name}> structure under <{node.name}>", child.location)
        member = MemberDef(attrs["name"], required == "Y", node.location, kind, children)
        previous = seen.get(key)
        if previous is not None:
            # QuickFIX FIX 4.3 repeats OwnershipType as an identical field
            # reference in RegistrationInstructions. Normalize exact duplicate
            # field refs once; repeated groups/components and conflicting field
            # requiredness remain errors.
            if kind == "field" and previous.required == member.required:
                continue
            raise SchemaError(f"duplicate {kind} reference {attrs['name']!r} under <{parent.name}>", node.location)
        seen[key] = member
        members.append(member)
    return members


def _locations(locations: Iterable[Location]) -> str:
    return " and ".join(loc.format() for loc in sorted(locations))


def _expand_members(members: tuple[MemberDef, ...] | list[MemberDef],
                    components: dict[str, ComponentDef],
                    inherited_required: bool = True,
                    stack: tuple[str, ...] = (),
                    budget: list[int] | None = None) -> tuple[MemberDef, ...]:
    if budget is None:
        budget = [100_000]
    expanded: list[MemberDef] = []
    for member in members:
        if budget[0] <= 0:
            raise SchemaError("expanded component member limit exceeded", member.location)
        budget[0] -= 1
        required = inherited_required and member.required
        if member.kind == "component":
            component = components[member.field_name]
            if component.name in stack:
                chain = " -> ".join((*stack, component.name))
                raise SchemaError(f"recursive component reference: {chain}", member.location)
            expanded.extend(_expand_members(component.members, components, required, (*stack, component.name), budget))
        elif member.kind == "group":
            expanded.append(dataclasses.replace(member, required=required,
                members=_expand_members(member.members, components, True, stack, budget)))
        else:
            expanded.append(dataclasses.replace(member, required=required))
    return tuple(expanded)


def _check_member_references(members: tuple[MemberDef, ...] | list[MemberDef],
                             fields: dict[str, FieldDef], components: dict[str, ComponentDef],
                             context: str, stack: tuple[str, ...] = (),
                             verified_components: set[str] | None = None) -> None:
    if verified_components is None:
        verified_components = set()
    for member in members:
        if member.kind == "field":
            if member.field_name not in fields:
                raise SchemaError(f"unknown field reference {member.field_name!r} in {context}", member.location)
        elif member.kind == "component":
            component = components.get(member.field_name)
            if component is None:
                raise SchemaError(f"unknown component reference {member.field_name!r} in {context}", member.location)
            if component.name in stack:
                chain = " -> ".join((*stack, component.name))
                raise SchemaError(f"recursive component reference: {chain}", member.location)
            if component.name in verified_components:
                continue
            _check_member_references(component.members, fields, components, context,
                (*stack, component.name), verified_components)
            verified_components.add(component.name)
        elif member.kind == "group":
            count_field = fields.get(member.field_name)
            if count_field is None:
                raise SchemaError(f"unknown repeating group counter field {member.field_name!r} in {context}", member.location)
            # FIX 4.2 defines some repeating-group counters as INT; later
            # dictionaries use NUMINGROUP for the same wire role.
            if count_field.field_type not in {"NUMINGROUP", "INT"}:
                raise SchemaError(f"repeating group counter {member.field_name!r} must have NUMINGROUP or legacy INT type", member.location)
            _check_member_references(member.members, fields, components, context, stack, verified_components)


def _validate_member_scope(members: tuple[MemberDef, ...], fields: dict[str, FieldDef],
                           context: str, allow_managed: bool = False) -> None:
    seen: dict[int, MemberDef] = {}
    for member in members:
        field = fields[member.field_name]
        if field.number in seen:
            raise SchemaError(f"duplicate field tag {field.number} in {context}", member.location)
        seen[field.number] = member
        if member.kind == "field" and not allow_managed and field.number in MANAGED_TAGS:
            raise SchemaError(f"{context} references engine-managed FIX tag {field.number}", member.location)
        if member.kind == "group":
            if not member.members or member.members[0].kind not in {"field", "group"}:
                raise SchemaError(f"group {member.field_name!r} must begin with a field delimiter", member.location)
            _validate_member_scope(member.members, fields, f"group {member.field_name!r}", allow_managed)


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
        raise SchemaError("conflicting FIX schema identity/version across XML inputs: " +
            _locations(doc.root_location for doc in docs))

    fields_by_name: dict[str, FieldDef] = {}
    fields_by_number: dict[int, FieldDef] = {}
    for doc in docs:
        for field in doc.fields:
            previous = fields_by_name.get(field.name) or fields_by_number.get(field.number)
            if previous is not None and previous.signature() != field.signature():
                raise SchemaError(
                    f"conflicting field definition for {field.name!r} (tag {field.number}); "
                    f"definitions at {_locations((previous.location, field.location))}"
                )
            fields_by_name.setdefault(field.name, previous or field)
            fields_by_number.setdefault(field.number, previous or field)

    components_by_name: dict[str, ComponentDef] = {}
    for doc in docs:
        for component in doc.components:
            previous = components_by_name.get(component.name)
            if previous is not None and previous.signature() != component.signature():
                raise SchemaError(
                    f"conflicting component definition for {component.name!r}; definitions at "
                    f"{_locations((previous.location, component.location))}"
                )
            components_by_name.setdefault(component.name, previous or component)

    messages_by_name: dict[str, MessageDef] = {}
    messages_by_type: dict[str, MessageDef] = {}
    for doc in docs:
        for message in doc.messages:
            previous = messages_by_name.get(message.name) or messages_by_type.get(message.msg_type)
            if previous is not None and previous.signature() != message.signature():
                raise SchemaError(
                    f"conflicting message definition for {message.name!r} (MsgType {message.msg_type!r}); "
                    f"definitions at {_locations((previous.location, message.location))}"
                )
            messages_by_name.setdefault(message.name, previous or message)
            messages_by_type.setdefault(message.msg_type, previous or message)

    if not messages_by_name:
        raise SchemaError("merged XML inputs contain no <message> definitions")

    verified_components: set[str] = set()
    expanded_components_cache: dict[str, tuple[MemberDef, ...]] = {}
    for component in components_by_name.values():
        _check_member_references(component.members, fields_by_name, components_by_name,
            f"component {component.name!r}", (component.name,), verified_components)
        expanded = _expand_members(component.members, components_by_name, True, (component.name,))
        _validate_member_scope(expanded, fields_by_name, f"component {component.name!r}",
            component.name in {"Header", "Trailer"})
        expanded_components_cache[component.name] = expanded

    expanded_messages: dict[str, tuple[MemberDef, ...]] = {}
    for message in messages_by_name.values():
        _check_member_references(message.members, fields_by_name, components_by_name,
            f"message {message.name!r}", (), verified_components)
        expanded = _expand_members(message.members, components_by_name)
        _validate_member_scope(expanded, fields_by_name, f"message {message.name!r}")
        expanded_messages[message.name] = expanded

    # Component and group references are expanded to preserve their exact XML
    # wire order. Generated accessors are emitted for direct message fields;
    # group entry accessors use their own independent descriptor context.
    for message in messages_by_name.values():
        setters: dict[str, MemberDef] = {}
        for member in expanded_messages[message.name]:
            if member.kind != "field":
                continue
            setter = _snake_case(member.field_name)
            if setter in _BUILDER_RESERVED_SETTERS:
                raise SchemaError(f"field {member.field_name!r} generates reserved builder setter {setter!r}", member.location)
            if setter in setters:
                other = setters[setter]
                raise SchemaError(
                    f"field names {other.field_name!r} and {member.field_name!r} both generate setter {setter!r}",
                    member.location,
                )
            _validate_xml_name(setter, "setter", member.location)
            setters[setter] = member

    generated_symbols = {"FieldDescriptor", "EnumValueDescriptor", "fields", "enum_values", "Schema"}
    for message in messages_by_name.values():
        for symbol in (message.name, f"{message.name}View", f"{message.name}Policy",
                       f"{message.name}Builder", f"{message.name}_allowed_tags",
                       f"{message.name}_required_tags"):
            if symbol in generated_symbols:
                raise SchemaError(
                    f"message {message.name!r} collides with generated C++ symbol {symbol!r}", message.location)
            generated_symbols.add(symbol)

    schema = Schema(next(iter(identities)), fields_by_name, messages_by_name, components_by_name)
    # Cache the expanded wire model for deterministic rendering without mutating
    # any user-visible XML definitions.
    schema._expanded_messages = expanded_messages
    schema._expanded_components = expanded_components_cache
    return schema


def filter_schema_messages(schema: Schema, requested: list[str]) -> Schema:
    """Subset a loaded schema to only the requested message names or MsgTypes."""
    selected_messages: dict[str, MessageDef] = {}
    by_type = {msg.msg_type: msg for msg in schema.messages_by_name.values()}
    for token in requested:
        name = token.strip()
        if not name:
            continue
        msg = schema.messages_by_name.get(name) or by_type.get(name)
        if msg is None:
            raise SchemaError(f"unknown message selector {name!r} for --messages")
        selected_messages[msg.name] = msg
    if not selected_messages:
        raise SchemaError("--messages selected no messages")

    used_fields: set[str] = set()

    def collect_fields(members: tuple[MemberDef, ...]) -> None:
        for member in members:
            used_fields.add(member.field_name)
            if member.kind == "group":
                collect_fields(member.members)

    selected_components: dict[str, ComponentDef] = {}
    selected_expanded_components: dict[str, tuple[MemberDef, ...]] = {}
    for comp_name in ("Header", "Trailer"):
        if comp_name in schema.components_by_name:
            selected_components[comp_name] = schema.components_by_name[comp_name]
            selected_expanded_components[comp_name] = schema._expanded_components[comp_name]
            collect_fields(selected_expanded_components[comp_name])

    selected_expanded_messages: dict[str, tuple[MemberDef, ...]] = {}
    for name, msg in selected_messages.items():
        expanded = schema._expanded_messages[name]
        selected_expanded_messages[name] = expanded
        collect_fields(expanded)

    # Ensure paired LENGTH fields for any selected DATA/XMLDATA field are kept.
    def expand_pair_name(value: str) -> tuple[str, ...]:
        compact = value.replace("Security", "Sec")
        expanded = value.replace("Sec", "Security") if "Security" not in value else value
        return (value, compact, expanded)

    for fname in list(used_fields):
        fdef = schema.fields_by_name[fname]
        if fdef.field_type in {"DATA", "XMLDATA"}:
            for base in expand_pair_name(fdef.name):
                for suffix in ("Length", "Len"):
                    cand = base + suffix
                    if cand in schema.fields_by_name and schema.fields_by_name[cand].field_type == "LENGTH":
                        used_fields.add(cand)

    selected_fields = {name: schema.fields_by_name[name] for name in used_fields if name in schema.fields_by_name}
    subset = Schema(schema.identity, selected_fields, selected_messages, selected_components)
    subset._expanded_messages = selected_expanded_messages
    subset._expanded_components = selected_expanded_components
    return subset


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
    """Render a stable typed schema header from the normalized XML model."""
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
    fields = sorted(schema.fields_by_name.values(), key=lambda item: item.number)
    field_indexes = {field.name: index for index, field in enumerate(fields)}
    expanded_messages = schema._expanded_messages
    expanded_components = schema._expanded_components
    messages = sorted(schema.messages_by_name.values(), key=lambda item: (item.name, item.msg_type))
    components = sorted(schema.components_by_name.values(), key=lambda item: item.name)
    version_parts = [dict(schema.identity).get(key, "") for key in ("major", "minor", "servicepack")]
    version = ".".join(part for part in version_parts if part)

    enum_values: list[tuple[int, str, str]] = []
    enum_ranges: dict[int, tuple[int, int]] = {}
    for field in fields:
        start = len(enum_values)
        enum_values.extend((field.number, value, description) for value, description in field.enum_values)
        enum_ranges[field.number] = (start, len(field.enum_values))

    def pair_name_key(value: str) -> str:
        # QuickFIX's FIX 5.0 SP2 dictionary abbreviates Security as Sec in a
        # single legacy Length/Data field pair.
        return value.replace("Security", "Sec")

    field_pair_tags: dict[int, int] = {}
    for field in fields:
        if field.field_type in {"DATA", "XMLDATA"}:
            pair_name = pair_name_key(field.name)
            candidates = (field.name + "Length", field.name + "Len",
                          pair_name + "Length", pair_name + "Len")
            length = next((schema.fields_by_name[name] for name in candidates
                           if name in schema.fields_by_name and
                           schema.fields_by_name[name].field_type == "LENGTH"), None)
            if length is None:
                raise SchemaError(f"DATA field {field.name!r} has no matching LENGTH field", field.location)
            field_pair_tags[field.number] = length.number
        elif field.field_type in {"LENGTH", "NUMINGROUP"}:
            bases = (field.name[:-6] if field.name.endswith("Length") else "",
                     field.name[:-3] if field.name.endswith("Len") else "")
            candidates_list: list[str] = []
            for base in bases:
                if not base:
                    continue
                candidates_list.append(base)
                candidates_list.append(pair_name_key(base))
                if "Security" not in base and "Sec" in base:
                    candidates_list.append(base.replace("Sec", "Security"))
            data = next((schema.fields_by_name[name] for name in candidates_list
                           if name in schema.fields_by_name and
                           schema.fields_by_name[name].field_type in {"DATA", "XMLDATA"}), None)
            if data is not None:
                field_pair_tags[field.number] = data.number

    def member_key(member: MemberDef) -> tuple[object, ...]:
        if member.kind == "field":
            return ("f", field_indexes[member.field_name], member.required)
        return ("g", field_indexes[member.field_name], member.required, group_key(member))

    def group_key(member: MemberDef) -> tuple[object, ...]:
        # Requiredness belongs to the use-site and affects an entry's count
        # presence. Keep that distinction in the interned group shape.
        return (field_indexes[member.field_name], member.required,
                tuple(member_key(child) for child in member.members))

    group_by_key: dict[tuple[object, ...], MemberDef] = {}

    def collect_groups(members: tuple[MemberDef, ...]) -> None:
        for member in members:
            if member.kind != "group":
                continue
            key = group_key(member)
            group_by_key.setdefault(key, member)
            collect_groups(member.members)

    for members in expanded_messages.values():
        collect_groups(members)
    for members in expanded_components.values():
        collect_groups(members)
    group_keys = sorted(group_by_key, key=repr)
    group_indexes = {key: index for index, key in enumerate(group_keys)}
    component_indexes = {component.name: index for index, component in enumerate(components)}
    message_indexes = {message.name: index for index, message in enumerate(messages)}
    group_names_by_field: dict[str, list[int]] = {}
    for index, key in enumerate(group_keys):
        group_names_by_field.setdefault(group_by_key[key].field_name, []).append(index)

    def group_symbol(index: int, suffix: str) -> str:
        group = group_by_key[group_keys[index]]
        base = group.field_name + (f"Group{index}" if len(group_names_by_field[group.field_name]) > 1 else "Group")
        return base + suffix

    def member_groups(members: tuple[MemberDef, ...]) -> list[MemberDef]:
        return [member for member in members if member.kind == "group"]

    def group_depth(index: int, active: tuple[int, ...] = ()) -> int:
        if index in active:
            raise SchemaError("recursive repeating group nesting")
        children = [group_indexes[group_key(member)] for member in member_groups(group_by_key[group_keys[index]].members)]
        return 1 + max((group_depth(child, (*active, index)) for child in children), default=0)

    group_emit_order = sorted(range(len(group_keys)), key=lambda index: (group_depth(index), index))

    canonical = bytearray(b"nimblefix-schema-v3\0codegen-3\0")

    def add_text(value: str) -> None:
        encoded = value.encode("utf-8")
        canonical.extend(str(len(encoded)).encode("ascii"))
        canonical.append(ord(":"))
        canonical.extend(encoded)

    def add_number(value: int) -> None:
        canonical.extend(str(value).encode("ascii"))
        canonical.append(ord(";"))

    def add_members(members: tuple[MemberDef, ...]) -> None:
        add_number(len(members))
        for member in members:
            canonical.extend(b"g" if member.kind == "group" else b"f")
            add_number(schema.fields_by_name[member.field_name].number)
            canonical.extend(b"1" if member.required else b"0")
            if member.kind == "group":
                add_members(member.members)

    add_number(len(schema.identity))
    for key, value in schema.identity:
        add_text(key)
        add_text(value)
    add_number(len(fields))
    for field in fields:
        add_number(field.number)
        add_text(field.name)
        add_text(field.field_type)
        add_number(len(field.enum_values))
        for value, description in field.enum_values:
            add_text(value)
            add_text(description)
    add_number(len(expanded_components))
    for name in sorted(expanded_components):
        add_text(name)
        add_members(expanded_components[name])
    add_number(len(messages))
    for message in messages:
        add_text(message.name)
        add_text(message.msg_type)
        add_text(message.category)
        add_members(expanded_messages[message.name])
    fingerprint = hashlib.sha256(canonical).hexdigest()

    lines = [
        "// Generated by tools/schema/generate.py. Do not edit.",
        f"// FIX schema version {version}; normalized fingerprint {fingerprint}.",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <span>",
        "#include <string_view>",
        "#include <type_traits>",
        "#include <utility>",
        "#include <nimblefix/message.hpp>",
        "#include <nimblefix/group.hpp>",
        "#include <nimblefix/schema.hpp>",
        "",
        f"namespace {ns} {{",
        "",
        "using FieldDescriptor = ::nimblefix::FieldDescriptor;",
        "using EnumValueDescriptor = ::nimblefix::EnumValueDescriptor;",
        "using GroupDescriptor = ::nimblefix::GroupDescriptor;",
        "using ComponentDescriptor = ::nimblefix::ComponentDescriptor;",
        "using MessageDescriptor = ::nimblefix::MessageDescriptor;",
        "using MessageMemberDescriptor = ::nimblefix::MessageMemberDescriptor;",
        "",
        f"inline constexpr std::array<EnumValueDescriptor, {len(enum_values)}> enum_values{{{{",
    ]
    for tag, value, description in enum_values:
        lines.append(f"    {{{tag}u, {_cpp_string(value)}, {_cpp_string(description)}}},")
    lines += ["}};", "", f"inline constexpr std::array<FieldDescriptor, {len(fields)}> fields{{{{"]
    for field in fields:
        start, count = enum_ranges[field.number]
        enum_span = (f"std::span<const EnumValueDescriptor>{{enum_values.data() + {start}, {count}}}"
                    if count else "std::span<const EnumValueDescriptor>{}")
        length_tag = 0
        data_tag = field.number if field.field_type in {"DATA", "XMLDATA"} else 0
        if field.field_type in {"DATA", "XMLDATA"}:
            length_tag = field_pair_tags.get(field.number, 0)
            if length_tag == 0:
                raise SchemaError(f"DATA field {field.name!r} has no matching LENGTH field", field.location)
        elif field.number in field_pair_tags and field.field_type in {"LENGTH", "NUMINGROUP"}:
            data_tag = field_pair_tags[field.number]
        lines.append(
            f"    {{{field.number}u, {_cpp_string(field.name)}, {_cpp_string(field.field_type)}, "
            f"::nimblefix::FieldClass::{FIX_FIELD_CLASSES[field.field_type]}, "
            f"::nimblefix::WireType::{FIX_WIRE_TYPES[field.field_type]}, {enum_span}, {length_tag}u, {data_tag}u}},"
        )
    lines += ["}};", ""]

    def emit_members(name: str, members: tuple[MemberDef, ...], emit_required: bool = False) -> None:
        lines.append(f"inline constexpr std::array<MessageMemberDescriptor, {len(members)}> {name}{{{{")
        indexed_tags: list[tuple[int, int]] = []
        required_indices: list[int] = []
        for idx, member in enumerate(members):
            tag_num = schema.fields_by_name[member.field_name].number
            indexed_tags.append((tag_num, idx))
            if member.required:
                required_indices.append(idx)
            if member.kind == "field":
                lines.append(f"    {{::nimblefix::MemberKind::field, {field_indexes[member.field_name]}u, 0u, {'true' if member.required else 'false'}}},")
            else:
                group_index = group_indexes[group_key(member)]
                lines.append(f"    {{::nimblefix::MemberKind::group, 0u, {group_index}u, {'true' if member.required else 'false'}}},")
        lines.extend(["}};", ""])
        indexed_tags.sort(key=lambda item: item[0])
        lines.append(f"inline constexpr std::array<::nimblefix::MemberTagEntry, {len(indexed_tags)}> {name}_by_tag{{{{")
        for tag_num, idx in indexed_tags:
            lines.append(f"    {{{tag_num}u, {idx}u}},")
        lines.extend(["}};", ""])
        if emit_required:
            lines.append(f"inline constexpr std::array<std::uint32_t, {len(required_indices)}> {name}_required{{{{")
            for idx in required_indices:
                lines.append(f"    {idx}u,")
            lines.extend(["}};", ""])

    for index, key in enumerate(group_keys):
        emit_members(f"group_{index}_members", group_by_key[key].members, emit_required=True)
    for index, component in enumerate(components):
        emit_members(f"component_{index}_members", expanded_components[component.name], emit_required=False)
    for index, message in enumerate(messages):
        emit_members(f"message_{index}_members", expanded_messages[message.name], emit_required=True)

    lines.append(f"inline constexpr std::array<GroupDescriptor, {len(group_keys)}> groups{{{{")
    for index, key in enumerate(group_keys):
        group = group_by_key[key]
        counter = schema.fields_by_name[group.field_name].number
        first = group.members[0]
        delimiter = schema.fields_by_name[first.field_name].number
        lines.append(
            f"    {{{_cpp_string(group.field_name)}, {counter}u, {delimiter}u, "
            f"std::span<const MessageMemberDescriptor>{{group_{index}_members}}, "
            f"std::span<const ::nimblefix::MemberTagEntry>{{group_{index}_members_by_tag}}, "
            f"std::span<const std::uint32_t>{{group_{index}_members_required}}}},")
    lines += ["}};", "", f"inline constexpr std::array<ComponentDescriptor, {len(components)}> components{{{{"]
    for index, component in enumerate(components):
        lines.append(
            f"    {{{_cpp_string(component.name)}, std::span<const MessageMemberDescriptor>{{component_{index}_members}}, "
            f"std::span<const ::nimblefix::MemberTagEntry>{{component_{index}_members_by_tag}}}},")
    lines += ["}};", "", f"inline constexpr std::array<MessageDescriptor, {len(messages)}> messages{{{{"]
    for index, message in enumerate(messages):
        lines.append(
            f"    {{{_cpp_string(message.name)}, {_cpp_string(message.msg_type)}, {_cpp_string(message.category)}, "
            f"std::span<const MessageMemberDescriptor>{{message_{index}_members}}, "
            f"std::span<const ::nimblefix::MemberTagEntry>{{message_{index}_members_by_tag}}, "
            f"std::span<const std::uint32_t>{{message_{index}_members_required}}}},")
    lines += [
        "}};",
        "",
        "inline constexpr ::nimblefix::SchemaDescriptor schema_descriptor{",
        f"    {_cpp_string(dict(schema.identity).get('type', 'FIX'))}, {_cpp_string(version)}, {_cpp_string(fingerprint)},",
        "    std::span<const FieldDescriptor>{fields}, std::span<const GroupDescriptor>{groups},",
        "    std::span<const ComponentDescriptor>{components}, std::span<const MessageDescriptor>{messages}",
        "};",
        "",
        f"inline constexpr std::array<::nimblefix::LengthDataPair, {sum(1 for field in fields if field.field_type in {'DATA', 'XMLDATA'})}> schema_length_data_pairs{{{{",
    ]
    for field in fields:
        if field.field_type in {"DATA", "XMLDATA"}:
            lines.append(f"    {{{field_pair_tags[field.number]}u, {field.number}u}},")
    lines += [
        "}};",
        "",
        "struct Schema {",
        f"    static constexpr std::string_view type = {_cpp_string(dict(schema.identity).get('type', 'FIX'))};",
        f"    static constexpr std::string_view version = {_cpp_string(version)};",
        f"    static constexpr std::string_view fingerprint = {_cpp_string(fingerprint)};",
        f"    static constexpr std::size_t field_count = {len(fields)};",
        f"    static constexpr std::size_t enum_value_count = {len(enum_values)};",
        f"    static constexpr std::size_t message_count = {len(messages)};",
        f"    static constexpr std::size_t component_count = {len(components)};",
        f"    static constexpr std::size_t group_count = {len(group_keys)};",
        "    inline static constexpr const auto& fields = ::" + ns + "::fields;",
        "    inline static constexpr const auto& enum_values = ::" + ns + "::enum_values;",
        "    [[nodiscard]] static constexpr const ::nimblefix::SchemaDescriptor& descriptor() noexcept { return schema_descriptor; }",
        "    [[nodiscard]] static constexpr const ::nimblefix::MessageDescriptor* find_message(std::string_view value) noexcept { return descriptor().find_message(value); }",
        "    [[nodiscard]] static constexpr const ::nimblefix::FieldDescriptor* find_field(std::uint32_t tag) noexcept { return descriptor().find_field(tag); }",
        "    [[nodiscard]] static constexpr ::nimblefix::WireHints wire_hints() noexcept { return {std::span<const ::nimblefix::LengthDataPair>{schema_length_data_pairs}}; }",
        "    template<class Visitor> static void for_each_message(Visitor&& visitor);",
        "};",
        "",
    ]

    lines.append("} // namespace " + ns)
    lines += ["", "namespace nimblefix {"]
    for field in fields:
        field_class = FIX_FIELD_CLASSES[field.field_type]
        lines += [
            "template<>",
            f"struct SchemaField<{ns}::Schema, {field.number}u> : std::integral_constant<FieldClass, FieldClass::{field_class}> {{",
            f"    static constexpr FieldClass field_class = FieldClass::{field_class};",
            "};",
        ]
    lines += ["} // namespace nimblefix", "", f"namespace {ns} {{", ""]

    def immediate_tags(members: tuple[MemberDef, ...]) -> list[tuple[int, bool, MemberDef]]:
        return [(schema.fields_by_name[member.field_name].number, member.required, member)
                for member in members]

    # Fields whose wire values require schema-level text/enum checking beyond
    # what SlotValue and the encoder already enforce directly on typed slots.
    _NEEDS_ENUM_OR_FORMAT_CHECK = {
        "BOOLEAN", "UTCDATE", "UTCDATEONLY", "LOCALMKTDATE", "UTCTIMEONLY",
        "MONTHYEAR", "DAYOFMONTH", "TAGNUM", "MULTIPLEVALUESTRING",
        "MULTIPLECHARVALUE", "MULTIPLESTRINGVALUE", "LENGTH", "NUMINGROUP", "SEQNUM",
    }

    def field_needs_policy_check(field: FieldDef) -> bool:
        return bool(field.enum_values) or field.field_type in _NEEDS_ENUM_OR_FORMAT_CHECK

    # Group policies and their trait specializations are emitted before the
    # entry wrappers instantiate FixedShapeMessage. This keeps group entry
    # plans identical to the normal static tag / generated message plan.
    for group_index in range(len(group_keys)):
        group = group_by_key[group_keys[group_index]]
        policy = group_symbol(group_index, "EntryPolicy")
        builder = group_symbol(group_index, "EntryBuilder")
        setters_mixin = group_symbol(group_index, "EntrySetters")
        members = group.members
        allowed = [tag for tag, _required, _member in immediate_tags(members)]
        required = [tag for tag, is_required, _member in immediate_tags(members) if is_required]
        lines += [
            f"template<class... Slots> class {builder};",
            f"template<class Derived> struct {setters_mixin};",
            f"struct {policy} {{",
            "    template<class... Slots>",
            f"    using rebind = {builder}<Slots...>;",
            "    template<class Derived>",
            f"    using named_setters = {setters_mixin}<Derived>;",
            "    static constexpr bool is_group_entry = true;",
            "    static constexpr std::string_view msg_type = \"__entry__\";",
            f"    inline static constexpr std::array<std::uint32_t, {len(allowed)}> allowed_tags{{{{",
        ]
        lines.extend(f"        {tag}u," for tag in allowed)
        lines += ["    }};", f"    inline static constexpr std::array<std::uint32_t, {len(required)}> required_tags{{{{"]
        lines.extend(f"        {tag}u," for tag in required)
        lines += ["    }};", "    static constexpr bool enum_value_allowed(std::uint32_t tag, std::string_view value) noexcept {", "        switch (tag) {"]
        for member in members:
            if member.kind != "field":
                continue
            field = schema.fields_by_name[member.field_name]
            if not field_needs_policy_check(field):
                continue
            lines += [f"        case {field.number}u:",
                f"            return ::nimblefix::is_valid_schema_field_value(fields[{field_indexes[field.name]}], value);"]
        lines += ["        default:", "            (void)value;", "            return true;", "        }", "    }", "};", ""]

    for index, message in enumerate(messages):
        name = message.name
        policy = f"{name}Policy"
        builder = f"{name}Builder"
        setters_mixin = f"{name}Setters"
        members = expanded_messages[name]
        allowed_tags = [tag for tag, _required, _member in immediate_tags(members)]
        required_tags = [tag for tag, is_required, _member in immediate_tags(members) if is_required]
        direct_fields = [member for member in members if member.kind == "field"]
        lines += [
            f"template<class... Slots> class {builder};",
            f"template<class Derived> struct {setters_mixin};",
            f"struct {policy} {{",
            "    using schema_type = Schema;",
            "    template<class... Slots>",
            f"    using rebind = {builder}<Slots...>;",
            "    template<class Derived>",
            f"    using named_setters = {setters_mixin}<Derived>;",
            f"    static constexpr std::string_view msg_type = {_cpp_string(message.msg_type)};",
            f"    inline static constexpr std::array<std::uint32_t, {len(allowed_tags)}> allowed_tags{{{{",
        ]
        lines.extend(f"        {tag}u," for tag in allowed_tags)
        lines += ["    }};", f"    inline static constexpr std::array<std::uint32_t, {len(required_tags)}> required_tags{{{{"]
        lines.extend(f"        {tag}u," for tag in required_tags)
        lines += ["    }};", "    static constexpr bool enum_value_allowed(std::uint32_t tag, std::string_view value) noexcept {", "        switch (tag) {"]
        for member in direct_fields:
            field = schema.fields_by_name[member.field_name]
            if not field_needs_policy_check(field):
                continue
            lines += [f"        case {field.number}u:",
                f"            return ::nimblefix::is_valid_schema_field_value(fields[{field_indexes[field.name]}], value);"]
        lines += ["        default:", "            (void)value;", "            return true;", "        }", "    }", "};", ""]

    lines.append("} // namespace " + ns)
    lines += ["", "namespace nimblefix {"]
    for group_index, key in enumerate(group_keys):
        group = group_by_key[key]
        policy = f"{ns}::{group_symbol(group_index, 'EntryPolicy')}"
        for rank, member in enumerate(group.members):
            tag = schema.fields_by_name[member.field_name].number
            field = schema.fields_by_name[member.field_name]
            lines += [
                "template<>",
                f"struct WireRank<{policy}, {tag}> : std::integral_constant<std::size_t, {rank}> {{}};",
                "template<>",
                f"struct FieldWireType<{policy}, {tag}> : std::integral_constant<WireType, WireType::{FIX_WIRE_TYPES[field.field_type]}> {{}};",
            ]
            if member.kind == "group":
                lines += [
                    "template<>",
                    f"struct GroupCountTag<{policy}, {tag}> : std::true_type {{}};",
                ]
            if tag in field_pair_tags:
                lines += [
                    "template<>",
                    f"struct FieldPairTag<{policy}, {tag}> : std::integral_constant<std::uint32_t, {field_pair_tags[tag]}u> {{}};",
                ]
    lines += ["} // namespace nimblefix", "", f"namespace {ns} {{", ""]

    # Leaf-first so nested GroupBuilder aliases are complete before their
    # parent GroupEntryOwner tuple is declared.
    for group_index in group_emit_order:
        key = group_keys[group_index]
        group = group_by_key[key]
        symbol = group_symbol(group_index, "")
        entry_name = group_symbol(group_index, "Entry")
        builder = group_symbol(group_index, "EntryBuilder")
        setters_mixin = group_symbol(group_index, "EntrySetters")
        policy = group_symbol(group_index, "EntryPolicy")
        count_tag = schema.fields_by_name[group.field_name].number
        delimiter_tag = schema.fields_by_name[group.members[0].field_name].number
        members = group.members
        lines += [
            f"template<class Derived>",
            f"struct {setters_mixin} {{",
            "    static constexpr bool is_group_entry = true;",
        ]
        for member in members:
            if member.kind == "field":
                field = schema.fields_by_name[member.field_name]
                setter = _snake_case(field.name)
                lines += [
                    "    template<class V>",
                    f"    auto {setter}(V&& value) && {{",
                    f"        return std::move(static_cast<Derived&>(*this)).template set<{field.number}u>(std::forward<V>(value));",
                    "    }",
                ]
            else:
                child = schema.fields_by_name[member.field_name]
                group_setter = _snake_case(child.name)
                lines += [
                    "    template<class G>",
                    f"    auto {group_setter}(G&& group) && {{",
                    f"        static_assert(std::remove_cvref_t<G>::count_tag == {child.number}u, \"wrong nested repeating group type\");",
                    "        return ::nimblefix::attach_group(std::move(static_cast<Derived&>(*this)), std::forward<G>(group));",
                    "    }",
                ]
        lines += [
            "};",
            "",
            f"struct {entry_name} {{",
            f"    using policy_type = {policy};",
            "    static constexpr bool is_group_entry = true;",
            f"    static auto make(::nimblefix::MessageLimits limits = {{}});",
            "};",
            "",
            f"template<class... Slots>",
            f"class {builder} : public ::nimblefix::MessageBuilder<{policy}, \"__entry__\", ::nimblefix::FieldList<Slots...>>, public {setters_mixin}<{builder}<Slots...>> {{",
            f"    using Base = ::nimblefix::MessageBuilder<{policy}, \"__entry__\", ::nimblefix::FieldList<Slots...>>;",
            "public:",
            "    static constexpr bool is_group_entry = true;",
            "    using Base::Base;",
            "};",
            f"inline auto {entry_name}::make(::nimblefix::MessageLimits limits) {{ return {builder}<>{{limits}}; }}",
            f"template<std::size_t Capacity = 16>",
            f"using {symbol} = ::nimblefix::OwnedGroupBuilder<{count_tag}u, {delimiter_tag}u, Capacity, {'true' if group.required else 'false'}>;",
            "",
        ]

    for index, message in enumerate(messages):
        name = message.name
        policy = f"{name}Policy"
        builder = f"{name}Builder"
        setters_mixin = f"{name}Setters"
        fixed_name = f"{name}Fixed"
        members = expanded_messages[name]
        direct_fields = [member for member in members if member.kind == "field"]

        lines += [
            f"struct {name};",
            f"class {name}View : public ::nimblefix::TypedMessageView<Schema> {{",
            f"    using Base = ::nimblefix::TypedMessageView<Schema>;",
            "public:",
            f"    explicit {name}View(const ::nimblefix::FlatMessageView& flat) noexcept : Base(flat, messages[{index}]) {{}}",
        ]
        for member in direct_fields:
            field = schema.fields_by_name[member.field_name]
            getter = _snake_case(field.name)
            lines += [
                "    [[nodiscard]] auto " + getter + "() const noexcept {",
                f"        return this->template get<{field.number}>();",
                "    }",
            ]
        for member in members:
            if member.kind != "group":
                continue
            group_field = schema.fields_by_name[member.field_name]
            group_getter = _snake_case(member.field_name)
            lines += [
                "    [[nodiscard]] auto " + group_getter + "() const noexcept {",
                f"        return this->template group<{group_field.number}>();",
                "    }",
            ]
        lines += ["};", ""]

        lines += [
            f"template<class Derived>",
            f"struct {setters_mixin} {{",
        ]
        for member in direct_fields:
            field = schema.fields_by_name[member.field_name]
            setter = _snake_case(field.name)
            lines += [
                "    template<class V>",
                f"    auto {setter}(V&& value) && {{",
                f"        return std::move(static_cast<Derived&>(*this)).template set<{field.number}>(std::forward<V>(value));",
                "    }",
            ]
        for member in members:
            if member.kind != "group":
                continue
            counter = schema.fields_by_name[member.field_name]
            setter = _snake_case(counter.name)
            group_type = f"{group_symbol(group_indexes[group_key(member)], '')}<>"
            lines += [
                "    template<class G>",
                f"    auto {setter}(G&& group) && {{",
                f"        static_assert(std::remove_cvref_t<G>::count_tag == {counter.number}u, \"wrong repeating group type\");",
                "        return ::nimblefix::attach_group(std::move(static_cast<Derived&>(*this)), std::forward<G>(group));",
                "    }",
                f"    auto {setter}({group_type}&& group) && {{",
                "        return ::nimblefix::attach_group(std::move(static_cast<Derived&>(*this)), std::move(group));",
                "    }",
            ]
        lines += ["};", ""]

        lines += [
            f"template<class Shape, std::size_t ByteCapacity = 512>",
            f"class {fixed_name} : public ::nimblefix::FixedShapeMessage<{_cpp_string(message.msg_type)}, Shape, ByteCapacity, {policy}> {{",
            f"    using Base = ::nimblefix::FixedShapeMessage<{_cpp_string(message.msg_type)}, Shape, ByteCapacity, {policy}>;",
            "public:",
            "    using Base::Base;",
        ]
        for member in direct_fields:
            field = schema.fields_by_name[member.field_name]
            setter = _snake_case(field.name)
            lines += [
                "    template<class V>",
                f"    [[nodiscard]] ::nimblefix::Status {setter}(V&& value) noexcept {{",
                f"        return this->template set<{field.number}>(std::forward<V>(value));",
                "    }",
            ]
        lines += ["};", ""]

        lines += [
            f"struct {name} {{",
            "    using schema_type = Schema;",
            f"    using view_type = {name}View;",
            f"    using policy_type = {policy};",
            f"    template<class Shape, std::size_t ByteCapacity = 512>",
            f"    using fixed_type = {fixed_name}<Shape, ByteCapacity>;",
            f"    static constexpr std::string_view msg_type = {_cpp_string(message.msg_type)};",
            f"    inline static constexpr const MessageDescriptor& descriptor = messages[{index}];",
            f"    static auto make(::nimblefix::MessageLimits limits = {{}});",
            "};",
            "",
            "template<class... Slots>",
            f"class {builder} : public ::nimblefix::MessageBuilder<{policy}, {_cpp_string(message.msg_type)}, ::nimblefix::FieldList<Slots...>>, public {setters_mixin}<{builder}<Slots...>> {{",
            f"    using Base = ::nimblefix::MessageBuilder<{policy}, {_cpp_string(message.msg_type)}, ::nimblefix::FieldList<Slots...>>;",
            "public:",
            "    using schema_type = Schema;",
            "    using Base::Base;",
            "};",
            f"inline auto {name}::make(::nimblefix::MessageLimits limits) {{ return {builder}<>{{limits}}; }}",
            "",
        ]

    lines += ["template<class Visitor>", "inline void Schema::for_each_message(Visitor&& visitor) {"]
    lines.append("    auto&& callable = visitor;")
    for message in messages:
        lines.append(f"    callable.template operator()<{message.name}>();")
    lines += ["}", ""]
    lines.append("} // namespace " + ns)
    lines += ["", "namespace nimblefix {"]
    for message in messages:
        policy = f"{ns}::{message.name}Policy"
        for rank, member in enumerate(expanded_messages[message.name]):
            field = schema.fields_by_name[member.field_name]
            lines += [
                "template<>",
                f"struct WireRank<{policy}, {field.number}> : std::integral_constant<std::size_t, {rank}> {{}};",
                "template<>",
                f"struct FieldWireType<{policy}, {field.number}> : std::integral_constant<WireType, WireType::{FIX_WIRE_TYPES[field.field_type]}> {{}};",
            ]
            if member.kind == "group":
                lines += [
                    "template<>",
                    f"struct GroupCountTag<{policy}, {field.number}> : std::true_type {{}};",
                ]
            if field.number in field_pair_tags:
                lines += [
                    "template<>",
                    f"struct FieldPairTag<{policy}, {field.number}> : std::integral_constant<std::uint32_t, {field_pair_tags[field.number]}u> {{}};",
                ]
    lines += ["} // namespace nimblefix", ""]
    # Legacy-style per-message arrays remain as cheap views of the generated
    # descriptor and preserve the existing public codegen contract.
    lines += [f"namespace {ns} {{"]
    for message in messages:
        members = expanded_messages[message.name]
        ident = message.name
        lines.append(f"inline constexpr std::array<std::uint32_t, {len(members)}> {ident}_allowed_tags{{{{")
        lines.extend(f"    {schema.fields_by_name[member.field_name].number}u," for member in members)
        lines.append("}};")
        required = [member for member in members if member.required]
        lines.append(f"inline constexpr std::array<std::uint32_t, {len(required)}> {ident}_required_tags{{{{")
        lines.extend(f"    {schema.fields_by_name[member.field_name].number}u," for member in required)
        lines.extend(["}};", ""])
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
    parser.add_argument("--messages", default="",
        help="optional comma-separated subset of message names or MsgTypes to generate")
    parser.add_argument("xml", nargs="+", type=Path, help="FIX XML schema input(s)")
    args = parser.parse_args(argv)
    try:
        schema = load_schema(args.xml)
        if args.messages:
            schema = filter_schema_messages(schema, args.messages.split(","))
        header = render_header(schema, args.namespace)
        write_if_changed(args.output, header)
    except SchemaError as exc:
        print(f"schema codegen: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

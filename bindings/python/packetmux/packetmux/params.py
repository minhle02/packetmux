"""Typed stream params: a kind plus declared fields, carried as JSON inside opaque bytes."""

import dataclasses
import json
import math
import types
import typing
from dataclasses import dataclass
from typing import Any, Mapping, TypeVar

from .errors import InvalidParams

T = TypeVar("T", bound=type)

MAX_INT = 2**53 - 1  # the largest whole number every language reads exactly
_TYPE_NAMES = {str: "string", int: "int", float: "float", bool: "bool"}


@dataclass
class _Field:
    name: str
    type: str
    nullable: bool = False
    has_default: bool = False
    default: Any = None


def _field_from_annotation(field: dataclasses.Field, annotation: Any) -> _Field:
    nullable = False
    if typing.get_origin(annotation) in (typing.Union, types.UnionType):
        members = [arg for arg in typing.get_args(annotation) if arg is not type(None)]
        nullable = len(members) == 1 and len(typing.get_args(annotation)) == 2
        annotation = members[0] if nullable else annotation
    if annotation not in _TYPE_NAMES:
        raise TypeError(f"{field.name}: use str, int, float, bool, or one of those | None")
    has_default = field.default is not dataclasses.MISSING
    return _Field(
        field.name,
        _TYPE_NAMES[annotation],
        nullable,
        has_default,
        field.default if has_default else None,
    )


def stream_type(kind: str):
    """Declares a dataclass as a stream type: stream_type("tcp-forward")."""

    def declare(cls: T) -> T:
        hints = typing.get_type_hints(cls)
        cls.__stream_kind__ = kind
        cls.__stream_fields__ = [
            _field_from_annotation(field, hints[field.name])
            for field in dataclasses.fields(cls)
        ]
        return cls

    return declare


def define_stream(kind: str, fields: Mapping[str, Any]) -> type:
    """The same as stream_type, from a list of fields. It matches defineStream in TypeScript."""
    declared = []
    for name, spec in fields.items():
        type_name, has_default, default = spec, False, None
        if isinstance(spec, Mapping):
            type_name, has_default, default = spec["type"], "default" in spec, spec.get("default")
        declared.append(_Field(name, type_name.rstrip("?"), type_name.endswith("?"), has_default, default))
    cls = dataclasses.make_dataclass(
        "StreamParams",
        [
            (field.name, Any, dataclasses.field(default=field.default))
            if field.has_default
            else (field.name, Any)
            for field in sorted(declared, key=lambda field: field.has_default)
        ],
    )
    cls.__stream_kind__ = kind
    cls.__stream_fields__ = declared
    return cls


def is_stream_type(value: Any) -> bool:
    return hasattr(value, "__stream_kind__")


def _is_number(value: Any) -> bool:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    try:
        return math.isfinite(value)
    except OverflowError:  # an int too large for a float, which JSON allows
        return False


def _check_value(field: _Field, value: Any) -> Any:
    if value is None:
        if field.nullable:
            return None
        raise InvalidParams(f"{field.name} is null")
    if field.type == "string" and isinstance(value, str):
        return value
    if field.type == "bool" and isinstance(value, bool):
        return value
    if field.type == "float" and _is_number(value):
        return value
    if (
        field.type == "int"
        and _is_number(value)
        and value == math.floor(value)
        and abs(value) <= MAX_INT
    ):
        return int(value)  # 5037.0 becomes 5037
    raise InvalidParams(f"{field.name} is not a {field.type}")


def encode_params(value: Any) -> bytes:
    fields = {
        field.name: _check_value(field, getattr(value, field.name))
        for field in value.__stream_fields__
    }
    return json.dumps(fields, separators=(",", ":")).encode("utf-8")


def _refuse_constant(name: str) -> None:
    raise ValueError(f"{name} is not JSON")


def decode_params(stream: type, data: bytes) -> Any:
    try:
        parsed = json.loads(data.decode("utf-8"), parse_constant=_refuse_constant)
    except (ValueError, RecursionError) as error:
        raise InvalidParams(str(error)) from error
    if not isinstance(parsed, dict):
        raise InvalidParams("params are not an object")
    values = {}
    for field in stream.__stream_fields__:  # undeclared keys are ignored
        if field.name in parsed:
            values[field.name] = _check_value(field, parsed[field.name])
        elif field.has_default:
            values[field.name] = field.default
        else:
            raise InvalidParams(f"missing field {field.name}")
    return stream(**values)

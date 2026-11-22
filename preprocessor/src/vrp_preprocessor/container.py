"""The binary container shared by the graph files (docs/formats/graph.md).

Every binary file of a graph version is a header, a section table and the
sections: tagged, 8-byte-aligned arrays of little-endian integers.
"""

import enum
import hashlib
import struct
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import BinaryIO, Final, NamedTuple

import numpy as np
import numpy.typing as npt

FORMAT_VERSION: Final = 1

MAX_COUNT: Final = 2**32 - 2
"""The largest node, edge or shape-point count; 2**32 - 1 means no node."""

type IntArray = npt.NDArray[np.uint32 | np.int32 | np.int64]
"""An array of one of the format's element types."""

_ALIGNMENT: Final = 8
_MAGIC_SIZE: Final = 8
_TAG_SIZE: Final = 4
_HEADER: Final = struct.Struct("<8sIIIIII")
_ENTRY: Final = struct.Struct("<4sIQQ")


class ElementType(enum.IntEnum):
    """Element type codes of the section table."""

    UINT32 = 1
    INT32 = 2
    INT64 = 3


_SCALAR_TYPES: Final[
    Mapping[ElementType, type[np.uint32] | type[np.int32] | type[np.int64]]
] = {
    ElementType.UINT32: np.uint32,
    ElementType.INT32: np.int32,
    ElementType.INT64: np.int64,
}


class FormatError(ValueError):
    """Data does not follow the graph file format."""


@dataclass(frozen=True, eq=False)
class Section:
    """One tagged array of a container."""

    tag: str
    element_type: ElementType
    values: IntArray


@dataclass(frozen=True)
class FileDigest:
    """The size and SHA-256 of a written file, as the manifest records them."""

    size_bytes: int
    sha256: str


@dataclass(frozen=True, eq=False)
class Container:
    """The counts from a container's header and its required sections."""

    node_count: int
    edge_count: int
    sections: Mapping[str, IntArray]


class _Header(NamedTuple):
    magic: bytes
    version: int
    section_count: int
    node_count: int
    edge_count: int
    flags: int
    reserved: int


class _Entry(NamedTuple):
    tag: str
    type_code: int
    offset: int
    element_count: int


def _padding(size: int) -> int:
    return -size % _ALIGNMENT


def _require_little_endian() -> None:
    # The arrays are used in the host's byte order, which the format
    # requires to be little-endian.
    if sys.byteorder != "little":
        msg = "graph files can only be used on little-endian hosts"
        raise FormatError(msg)


def write_container(
    stream: BinaryIO,
    magic: bytes,
    node_count: int,
    edge_count: int,
    sections: Sequence[Section],
) -> FileDigest:
    """Writes a container to a binary stream.

    Args:
        stream: Where to write; it should be positioned at the file's start.
        magic: The file's 8-byte magic number, such as `b"VRPGRAPH"`.
        node_count: N, stored in the header.
        edge_count: E, stored in the header.
        sections: The sections, in the order they are written.

    Returns:
        The size and SHA-256 of what was written.

    Raises:
        ValueError: If the magic number, a tag or a count is invalid.
    """
    _require_little_endian()
    if len(magic) != _MAGIC_SIZE:
        msg = f"the magic number must be {_MAGIC_SIZE} bytes"
        raise ValueError(msg)
    if not 0 <= node_count <= MAX_COUNT or not 0 <= edge_count <= MAX_COUNT:
        msg = f"node and edge counts must be at most {MAX_COUNT}"
        raise ValueError(msg)
    for section in sections:
        if len(section.tag) != _TAG_SIZE or not section.tag.isascii():
            msg = f"tag {section.tag!r} is not four ASCII characters"
            raise ValueError(msg)

    hasher = hashlib.sha256()
    size = 0

    def emit(data: bytes | memoryview) -> None:
        nonlocal size
        stream.write(data)
        hasher.update(data)
        size += len(data)

    # The table needs every section's size first. Computing it from the
    # length avoids holding a second copy of the arrays as bytes.
    byte_sizes = [
        section.values.size
        * np.dtype(_SCALAR_TYPES[section.element_type]).itemsize
        for section in sections
    ]
    emit(
        _HEADER.pack(
            magic, FORMAT_VERSION, len(sections), node_count, edge_count, 0, 0
        )
    )
    offset = _HEADER.size + _ENTRY.size * len(sections)
    for section, byte_size in zip(sections, byte_sizes, strict=True):
        emit(
            _ENTRY.pack(
                section.tag.encode("ascii"),
                section.element_type,
                offset,
                section.values.size,
            )
        )
        offset += byte_size + _padding(byte_size)
    for section, byte_size in zip(sections, byte_sizes, strict=True):
        # No copy when the array already has the right type and layout.
        array = np.ascontiguousarray(
            section.values, dtype=_SCALAR_TYPES[section.element_type]
        )
        emit(memoryview(array).cast("B"))
        emit(bytes(_padding(byte_size)))
    return FileDigest(size_bytes=size, sha256=hasher.hexdigest())


def read_container(
    data: bytes, magic: bytes, required: Mapping[str, ElementType]
) -> Container:
    """Reads a container and runs the spec's container-level checks.

    Sections whose tags are not in `required` are skipped. Element counts of
    the required sections are not checked here, because they depend on the
    file type; the caller checks them.

    Args:
        data: The whole file.
        magic: The magic number the file must start with.
        required: The tags the file must contain, each exactly once, and
            their element types.

    Returns:
        The header counts and the required sections, as read-only arrays.

    Raises:
        FormatError: If the file breaks a rule of the format.
    """
    _require_little_endian()
    if len(data) < _HEADER.size:
        msg = f"the file is {len(data)} bytes, shorter than its header"
        raise FormatError(msg)
    header = _Header._make(_HEADER.unpack_from(data))
    _check_header(header, magic)

    table_end = _HEADER.size + _ENTRY.size * header.section_count
    if table_end > len(data):
        msg = "the section table runs past the end of the file"
        raise FormatError(msg)

    sections: dict[str, IntArray] = {}
    expected_offset = table_end
    for index in range(header.section_count):
        raw_tag, type_code, offset, count = _ENTRY.unpack_from(
            data, _HEADER.size + _ENTRY.size * index
        )
        entry = _Entry(
            raw_tag.decode("ascii", errors="replace"), type_code, offset, count
        )
        scalar_type = _scalar_type(entry)
        item_size = np.dtype(scalar_type).itemsize
        _check_placement(entry, item_size, expected_offset, len(data))
        end = entry.offset + entry.element_count * item_size
        expected_offset = end + _padding(end)
        if entry.tag not in required:
            continue
        if entry.tag in sections:
            msg = f"section {entry.tag} appears more than once"
            raise FormatError(msg)
        if entry.type_code != required[entry.tag]:
            msg = f"section {entry.tag} has element type {entry.type_code}"
            raise FormatError(msg)
        sections[entry.tag] = np.frombuffer(
            data,
            dtype=scalar_type,
            count=entry.element_count,
            offset=entry.offset,
        )

    if expected_offset != len(data):
        msg = (
            f"the file is {len(data)} bytes, but its sections end at "
            f"{expected_offset}"
        )
        raise FormatError(msg)
    missing = sorted(required.keys() - sections.keys())
    if missing:
        msg = f"required sections are missing: {', '.join(missing)}"
        raise FormatError(msg)
    return Container(
        node_count=header.node_count,
        edge_count=header.edge_count,
        sections=sections,
    )


def _check_header(header: _Header, magic: bytes) -> None:
    if header.magic != magic:
        msg = f"the magic number is {header.magic!r}, not {magic!r}"
        raise FormatError(msg)
    if header.version != FORMAT_VERSION:
        msg = f"format version {header.version} is not supported"
        raise FormatError(msg)
    if header.flags != 0:
        msg = f"flags are {header.flags:#x}; version 1 defines none"
        raise FormatError(msg)
    if header.node_count > MAX_COUNT or header.edge_count > MAX_COUNT:
        msg = f"node and edge counts must be at most {MAX_COUNT}"
        raise FormatError(msg)


def _scalar_type(
    entry: _Entry,
) -> type[np.uint32] | type[np.int32] | type[np.int64]:
    try:
        return _SCALAR_TYPES[ElementType(entry.type_code)]
    except ValueError:
        msg = f"section {entry.tag} has unknown element type {entry.type_code}"
        raise FormatError(msg) from None


def _check_placement(
    entry: _Entry, item_size: int, expected_offset: int, file_size: int
) -> None:
    if entry.offset != expected_offset:
        msg = (
            f"section {entry.tag} starts at {entry.offset}; the format "
            f"requires {expected_offset}, right after the previous section"
        )
        raise FormatError(msg)
    # Overflow-safe: never compute offset + count * item_size first.
    if (
        entry.offset > file_size
        or entry.element_count > (file_size - entry.offset) // item_size
    ):
        msg = f"section {entry.tag} runs past the end of the file"
        raise FormatError(msg)

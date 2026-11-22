import hashlib
import io
import struct

import numpy as np
import numpy.typing as npt
import pytest
from hypothesis import given
from hypothesis import strategies as st

from vrp_preprocessor.container import (
    MAX_COUNT,
    ElementType,
    FormatError,
    Section,
    read_container,
    write_container,
)

MAGIC = b"VRPTESTS"
REQUIRED = {"AAAA": ElementType.UINT32, "BBBB": ElementType.INT64}
ELEMENT_RANGES = {
    ElementType.UINT32: (0, 2**32 - 1, np.uint32),
    ElementType.INT32: (-(2**31), 2**31 - 1, np.int32),
    ElementType.INT64: (-(2**63), 2**63 - 1, np.int64),
}

# Byte offsets in the sample: header fields, then the two table entries.
FLAGS_AT = 24
SECTION_COUNT_AT = 12
FIRST_ENTRY_AT = 32
SECOND_ENTRY_AT = 56


@st.composite
def section_lists(draw: st.DrawFn) -> list[Section]:
    """Any sections: distinct printable tags, any types, any values."""
    tag = st.text(
        alphabet=st.characters(min_codepoint=33, max_codepoint=126),
        min_size=4,
        max_size=4,
    )
    result = []
    for name in draw(st.lists(tag, unique=True, max_size=6)):
        element_type = draw(st.sampled_from(list(ElementType)))
        low, high, scalar = ELEMENT_RANGES[element_type]
        values: npt.NDArray[np.uint32 | np.int32 | np.int64] = np.array(
            draw(st.lists(st.integers(low, high), max_size=20)), dtype=scalar
        )
        result.append(Section(name, element_type, values))
    return result


# Covers the layout: alignment, padding, offsets, extreme values, digest.
@given(section_lists(), st.integers(0, MAX_COUNT), st.integers(0, MAX_COUNT))
def test_any_sections_round_trip(
    sections: list[Section], node_count: int, edge_count: int
) -> None:
    stream = io.BytesIO()
    digest = write_container(stream, MAGIC, node_count, edge_count, sections)
    data = stream.getvalue()
    assert len(data) % 8 == 0
    assert digest.size_bytes == len(data)
    assert digest.sha256 == hashlib.sha256(data).hexdigest()
    required = {section.tag: section.element_type for section in sections}
    container = read_container(data, MAGIC, required)
    assert (container.node_count, container.edge_count) == (
        node_count,
        edge_count,
    )
    for section in sections:
        read_back = container.sections[section.tag]
        assert read_back.dtype == section.values.dtype
        assert read_back.tolist() == section.values.tolist()


def sample(*, with_extra_section: bool = False) -> bytearray:
    """Three uint32s (padded to 16 bytes), then two int64s."""
    sections = [
        Section("AAAA", ElementType.UINT32, np.array([1, 2, 3], np.uint32)),
        Section("BBBB", ElementType.INT64, np.array([-1, 2**40], np.int64)),
    ]
    if with_extra_section:
        sections.insert(
            1, Section("NEW!", ElementType.INT32, np.array([5], np.int32))
        )
    stream = io.BytesIO()
    write_container(stream, MAGIC, 3, 5, sections)
    return bytearray(stream.getvalue())


def test_unknown_sections_are_skipped() -> None:
    container = read_container(
        bytes(sample(with_extra_section=True)), MAGIC, REQUIRED
    )
    assert set(container.sections) == {"AAAA", "BBBB"}


def patched(fmt: str, at: int, value: int | bytes) -> bytes:
    data = sample()
    struct.pack_into(fmt, data, at, value)
    return bytes(data)


# One case per reader check of the spec.
@pytest.mark.parametrize(
    ("data", "message"),
    [
        (bytes(sample())[:31], "shorter than its header"),
        (b"VRPWRONG" + bytes(sample())[8:], "magic number"),
        (patched("<I", 8, 2), "format version 2"),
        (patched("<I", FLAGS_AT, 1), "flags"),
        (patched("<I", 16, MAX_COUNT + 1), "at most"),
        (patched("<I", SECTION_COUNT_AT, 1000), "section table runs past"),
        (patched("<I", FIRST_ENTRY_AT + 4, 9), "unknown element type 9"),
        (patched("<I", FIRST_ENTRY_AT + 4, 2), "element type 2"),
        (patched("<Q", FIRST_ENTRY_AT + 8, 88), "starts at 88"),
        # Would overflow 64 bits if computed as offset + count * size.
        (patched("<Q", FIRST_ENTRY_AT + 16, 2**63), "runs past the end"),
        (patched("<Q", SECOND_ENTRY_AT + 16, 1), "sections end at"),
        (bytes(sample()) + bytes(8), "sections end at"),
        (patched("4s", SECOND_ENTRY_AT, b"ZZZZ"), "missing: BBBB"),
        (patched("4s", SECOND_ENTRY_AT, b"AAAA"), "more than once"),
    ],
)
def test_rejects_files_that_break_the_format(data: bytes, message: str) -> None:
    with pytest.raises(FormatError, match=message):
        read_container(data, MAGIC, REQUIRED)

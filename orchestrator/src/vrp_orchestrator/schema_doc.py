"""Writes the database schema's documentation, `docs/formats/database.md`.

    python -m vrp_orchestrator.schema_doc <conninfo> <output>

Everything comes from the catalogue of a migrated database: the tables,
columns, keys, and the descriptions that the migrations attach with
`COMMENT ON`. CI regenerates the page and fails if it changed.
"""

import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Final

import psycopg
from psycopg.rows import dict_row

_TABLES: Final = """
    SELECT c.oid, c.relname AS name,
           coalesce(obj_description(c.oid, 'pg_class'), '') AS comment
    FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE n.nspname = 'public' AND c.relkind = 'r'
    ORDER BY c.relname
"""
_COLUMNS: Final = """
    SELECT a.attname AS name, format_type(a.atttypid, a.atttypmod) AS type,
           NOT a.attnotnull AS nullable,
           coalesce(col_description(a.attrelid, a.attnum), '') AS comment
    FROM pg_attribute a
    WHERE a.attrelid = %s AND a.attnum > 0 AND NOT a.attisdropped
    ORDER BY a.attnum
"""
_KEYS: Final = """
    SELECT k.contype AS kind, k.conrelid::regclass::text AS child,
           k.confrelid::regclass::text AS parent,
           array(
               SELECT a.attname
               FROM unnest(k.conkey) WITH ORDINALITY AS c(number, place)
               JOIN pg_attribute a
                 ON a.attrelid = k.conrelid AND a.attnum = c.number
               ORDER BY c.place
           ) AS columns
    FROM pg_constraint k JOIN pg_namespace n ON n.oid = k.connamespace
    WHERE n.nspname = 'public' AND k.contype IN ('p', 'f')
    ORDER BY child, kind, columns
"""
_HEADER: Final = """\
# Database schema

The orchestrator's PostgreSQL tables (ADR 0001); no other program reads or
writes them (ADR 0003). This page is generated from the migrated schema by
`python -m vrp_orchestrator.schema_doc`: change the migrations and their
`COMMENT ON` descriptions in `orchestrator/src/vrp_orchestrator/repository.py`,
then regenerate it.
"""


def describe(conninfo: str) -> str:
    """The documentation of a migrated database's schema.

    Args:
        conninfo: A libpq connection string or URL.

    Returns:
        The Markdown page.
    """
    with psycopg.connect(conninfo, row_factory=dict_row) as connection:
        tables = connection.execute(_TABLES).fetchall()
        columns = {
            table["name"]: connection.execute(
                _COLUMNS, (table["oid"],)
            ).fetchall()
            for table in tables
        }
        keys = connection.execute(_KEYS).fetchall()

    primary = {
        (key["child"], column)
        for key in keys
        if key["kind"] == "p"
        for column in key["columns"]
    }
    foreign = {
        (key["child"], column)
        for key in keys
        if key["kind"] == "f"
        for column in key["columns"]
    }

    def marks(table: str, column: str) -> list[str]:
        return [
            mark
            for mark, keyed in (("PK", primary), ("FK", foreign))
            if (table, column) in keyed
        ]

    diagram = ["```mermaid", "erDiagram"]
    diagram += [
        f'    {key["parent"]} ||--o{{ {key["child"]} : "{label}"'
        for key in keys
        if key["kind"] == "f"
        for label in [", ".join(key["columns"])]
    ]
    for table in tables:
        diagram.append(f"    {table['name']} {{")
        for column in columns[table["name"]]:
            mark = ", ".join(marks(table["name"], column["name"]))
            # Mermaid types may not contain spaces.
            kind = _short(column["type"]).replace(" ", "_")
            diagram.append(f"        {kind} {column['name']} {mark}".rstrip())
        diagram.append("    }")
    diagram.append("```")

    sections = [_HEADER, "\n".join(diagram) + "\n"]
    for table in tables:
        rows = [
            [
                f"`{column['name']}`",
                _short(column["type"]),
                ", ".join(marks(table["name"], column["name"])),
                "yes" if column["nullable"] else "",
                column["comment"],
            ]
            for column in columns[table["name"]]
        ]
        header = ["Column", "Type", "Key", "Null", "Meaning"]
        title = f"## `{table['name']}`"
        sections.append(
            f"{title}\n\n{table['comment']}\n\n{_table(header, rows)}"
        )
    return "\n".join(sections)


def _short(kind: str) -> str:
    """A column type, with PostgreSQL's short name where it has one."""
    return kind.replace("timestamp with time zone", "timestamptz")


def _table(header: list[str], rows: Sequence[list[str]]) -> str:
    """A Markdown table with aligned columns."""
    widths = [
        max(len(row[index]) for row in [header, *rows])
        for index in range(len(header))
    ]

    def line(cells: list[str]) -> str:
        padded = (
            cell.ljust(width) for cell, width in zip(cells, widths, strict=True)
        )
        return "| " + " | ".join(padded) + " |"

    rule = "|" + "|".join("-" * (width + 2) for width in widths) + "|"
    return "\n".join([line(header), rule, *map(line, rows)]) + "\n"


def main(argv: Sequence[str]) -> int:
    """Writes the page of the database at `argv[0]` to the file `argv[1]`.

    Args:
        argv: The connection string and the output path.

    Returns:
        The process exit code.
    """
    if len(argv) != 2:  # noqa: PLR2004
        sys.stderr.write("usage: python -m vrp_orchestrator.schema_doc ")
        sys.stderr.write("<conninfo> <output>\n")
        return 2
    Path(argv[1]).write_text(describe(argv[0]), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

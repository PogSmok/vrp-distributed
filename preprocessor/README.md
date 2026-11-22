# vrp-preprocessor

Turns OpenStreetMap extracts into versioned road graphs in the format of
`docs/formats/graph.md` (ADR 0005, 0015, 0027), and writes synthetic graphs
for testing. `vrp-preprocessor problem` writes a random problem on a graph
version, in the format of `docs/formats/problem.md` (ADR 0006). It is a
member of the Python workspace defined by the root `pyproject.toml`.

## Setup

Install [uv](https://docs.astral.sh/uv/), then, from the repository root:

```sh
uv sync --all-packages # creates .venv from uv.lock
uv run vrp-preprocessor --version
```
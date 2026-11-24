"""The client application: `streamlit run client/src/vrp_client/app.py`."""

import json
from functools import partial
from pathlib import Path
from typing import Final

import grpc
import streamlit as st
from streamlit.components.v1 import declare_component
from streamlit.navigation.page import StreamlitPage

from vrp_client import vrp_pb2
from vrp_client.api import Orchestrator, rpc_status
from vrp_client.files import (
    FileFormatError,
    complete_rows,
    parse_problem,
    problem_file,
)
from vrp_client.job_view import JobView, JobViews, stop_positions

_HERE: Final = Path(__file__).parent
_COST_NAMES: Final = {"length": "Distance", "time": "Travel time"}
_MAX_INTEGER: Final = 2**32 - 1
_BUILT: Final = "built_problem"
_TOKEN: Final = "token"  # noqa: S105  # a key; its value is this tab's token
_MAP: Final = "map"  # the component's key; its value is the plan it holds
# A problem around Warsaw, to edit or replace.
_EXAMPLE: Final = json.loads((_HERE / "example_problem.json").read_bytes())
_map = declare_component("vrp_map", path=str(_HERE / "map"))


@st.cache_resource
def _orchestrator(token: str) -> Orchestrator:
    return Orchestrator.from_environment(token)


@st.cache_resource
def _jobs(token: str) -> JobViews:  # noqa: ARG001  # `token` is the cache key
    """The token's jobs, so that no user sees another's (ADR 0038)."""
    return JobViews()


def _reason(error: grpc.RpcError) -> str:
    return rpc_status(error)[1]


def _style(name: str) -> None:
    style = (_HERE / name).read_text("utf-8")
    st.html(f"<style>{style}</style>")


def _load_problem(upload: bytes | None) -> vrp_pb2.Problem | None:
    if upload is None:
        return None
    try:
        return parse_problem(upload)
    except FileFormatError as error:
        st.sidebar.error(str(error))
        return None


def _problem_source() -> vrp_pb2.Problem | None:
    """The hand-built problem, or else the uploaded file."""
    built: bytes | None = st.session_state.get(_BUILT)
    if built is None:
        upload = st.file_uploader("Problem file", type="json")
        return _load_problem(upload.getvalue() if upload else None)
    problem = _load_problem(built)
    count = len(problem.customers) if problem else 0
    st.success(f"Hand-built problem: {count:,} customers")
    if st.button("Use a file instead", width="stretch"):
        del st.session_state[_BUILT]
        st.rerun()
    return problem


def _new_job_form(api: Orchestrator, jobs: JobViews) -> vrp_pb2.Problem | None:
    """The sidebar's form; submits a job. Returns the problem to preview."""
    try:
        graphs = api.graphs()
    except grpc.RpcError as error:
        st.error(f"Cannot reach the orchestrator: {_reason(error)}")
        return None
    if not graphs:
        st.info("No graph is registered yet. See `deploy/README.md`.")
        return None
    names = {
        g.graph_version: f"{g.region} · {g.node_count:,} nodes" for g in graphs
    }
    version = st.selectbox("Road graph", names, format_func=names.__getitem__)
    problem = _problem_source()
    st.subheader("Search settings")
    limit = st.number_input("Time limit (s)", 1, 86_400, 60)
    stagnation = st.number_input("Stop after no gain (s, 0 = never)", 0)
    tasks = st.number_input("Workers (0 = all)", 0, 64, 0)
    islands = st.number_input("Islands per worker", 1, 32, 5)
    seed = st.number_input("Seed", 0, _MAX_INTEGER, 1)
    if st.button(
        "Start solving",
        type="primary",
        disabled=problem is None or version is None,
        width="stretch",
    ):
        parameters = vrp_pb2.SolveParameters(
            task_count=int(tasks),
            islands_per_task=int(islands),
            seed=int(seed),
            time_limit_seconds=float(limit),
            stagnation_seconds=float(stagnation),
        )
        _submit(api, jobs, problem, str(version), parameters)
    return problem


def _submit(
    api: Orchestrator,
    jobs: JobViews,
    problem: vrp_pb2.Problem | None,
    version: str,
    parameters: vrp_pb2.SolveParameters,
) -> None:
    if problem is None:
        return
    try:
        job_id = api.submit(problem, version, parameters)
    except grpc.RpcError as error:
        st.error(_reason(error))
        return
    jobs.add(
        JobView(
            job_id,
            stops=stop_positions(problem),
            vehicles=problem.vehicle_count,
        )
    )
    st.query_params["job"] = job_id
    st.rerun()


def _job_controls(api: Orchestrator, view: JobView) -> None:
    st.caption(f"Job `{view.job_id[:12]}`")
    if view.finished:
        if view.result_file:
            name = f"vrp-result-{view.job_id[:12]}"
            st.download_button(
                "Export result (JSON)",
                view.result_file,
                file_name=f"{name}.json",
                mime="application/json",
                on_click="ignore",
                type="primary",
                width="stretch",
            )
            st.download_button(
                "Export routes (GeoJSON)",
                view.geojson,
                file_name=f"{name}.geojson",
                mime="application/geo+json",
                on_click="ignore",
                width="stretch",
            )
        if st.button("New job", width="stretch"):
            del st.query_params["job"]
            st.rerun()
    elif st.button("Cancel job", width="stretch"):
        try:
            api.cancel(view.job_id)
        except grpc.RpcError as error:
            st.error(_reason(error))


def _draw(
    api: Orchestrator,
    view: JobView | None,
    preview: list[list[float]],
    *,
    running: bool,
) -> None:
    """The map; while a job runs, this reruns on its own to poll it."""
    payload: dict[str, object] = {"stops": preview, "phase": "idle"}
    if view is not None:
        view.refresh(api)
        held = st.session_state.get(_MAP) or {}
        payload = view.payload(
            held["cost"] if held.get("job") == view.job_id else 0
        )
    _map(view=payload, key=_MAP, default=None)
    # `running` is from the page's last full run: another tab may have seen
    # the job end first.
    if running and view is not None and view.finished:
        st.rerun()  # stops the polling


def _solve_page() -> None:
    _style("solve_page.css")
    token: str = st.session_state[_TOKEN]
    api, jobs = _orchestrator(token), _jobs(token)
    job_id = st.query_params.get("job")
    view = jobs.get(job_id) if job_id else None
    preview: list[list[float]] = []
    with st.sidebar:
        if view is None:
            problem = _new_job_form(api, jobs)
            preview = stop_positions(problem) if problem else []
        else:
            _job_controls(api, view)
    running = view is not None and not view.finished
    draw = st.fragment(_draw, run_every=1.0 if running else None)
    draw(api, view, preview, running=running)


def _builder_form() -> tuple[bytes | None, list[list[float]], int, int]:
    """The inputs. Returns the file, the stops, and total demand and fleet."""
    vehicles_input, capacity_input = st.columns(2)
    vehicles = vehicles_input.number_input(
        "Vehicle limit",
        1,
        _MAX_INTEGER,
        _EXAMPLE["vehicle_count"],
        key="builder_vehicles",
    )
    capacity = capacity_input.number_input(
        "Capacity per vehicle",
        1,
        _MAX_INTEGER,
        _EXAMPLE["vehicle_capacity"],
        key="builder_capacity",
    )
    edge_cost = st.radio(
        "Minimise",
        tuple(_COST_NAMES),
        format_func=_COST_NAMES.__getitem__,
        horizontal=True,
        key="builder_cost",
    )
    st.subheader("Depot")
    latitude_input, longitude_input = st.columns(2)
    latitude = latitude_input.number_input(
        "Latitude",
        -90.0,
        90.0,
        _EXAMPLE["depot"]["latitude"],
        0.001,
        "%.6f",
        key="builder_latitude",
    )
    longitude = longitude_input.number_input(
        "Longitude",
        -180.0,
        180.0,
        _EXAMPLE["depot"]["longitude"],
        0.001,
        "%.6f",
        key="builder_longitude",
    )
    st.subheader("Customers")
    rows = st.data_editor(
        _EXAMPLE["customers"],
        key="builder_table",
        num_rows="dynamic",
        hide_index=True,
        width="stretch",
        column_config={
            "latitude": st.column_config.NumberColumn(
                "Latitude", min_value=-90.0, max_value=90.0, format="%.6f"
            ),
            "longitude": st.column_config.NumberColumn(
                "Longitude", min_value=-180.0, max_value=180.0, format="%.6f"
            ),
            "demand": st.column_config.NumberColumn(
                "Demand", min_value=0, max_value=_MAX_INTEGER, step=1
            ),
        },
    )
    stops = [[longitude, latitude]]
    fleet = int(vehicles) * int(capacity)
    try:
        customers = complete_rows(rows)
        stops += [[c["longitude"], c["latitude"]] for c in customers]
        demand = int(sum(c["demand"] for c in customers))
        text = problem_file(
            edge_cost=str(edge_cost),
            vehicle_count=int(vehicles),
            vehicle_capacity=int(capacity),
            latitude=latitude,
            longitude=longitude,
            customers=customers,
        )
        parse_problem(text)
    except FileFormatError as error:
        st.error(str(error))
        return None, stops, 0, fleet
    return text, stops, demand, fleet


def _builder_page(solve: StreamlitPage) -> None:
    _style("builder_page.css")
    st.title("Build a problem")
    st.caption(
        "Enter the fleet, depot and customers. Stops must be within 500 m of "
        "a road on the graph you solve on."
    )
    form, side = st.columns([5, 7], gap="large")
    with form:
        text, stops, demand, fleet = _builder_form()
    with side:
        counts = st.columns(3)
        counts[0].metric("Customers", f"{len(stops) - 1:,}")
        counts[1].metric("Total demand", f"{demand:,}")
        counts[2].metric(
            "Fleet load", f"{100 * demand / fleet:.0f}%" if fleet else "—"
        )
        _map(view={"stops": stops, "phase": "idle"}, key="builder_map")
        use, download = st.columns(2)
        if use.button(
            "Solve this problem",
            type="primary",
            disabled=text is None,
            width="stretch",
        ):
            st.session_state[_BUILT] = text
            st.switch_page(solve)
        download.download_button(
            "Download problem file",
            text or b"",
            file_name="problem.json",
            mime="application/json",
            disabled=text is None,
            width="stretch",
        )


def _sign_in_page() -> None:
    """Asks for a client token (ADR 0038), and checks it with a call."""
    st.title("Sign in")
    with st.form("sign_in"):
        token = st.text_input(
            "Token",
            type="password",
            help="From `admin.py client <your name>`; see `deploy/README.md`.",
        ).strip()
        if not st.form_submit_button("Sign in", type="primary"):
            return
    try:
        Orchestrator.from_environment(token).graphs()
    except (OSError, ValueError) as error:
        st.error(f"The client is not configured: {error}")
        return
    except grpc.RpcError as error:
        st.error(_reason(error))
        return
    st.session_state[_TOKEN] = token
    st.rerun()


def main() -> None:
    """Draws the app."""
    st.set_page_config(page_title="VRP Control", page_icon="🛰️", layout="wide")
    # The primary colour is bright in the dark theme, and deep in the light.
    ink = "#04121a" if st.context.theme.type == "dark" else "#ffffff"
    style = (_HERE / "style.css").read_text("utf-8")
    st.html(f"<style>:root {{ --vrp-ink: {ink}; }} {style}</style>")
    if _TOKEN not in st.session_state:
        sign_in = st.Page(_sign_in_page, title="Sign in", icon=":material/key:")
        st.navigation([sign_in], position="hidden").run()
        return
    solve = st.Page(
        _solve_page, title="Solve", icon=":material/route:", default=True
    )
    build = st.Page(
        partial(_builder_page, solve),
        title="Build a problem",
        icon=":material/edit_location_alt:",
        url_path="build",
    )
    st.navigation([solve, build]).run()
    if st.sidebar.button("Sign out", width="stretch"):
        del st.session_state[_TOKEN]
        st.rerun()


main()

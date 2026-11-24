# vrp-client

Submits a job, follows its search live on a map, and shows the final routes on
the roads (ADR 0012, 0030). It is a Streamlit application and a member of the
Python workspace defined by the root `pyproject.toml`.

## Contents

| Module        | What it is                                                         | ADR        |
|---------------|--------------------------------------------------------------------|------------|
| `app.py`      | The Solve and Build a problem pages, and the polling               | 0012       |
| `api.py`      | The orchestrator's client API, and result downloads                | 0008, 0009 |
| `job_view.py` | One job as the map shows it, built up from polls                   | 0030       |
| `files.py`    | Problem and result files, checked against `docs/formats/`          | 0006, 0028 |
| `map/`        | The map component: deck.gl and MapLibre, with no build step        | 0030       |
| `vrp_pb2`     | The contract, `proto/vrp/v1/vrp.proto`, generated when it is built | 0008       |

## Building a problem

**Build a problem** takes the fleet, the depot and a table of customers by
hand, and shows them on the map as you type. It starts with an example of 400
customers around Warsaw. It checks the same rules as a problem file, then
solves the problem directly or downloads it as one.

## Exporting a result

A finished job offers two downloads in the sidebar: the result file exactly as
stored (`docs/formats/result.md`), and GeoJSON with a line per vehicle and a
point per stop, for map tools such as QGIS or geojson.io.

## What the map shows

- **The best plan** in neon along the real roads, one colour per direction
  from the depot, with pulses travelling along each route. The cost pulses
  when a better plan arrives.
- **Older plans** stay on the map, dimmer the older they are, so that you can
  see where the search has moved. The last six are shown.
- **Convergence.** Drag across the chart to replay the plans seen so far.
- **Islands** with each one's best cost.

Nothing is drawn until a plan exists. A worker routes each new best on the
roads in the background (ADR 0030), so the map and the reported cost can
differ by a moment; the cost shown is the latest the islands have reported.
Everything drawn is a real solution from the solver. The page follows the
system's dark or light theme.

The map's libraries come from jsDelivr, at exact versions with integrity
hashes, and its basemap from [OpenFreeMap](https://openfreemap.org), with
tiles from [OpenMapTiles](https://openmaptiles.org). Without the basemap,
routes still draw on a plain background. Routes and basemap are
© [OpenStreetMap contributors](https://www.openstreetmap.org/copyright), and
the map always shows that credit, which MapLibre never collapses.

## Running it

`deploy/README.md` starts it with the rest of the control plane. To run it on
its own against an orchestrator:

```sh
cd client
export VRP_ORCHESTRATOR=server.lab:50051
export VRP_TOKEN_FILE=token VRP_CA_CERTIFICATE=ca.pem
uv run streamlit run src/vrp_client/app.py
```

`VRP_INSECURE=1` replaces the certificate for local development only. Make a
problem file with `vrp-preprocessor problem` (`preprocessor/README.md`).

Known limits, to change when they matter:

- A job's map needs its problem, which this server keeps for its latest 20
  jobs. Opened after a client restart, a finished job still shows its routes,
  but a running one shows no stops until its result arrives.
- Replay holds at most 60 of the plans seen since the page opened, fewer when
  they are large.
- A road plan over 3 MB is not sent, so a very large problem may show its
  routes only once the result arrives; untested at 5,000 customers.
- No login, so the client listens on localhost only (ADR 0031).
- The job view and the map's JavaScript have no automated tests.

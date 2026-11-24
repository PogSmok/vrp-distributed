# Architecture views

Exported from [workspace.dsl](workspace.dsl) by `export.py`; do not edit by hand.

## System context

```mermaid
graph LR

    1["<b>Dispatcher</b><br/><small>[Person]</small><br/>Submits problems and reviews routes."]
    2["<b>System Administrator</b><br/><small>[Person]</small><br/>Runs the system and graph imports."]
    3["<b>VRP-Distributed</b><br/><small>[Software System]</small><br/>Distributed GPU solver for the Vehicle Routing Problem."]
    30["<b>OpenStreetMap</b><br/><small>[Software System]</small><br/>Road network source (offline import)."]
    31["<b>Prometheus</b><br/><small>[Software System]</small><br/>Collects and stores metrics."]
    32["<b>Grafana</b><br/><small>[Software System]</small><br/>Shows dashboards."]

    1-.->3
    3-.->30
    31-.->3
    32-.->31
    2-.->32
    2-.->3

```

## Containers

```mermaid
graph LR

    1["<b>Dispatcher</b><br/><small>[Person]</small><br/>Submits problems and reviews routes."]
    2["<b>System Administrator</b><br/><small>[Person]</small><br/>Runs the system and graph imports."]
    30["<b>OpenStreetMap</b><br/><small>[Software System]</small><br/>Road network source (offline import)."]
    31["<b>Prometheus</b><br/><small>[Software System]</small><br/>Collects and stores metrics."]

    subgraph 3 ["VRP-Distributed"]

      16["<b>Worker</b><br/><small>[Container: C++ / CUDA]</small><br/>Computes distances and runs the solver on one GPU, or on the CPU if built without CUDA."]
      26["<b>Graph Preprocessor</b><br/><small>[Container: Python CLI / pyrosm]</small><br/>Turns OSM extracts into versioned graphs."]
      27["<b>Admin Tool</b><br/><small>[Container: Python CLI]</small><br/>Creates secrets, issues worker configurations and client tokens, uploads and registers graphs."]
      28[("<b>Object Store</b><br/><small>[Container: SeaweedFS (S3)]</small><br/>Graphs, distance matrices and results.")]
      29[("<b>Job State DB</b><br/><small>[Container: PostgreSQL]</small><br/>Jobs, tasks, assignments, graph catalogue.")]
      4["<b>Client Application</b><br/><small>[Container: Python / Streamlit]</small><br/>Submits problems, shows routes and convergence."]
      5["<b>Orchestrator</b><br/><small>[Container: Python / grpcio (asyncio)]</small><br/>Schedules jobs on workers; owns job state and the migrant pool."]
    end

    1-. "Signs in with a token<br/><small>[HTTPS]</small>" .->4
    4-. "Submits jobs, polls progress<br/><small>[gRPC/TLS]</small>" .->5
    4-. "Downloads results<br/><small>[HTTPS, presigned]</small>" .->28
    5-.->29
    5-. "Manages artifacts<br/><small>[S3 API]</small>" .->28
    16-. "Gets tasks, reports progress, sends logs<br/><small>[gRPC/TLS]</small>" .->5
    16-. "Reads graphs, writes results<br/><small>[HTTPS, presigned]</small>" .->28
    26-.->30
    27-. "Uploads graphs<br/><small>[S3 API]</small>" .->28
    27-. "Registers graph versions<br/><small>[gRPC/TLS]</small>" .->5
    31-. "Scrapes metrics, discovers workers<br/><small>[HTTP]</small>" .->5
    31-.->29
    31-.->28
    31-.->16
    2-.->26
    2-.->27

```

## Orchestrator components

```mermaid
graph TB

    31["<b>Prometheus</b><br/><small>[Software System]</small><br/>Collects and stores metrics."]

    subgraph 3 ["VRP-Distributed"]

      subgraph 5 ["Orchestrator"]

        10["<b>Scheduler</b><br/><small>[Component: asyncio event loop]</small><br/>Job phases, leases, island groups. Decides every database write; applies it after commit."]
        11["<b>Worker Registry</b><br/><small>[Component: In memory, HTTP SD endpoint]</small><br/>Connected workers, their send queues and metrics addresses; serves worker discovery to Prometheus."]
        12["<b>Migrant Pool</b><br/><small>[Component: In memory]</small><br/>Migrants of registered islands per job; bypasses the Scheduler's queue."]
        13["<b>Graph Catalogue</b><br/><small>[Component: In memory, owned by Scheduler]</small><br/>Registered graph versions."]
        14["<b>Artifact Manager</b><br/><small>[Component: S3 client]</small><br/>Signs presigned URLs, checks uploads, verifies graphs, deletes expired objects."]
        15["<b>State Repository</b><br/><small>[Component: psycopg 3, commits outside the loop]</small><br/>All SQL; commits transitions in order, one transaction each; applies schema migrations at start-up."]
        6["<b>Client API</b><br/><small>[Component: gRPC service]</small><br/>Submits and cancels jobs, answers progress polls, returns result URLs; only for the caller's own jobs; limits poll rate."]
        7["<b>Worker API</b><br/><small>[Component: gRPC service]</small><br/>One bidirectional session stream per worker; a new session replaces the old one."]
        8["<b>Admin API</b><br/><small>[Component: gRPC service]</small><br/>Registers graph versions."]
        9["<b>Auth</b><br/><small>[Component: gRPC interceptor]</small><br/>Wraps all three APIs; checks named tokens and roles from the token file; identifies the caller."]
      end

      16["<b>Worker</b><br/><small>[Container: C++ / CUDA]</small><br/>Computes distances and runs the solver on one GPU, or on the CPU if built without CUDA."]
      27["<b>Admin Tool</b><br/><small>[Container: Python CLI]</small><br/>Creates secrets, issues worker configurations and client tokens, uploads and registers graphs."]
      28[("<b>Object Store</b><br/><small>[Container: SeaweedFS (S3)]</small><br/>Graphs, distance matrices and results.")]
      29[("<b>Job State DB</b><br/><small>[Container: PostgreSQL]</small><br/>Jobs, tasks, assignments, graph catalogue.")]
      4["<b>Client Application</b><br/><small>[Container: Python / Streamlit]</small><br/>Submits problems, shows routes and convergence."]
    end

    4-. "Downloads results<br/><small>[HTTPS, presigned]</small>" .->28
    16-. "Reads graphs, writes results<br/><small>[HTTPS, presigned]</small>" .->28
    27-. "Uploads graphs<br/><small>[S3 API]</small>" .->28
    31-.->29
    31-.->28
    31-.->16
    4-. "Submits, cancels and polls jobs<br/><small>[gRPC/TLS]</small>" .->6
    27-. "Registers graph versions<br/><small>[gRPC/TLS]</small>" .->8
    31-. "Discovers worker metrics endpoints<br/><small>[HTTP]</small>" .->11
    6-. "Checks token and role" .->9
    7-. "Checks token; gets the worker name for the session" .->9
    8-. "Checks admin token" .->9
    6-. "Posts commands and progress queries" .->10
    7-. "Posts worker events" .->10
    7-. "Opens and closes sessions" .->11
    7-. "Pushes and pulls migrants" .->12
    8-. "Verifies uploaded graphs" .->14
    8-. "Posts graph registrations" .->10
    10-. "Queues assignments" .->11
    10-. "Manages pools and islands" .->12
    10-.->13
    10-. "Signs URLs, requests upload checks and deletions" .->14
    14-. "Posts check and deletion results" .->10
    10-. "Hands over transitions" .->15
    15-. "Posts commit results" .->10
    15-. "<br/><small>[SQL]</small>" .->29
    14-. "<br/><small>[S3 API]</small>" .->28
    16-. "Session stream<br/><small>[gRPC/TLS]</small>" .->7

```

## Worker components

```mermaid
graph TB

    subgraph 3 ["VRP-Distributed"]

      subgraph 16 ["Worker"]

        17["<b>Session Client</b><br/><small>[Component: gRPC client, session thread]</small><br/>Registration, stream, heartbeats, reconnects, fresh URLs; resends unconfirmed reports."]
        18["<b>Task Runner</b><br/><small>[Component: Compute thread]</small><br/>Runs one task at a time in short steps; handles cancel."]
        19["<b>Graph Cache</b><br/><small>[Component]</small><br/>Verified graphs on disk; one graph resident on the GPU or in memory."]
        20["<b>Object Transfer</b><br/><small>[Component: libcurl]</small><br/>Presigned GET and PUT with retries."]
        21["<b>Snapper</b><br/><small>[Component: vrp-core / C++]</small><br/>Snaps coordinates to graph nodes."]
        22["<b>Distance Engine</b><br/><small>[Component: vrp-core / CUDA or C++]</small><br/>Many-source shortest paths."]
        23["<b>Island Solver</b><br/><small>[Component: vrp-core / CUDA or C++]</small><br/>Populations, local search, migrant import and export."]
        24["<b>Result Builder</b><br/><small>[Component: vrp-core / C++]</small><br/>Routes legs on the CPU, checks the cost, builds polylines and the result artifact."]
        25["<b>Plan Router</b><br/><small>[Component: Low-priority thread]</small><br/>Routes the latest best plan on the roads for the live map."]
      end

      28[("<b>Object Store</b><br/><small>[Container: SeaweedFS (S3)]</small><br/>Graphs, distance matrices and results.")]
      5["<b>Orchestrator</b><br/><small>[Container: Python / grpcio (asyncio)]</small><br/>Schedules jobs on workers; owns job state and the migrant pool."]
    end

    5-. "Manages artifacts<br/><small>[S3 API]</small>" .->28
    17-. "Session stream<br/><small>[gRPC/TLS]</small>" .->5
    17-. "Hands over assignments and cancels" .->18
    18-. "Reports progress, migrants, results, log lines" .->17
    18-. "Ensures graph is loaded" .->19
    18-. "Matrix rows and results" .->20
    19-. "Downloads graphs" .->20
    20-. "<br/><small>[HTTPS, presigned]</small>" .->28
    18-. "PREPARE" .->21
    18-. "DISTANCE" .->22
    18-. "SOLVE" .->23
    18-. "FINALIZE" .->24
    18-. "Offers each new best plan" .->25
    25-. "Sends road plans" .->17

```

## Deployment in the lab

```mermaid
graph TB

    subgraph 110 ["Worker PC"]

      111["<b>Worker</b><br/><small>[Container: C++ / CUDA]</small><br/>Computes distances and runs the solver on one GPU, or on the CPU if built without CUDA."]
    end

    subgraph 115 ["Admin Workstation"]

      116["<b>Graph Preprocessor</b><br/><small>[Container: Python CLI / pyrosm]</small><br/>Turns OSM extracts into versioned graphs."]
    end

    subgraph 91 ["Server PC"]

      subgraph 92 ["Docker Compose"]

        100[("<b>Object Store</b><br/><small>[Container: SeaweedFS (S3)]</small><br/>Graphs, distance matrices and results.")]
        104["<b>Prometheus</b><br/><small>[Software System]</small><br/>Collects and stores metrics."]
        108["<b>Grafana</b><br/><small>[Software System]</small><br/>Shows dashboards."]
        93["<b>Orchestrator</b><br/><small>[Container: Python / grpcio (asyncio)]</small><br/>Schedules jobs on workers; owns job state and the migrant pool."]
        94["<b>Client Application</b><br/><small>[Container: Python / Streamlit]</small><br/>Submits problems, shows routes and convergence."]
        96["<b>Admin Tool</b><br/><small>[Container: Python CLI]</small><br/>Creates secrets, issues worker configurations and client tokens, uploads and registers graphs."]
        98[("<b>Job State DB</b><br/><small>[Container: PostgreSQL]</small><br/>Jobs, tasks, assignments, graph catalogue.")]
      end

    end

    93-. "Manages artifacts<br/><small>[S3 API]</small>" .->100
    94-. "Downloads results<br/><small>[HTTPS, presigned]</small>" .->100
    96-. "Uploads graphs<br/><small>[S3 API]</small>" .->100
    104-. "Scrapes metrics, discovers workers<br/><small>[HTTP]</small>" .->93
    104-.->98
    104-.->100
    108-.->104
    111-. "Gets tasks, reports progress, sends logs<br/><small>[gRPC/TLS]</small>" .->93
    111-. "Reads graphs, writes results<br/><small>[HTTPS, presigned]</small>" .->100
    104-.->111
    94-. "Submits jobs, polls progress<br/><small>[gRPC/TLS]</small>" .->93
    96-. "Registers graph versions<br/><small>[gRPC/TLS]</small>" .->93
    93-.->98

```

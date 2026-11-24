# vrp-orchestrator

Schedules jobs on the workers and owns all job state (ADR 0003, 0021,
0026). It is a member of the Python workspace defined by the root
`pyproject.toml`; `uv sync --all-packages` installs it.

## Contents

| Module          | What it is                                                                       | ADR              |
|-----------------|----------------------------------------------------------------------------------|------------------|
| `vrp_pb2`       | The contract, `proto/vrp/v1/vrp.proto`, generated when the package is built      | 0008             |
| `model.py`      | The persisted rows: graphs, jobs, tasks, islands                                 | 0001, 0013       |
| `scheduler.py`  | Every scheduling decision, as a state machine with no I/O                        | 0013, 0014, 0021 |
| `runtime.py`    | Runs the Scheduler on the event loop: queue, retried commits, timer              | 0021, 0022       |
| `repository.py` | The State Repository: schema, migrations, the load at start-up, commits          | 0001, 0019       |
| `schema_doc.py` | Writes `docs/formats/database.md` from a migrated database's catalogue           | 0001             |
| `auth.py`       | The token file, the interceptor that checks tokens and roles, TLS credentials    | 0009             |
| `services.py`   | The Client, Admin and Worker APIs, which turn calls into the Scheduler's events  | 0002, 0012       |
| `registry.py`   | Connected workers: their send queues and metrics addresses                       | 0010, 0021       |
| `pool.py`       | The Migrant Pool, and each job's best plan on the roads for the live map         | 0004, 0030       |
| `artifacts.py`  | The Artifact Manager: presigned URLs, upload checks, graph checks, expiry        | 0009, 0016, 0029 |
| `telemetry.py`  | JSON-line logs with the workers' lines, metrics, worker discovery for Prometheus | 0010             |
| `config.py`     | The configuration file                                                           | 0019             |
| `main.py`       | `vrp-orchestrator`: starts every part, and runs the Scheduler                    | 0019             |

`docs/formats/protocol.md` explains the protocol: how the messages flow and
the rules both sides follow. `docs/formats/database.md` documents the
schema; it is generated, so change the migrations and their `COMMENT ON`
descriptions in `repository.py`, then regenerate it on a migrated, empty
database:

```sh
uv run python -m vrp_orchestrator.schema_doc <conninfo> docs/formats/database.md
```

A test checks that the page matches the schema.

## Running it

```sh
uv run vrp-orchestrator --config orchestrator.toml
```

`config.py` documents the file: the gRPC and metrics addresses, the
database, the token file, the log directory, TLS, and the object store
(SeaweedFS, ADR 0029), with the public address that presigned URLs are
signed for (ADR 0009). The database password and the store's keys can
come from `PGPASSWORD`, `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY`
instead. On start-up the orchestrator migrates the schema, loads its state
and creates the bucket if it is missing; SIGTERM or Ctrl+C stops it.

- **Logs** go to `<log_dir>/orchestrator.log` as JSON lines in UTC,
  rotated at midnight and kept 30 days; workers' lines are in the same
  file, tagged with the worker (ADR 0010).
- **Metrics** are on `http://<metrics>/metrics`, and the workers' metrics
  addresses on `http://<metrics>/workers`, for Prometheus's
  `http_sd_configs`.
- **Expiry.** Once a day, results and matrices older than 30 days are
  deleted (ADR 0016).

## How the Scheduler works

- **One commit at a time.** A transition is a set of new row versions. The
  Scheduler commits it, and only once the commit returns does it apply the
  rows in memory and send the messages (ADR 0022). While a commit is in
  flight, events that could change state wait in order. ADR 0021 asks only
  that the entities a commit touches wait; waiting for all of it is simpler
  and enough for 8 workers. Batching transitions is the revisit.
- **Answered at once.** Heartbeats, progress polls and URL requests change
  nothing durable, so a slow database never delays them.
- **One decision per transition.** Lease expiry, starting the next job,
  assigning a task and moving to the next phase are decided in that order,
  whenever nothing is in flight.
- **In memory only.** Lease deadlines, idle workers, the group wait and the
  last improvement are lost on a restart. Every lease then gets a grace
  period, and workers reconnect listing the attempts they hold (ADR 0014).
  Heartbeats do not extend two deadlines: an upload being checked, and a
  solve task told to stop. So a check that never answers, or a final
  report that never comes, fails the attempt rather than stall the job.
- **A bounded queue.** At most `max_waiting` events wait for a commit.
  Beyond that, clients are refused with `UNAVAILABLE`, and a worker's event
  raises `OverloadedError`: the process ends, and a restart recovers from
  the database while workers resend what was not confirmed.
- **Defaults** are in `scheduler.Settings`. The leases last 30 s and the
  group waits 60 s; a task has 3 attempts, and the solve phase restarts at
  most once.
- **CPU and GPU workers** (ADR 0039). The memory check (ADR 0007) counts
  what each phase allocates on the worker's device: the graph, plus one
  distance batch in DISTANCE on a GPU or one search per thread on a CPU, or
  the shared matrix and each island's working memory in SOLVE. A CPU
  worker's RAM also holds the graph's both directions. SOLVE goes to GPU
  workers first and the other phases to CPU workers first, and a solve
  group waits up to the group wait for its preferred workers.

Known limits, to change when they matter:

- **The memory check counts the graph in SOLVE**, although a GPU worker
  frees it then; on a GPU with little room, a solve task may wait for no
  reason.

- **Commits rewrite whole rows.** Each progress report writes its island's
  row again: its best solution, about 30 KB at 5,000 customers, and its
  whole convergence history, which grows during the run; the job's
  convergence too. That is a few MB for a 10-minute solve on 8 workers. If
  commits get slow, skip unchanged rows first, then keep convergence points
  in a table of their own.
- **`Scheduler._put` keeps memory in step implicitly.** Applying a row also
  starts or drops its lease deadline, marks its worker busy, and resets the
  solve phase's timers. This runs once, after the commit, for every
  decision; but the code that leases a task does not set its deadline, so a
  new state or field may need a matching line in `_put`.

## Tokens

The token file has one line per token, `<name> <role> <sha256>`, where the
role is `client`, `worker` or `admin` and the hash is of the token itself.
To make one:

```sh
uv run python -c "from vrp_orchestrator.auth import new_token, hash_token; t = new_token(); print(t, hash_token(t))"
```

Give the token to its owner, and add `<name> <role> <hash>` to the file.
Deleting the line revokes it; the file is re-read when it changes.

## Tests

`pytest` runs everything but the tests that need an empty PostgreSQL
database, or an S3 store with the keys `test`/`testtest`:

```sh
docker run -d --name vrp-test-postgres -e POSTGRES_PASSWORD=test -e POSTGRES_DB=vrp_test -p 127.0.0.1:55432:5432 postgres:18.7
VRP_TEST_DATABASE_URL=postgresql://postgres:test@127.0.0.1:55432/vrp_test uv run pytest -m postgres

# s3.json: {"identities": [{"name": "test", "credentials": [{"accessKey": "test", "secretKey": "testtest"}], "actions": ["Admin", "Read", "Write", "List", "Tagging"]}]}
docker run -d --name vrp-test-s3 -p 127.0.0.1:58333:8333 -v <dir with s3.json>:/etc/seaweedfs chrislusf/seaweedfs:4.54 server -s3 -s3.config=/etc/seaweedfs/s3.json
VRP_TEST_S3_URL=http://127.0.0.1:58333 uv run pytest -m s3
```

Each S3 test makes a bucket of its own. On Git Bash, set
`MSYS_NO_PATHCONV=1` for the `docker run`, or it rewrites the container's
paths.

They empty the database's `public` schema first. On Windows, psycopg's
asynchronous connections need `asyncio.SelectorEventLoop`, so the tests
start one.

## Generated code

`hatch_build.py` runs protoc when the package is built, and `[tool.uv]
cache-keys` makes `uv sync` rebuild it whenever `vrp.proto` changes. The
generated files are not committed.

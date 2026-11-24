# Deployment

One Compose file runs the control plane (ADR 0031): PostgreSQL, SeaweedFS, the
orchestrator, Prometheus, Grafana, the client, a PostgreSQL metrics exporter
with a read-only role (ADR 0003) and the nightly backup. Workers run natively
on their GPU machines. It needs Docker 26 or newer.

For a first run on one machine, `uv run --no-project deploy/demo.py` does the
steps below for you: it starts the stack, imports and registers a road graph,
and makes a worker configuration in `data/demo/` (`docs/setup.md`).

The deployment view of the [architecture model](../docs/architecture/README.md):

<!-- deployment:start -->
```mermaid
graph TB

    subgraph 110 ["GPU Worker PC"]

      111["<b>Worker</b><br/><small>[Container: C++ / CUDA]</small><br/>Computes distances and runs the solver on one GPU."]
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
        96["<b>Admin Tool</b><br/><small>[Container: Python CLI]</small><br/>Creates secrets, issues worker configurations, uploads and registers graphs."]
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
<!-- deployment:end -->

## Start it

From the repository root:

```sh
docker compose -f deploy/compose.yaml up --build --wait
```

Then open:

| Address                 | What it is                                   |
|-------------------------|----------------------------------------------|
| <http://localhost:8501> | The client: build a problem, solve, watch it |
| <http://localhost:3000> | Grafana: log in as `admin`, password below   |
| <http://localhost:9090> | Prometheus                                   |

```sh
docker compose -f deploy/compose.yaml run --rm admin grafana-password
```

The first start creates a private CA, TLS certificates, tokens and passwords
in the `secrets` volume, and each service sees only its own.

Certificates last a year. The `setup` service renews those within 30 days of
their end, but it runs only when the stack is brought up, and the services
read their certificates only when they start. So, once a year, renew them and
restart what uses them:

```sh
docker compose -f deploy/compose.yaml run --rm setup
docker compose -f deploy/compose.yaml restart orchestrator seaweedfs client
```

Workers reconnect within seconds and keep their tasks (`docs/failure-modes.md`).
When the CA itself was renewed, give each worker the new `ca.pem`.

## Open it to the lab

By default every port listens on this machine only. For workers on other
machines, give the name they reach this server by, and its lab address:

```sh
VRP_HOST=server.lab VRP_BIND=192.168.1.10 docker compose -f deploy/compose.yaml up --build --wait
```

| Port  | Service      | Reachable from         |
|-------|--------------|------------------------|
| 50051 | Orchestrator | `VRP_BIND`             |
| 8334  | Object store | `VRP_BIND`             |
| 3000  | Grafana      | `VRP_BIND`             |
| 8501  | Client       | This machine, no login |
| 9090  | Prometheus   | This machine, no login |

Limit those three ports to the lab's subnet (ADR 0019), and each worker's
metrics port to the server. Docker's published ports bypass `ufw`, so on Linux
add the rule to Docker's own chain:

```sh
# On the server. Replace the subnet and the network interface.
sudo iptables -I DOCKER-USER -i eth0 -p tcp -m multiport --dports 50051,8334,3000 ! -s 192.168.1.0/24 -j DROP
# On a Linux worker.
sudo ufw allow from 192.168.1.10 to any port 9101 proto tcp
```

```powershell
# On a Windows server.
New-NetFirewallRule -DisplayName "VRP server" -Direction Inbound -Protocol TCP -LocalPort 50051,8334,3000 -RemoteAddress 192.168.1.0/24 -Action Allow
# On a Windows worker.
New-NetFirewallRule -DisplayName "VRP worker metrics" -Direction Inbound -Protocol TCP -LocalPort 9101 -RemoteAddress 192.168.1.10 -Action Allow
```

## Add a road graph

Import one with `vrp-preprocessor` (`preprocessor/README.md`), then register
its directory:

```sh
docker compose -f deploy/compose.yaml run --rm -v "$PWD/mazowieckie:/graph:ro" admin register-graph /graph
```

## Add a worker

Each worker gets its own token. In an empty folder for that worker:

```sh
docker compose -f deploy/compose.yaml run --rm -T admin worker lab-pc-1 --metrics-address lab-pc-1:9101 > worker.json
docker compose -f deploy/compose.yaml run --rm -T admin ca > ca.pem
vrp-worker --config worker.json
```

In PowerShell 5, `>` writes UTF-16, which the worker cannot read: use
`cmd /c "... > worker.json"`. For a worker on the server itself, use
`--metrics-address host.docker.internal:9101`. To revoke a worker, delete its
line from `tokens.txt` in the volume. The demo names its workers `demo-…`, and
makes a new one whenever the CA changes; the old line stays until deleted.

### Run it as a service

A worker exits after a stall, a lost GPU or out of memory, and relies on its
service manager to restart it (ADR 0024, 0032).

On Linux, with `vrp-worker`, `worker.json` and `ca.pem` in `/opt/vrp-worker`:

```sh
sudo useradd --system --home /opt/vrp-worker vrp
sudo chown -R vrp /opt/vrp-worker && sudo chmod 600 /opt/vrp-worker/worker.json
sudo cp deploy/worker/vrp-worker.service /etc/systemd/system/
sudo systemctl enable --now vrp-worker
```

On Windows, put these in one folder, such as `C:\vrp-worker`: `vrp-worker.exe`,
`worker.json`, `ca.pem`, `deploy\worker\vrp-worker-service.xml`, and
[WinSW v2.12.0](https://github.com/winsw/winsw/releases/tag/v2.12.0)'s
`WinSW-x64.exe` renamed to `vrp-worker-service.exe`. From an administrator's
prompt:

```bat
vrp-worker-service.exe install
vrp-worker-service.exe start
```

If CUDA finds no GPU from a service on a consumer card (ADR 0019), run the
worker as a logon task with restarts instead:

```powershell
$action = New-ScheduledTaskAction -Execute "C:\vrp-worker\vrp-worker.exe" -Argument '--config "C:\vrp-worker\worker.json"' -WorkingDirectory "C:\vrp-worker"
$settings = New-ScheduledTaskSettingsSet -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit 0
Register-ScheduledTask -TaskName "VRP worker" -Action $action -Trigger (New-ScheduledTaskTrigger -AtLogOn) -Settings $settings
```

## Backups

The `backup` service dumps the database every night at 02:00 UTC into
`deploy/backups`, keeping the last 14. Set `VRP_BACKUP_DIR` to a folder that
reaches the admin workstation, such as a mounted share, or copy from there.
A second Compose project from the same checkout would share the folder, so
give it its own `VRP_BACKUP_DIR`.

```sh
scp 'server.lab:vrp-distributed/deploy/backups/*.dump' backups/
```

Results and matrices are not backed up (ADR 0019). To back up now, check that
the latest dump restores, or restore one:

```sh
docker compose -f deploy/compose.yaml exec backup sh /backup.sh now
docker compose -f deploy/compose.yaml exec backup sh /backup.sh verify

docker compose -f deploy/compose.yaml stop orchestrator
docker compose -f deploy/compose.yaml exec backup sh /backup.sh restore vrp-20261124T020000Z.dump
docker compose -f deploy/compose.yaml start orchestrator
```

## Stop it

```sh
docker compose -f deploy/compose.yaml down     # keeps the data
docker compose -f deploy/compose.yaml down -v  # deletes the data and secrets
```

Deleting the secrets invalidates every worker's token and CA certificate.

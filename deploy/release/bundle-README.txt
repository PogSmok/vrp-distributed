VRP-Distributed @VERSION@: the control plane
=============================================

This folder runs the control plane in Docker Compose, with Docker 26 or
newer: PostgreSQL, SeaweedFS, the orchestrator, Prometheus,
Grafana, the client, a metrics exporter and the nightly backup. Workers run
natively on their own machines, from this release's vrp-worker archives.

Start it from this folder. Give the name or fixed IP address that workers and
users reach this machine by, and the address to listen on:

    VRP_HOST=server.lab VRP_BIND=192.168.1.10 docker compose up --wait

Without them it listens on this machine only. Then, with the admin tool:

    docker compose run --rm -T admin worker lab-pc-1 --metrics-address lab-pc-1:9101 > worker.json
    docker compose run --rm -T admin ca > ca.pem
    docker compose run --rm -T admin client alice
    docker compose run --rm -v "$PWD/mazowieckie:/graph:ro" admin register-graph /graph

The first two make a worker's configuration, the third a user's token for
the client at https://<VRP_HOST>:8501, and the last registers a road graph
made by vrp-preprocessor (preprocessor/README.md in the repository).

The full guide, from firewall rules to backups and renewing certificates:
@REPOSITORY@/blob/@TAG@/deploy/README.md
Its commands name deploy/compose.yaml, which is compose.yaml here.

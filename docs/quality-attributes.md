# Quality attributes

These are the numbers the architecture decisions refer to. Targets are decided
values. "(assumption)" marks facts about hardware we do not know yet;
"(hypothesis)" and "(to be measured)" mark outcomes the benchmarks will show.

## Constraints

| Constraint          | Value                                                                                                        | Referenced by              |
|---------------------|--------------------------------------------------------------------------------------------------------------|----------------------------|
| Network             | All workers on the same network as the server (reachable by Prometheus, discovered through the orchestrator) | ADR 0002, 0010             |
| Operating systems   | Mix of Windows and Linux machines                                                                            | ADR 0007, 0008, 0019       |
| GPUs                | NVIDIA GPU of compute capability 6.1 (Pascal) or newer; the oldest and weakest is a GTX 1060 6 GB            | ADR 0007, 0011, 0017, 0019 |
| Smallest GPU memory | 6 GB (GTX 1060); must hold graph + matrix + solver state                                                     | ADR 0011                   |

## Scale

| Attribute          | Target                                                              | How to check                      |
|--------------------|---------------------------------------------------------------------|-----------------------------------|
| Number of workers  | 1 to 8 GPU machines                                                 | Scaling benchmark                 |
| Customers per job  | up to 5,000 (matrix about 100 MB)                                   | Largest generated problem         |
| Road graph size    | One region; compact graph fits on the smallest GPU (to be measured) | Preprocess the region and measure |
| Solve phase length | 1 to 10 minutes                                                     | Benchmark runs                    |
| Concurrent jobs    | 1 running, others queued                                            | Scheduler test                    |

## Performance

| Attribute               | Target                                                                                         | How to check                                         |
|-------------------------|------------------------------------------------------------------------------------------------|------------------------------------------------------|
| Distance phase length   | Under 5 minutes for 5,000 customers on 1 GPU                                                   | Benchmark runs                                       |
| Distance phase scaling  | Near-linear speedup from 1 to N GPUs (hypothesis), warm graph caches                           | Strong-scaling benchmark                             |
| Graph download and load | Measured separately (cold vs warm cache), not part of scaling numbers                          | Benchmark runs                                       |
| Finalize phase length   | Under 1 minute for 5,000 customers on the slowest worker (to be measured)                      | Benchmark runs (ADR 0025)                            |
| Solve phase scaling     | Faster time-to-target than 1 GPU running the same number of islands; target = 1-GPU final cost | Time-to-target benchmark, median over ≥ 5 seeds      |
| Migration overhead      | Negligible compared to iteration time                                                          | Metrics: migration latency                           |
| Solver quality          | Gap to best known CVRPLIB solutions reported; target set after first runs                      | Validation harness (ADR 0006), median over ≥ 5 seeds |

## Availability and recovery

| Attribute            | Target                                                                                     | How to check            |
|----------------------|--------------------------------------------------------------------------------------------|-------------------------|
| Orchestrator restart | No accepted job is lost; recovery within about 1 minute                                    | Fault-injection test    |
| Worker failure       | Job still finishes; lost task reassigned within the lease timeout                          | Fault-injection test    |
| Result availability  | Results retrievable after client disconnect, for 30 days                                   | Reconnect test          |
| Server disk loss     | Job state restored from the nightly backup; results and matrices lost (accepted, ADR 0019) | Restore test (ADR 0019) |
| Planned downtime     | Acceptable (single orchestrator, single database)                                          | n/a                     |

## Security

| Attribute           | Target                                                                        | How to check                  |
|---------------------|-------------------------------------------------------------------------------|-------------------------------|
| Transport           | TLS for all control-plane connections                                         | Configuration review          |
| Access              | Named token with role per client, worker and admin; revocable without restart | Negative tests (see ADR 0009) |
| Job ownership       | A client sees, cancels and fetches only the jobs it submitted                 | Negative tests (see ADR 0009) |
| Storage credentials | Held only by the orchestrator and the admin tool (ADR 0031)                   | Configuration review          |

## Targets versus measured

| Attribute              | Target                                       | Measured so far                                                                                        |
|------------------------|----------------------------------------------|--------------------------------------------------------------------------------------------------------|
| Road graph size        | Fits the smallest GPU                        | Mazowieckie (303 MB extract): 629,318 nodes, 1,343,118 edges, `graph.bin` 23.7 MB; GTX 1060 not tried  |
| Graph load             | Measured apart from scaling                  | `graph.bin` of Mazowieckie parsed in 2.2 s (Debug build); download —                                   |
| Distance phase length  | Under 5 minutes for 5,000 customers on 1 GPU | 500 customers: 4.5 s (Debug build). 5,000 customers, on a synthetic grid only: about 34 minutes        |
| Distance phase scaling | Near-linear from 1 to N GPUs (hypothesis)    | —                                                                                                      |
| Finalize phase length  | Under 1 minute for 5,000 customers           | 500 customers (529 legs): 24 to 26 s (Debug build)                                                     |
| Solve phase scaling    | Faster time-to-target than 1 GPU             | —                                                                                                      |
| Migration overhead     | Negligible against iteration time            | — (the worker exports `vrp_worker_migration_seconds`)                                                  |
| Solver quality         | Gap to CVRPLIB best known solutions          | No CVRPLIB gaps yet. 400 customers, 5 islands, 40 s: 817.4 and 821.0 km in two runs, 22 of 24 vehicles |
| Orchestrator restart   | No accepted job lost                         | Scheduler tests; checked by hand during SOLVE ([failure modes](failure-modes.md))                      |
| Worker failure         | Job still finishes                           | Scheduler tests, including the liveness property test; checked by hand                                 |
| Result availability    | Retrievable for 30 days                      | Expiry tested against SeaweedFS; a client reconnecting after 30 days —                                 |
| Server disk loss       | State restored from the nightly backup       | CI restores a backup into a scratch database                                                           |

Importing the 303 MB Mazowieckie extract, which has no target, took 82 s and
148 s in two runs on the development machine, with byte-identical output.

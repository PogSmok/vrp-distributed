# Quality attributes

These are the numbers the architecture decisions refer to. Targets are decided
values. "(assumption)" marks facts about hardware we do not know yet;
"(hypothesis)" and "(to be measured)" mark outcomes the benchmarks will show.

## Constraints

| Constraint          | Value                                                                                                        | Referenced by                |
|---------------------|--------------------------------------------------------------------------------------------------------------|------------------------------|
| Network             | All workers on the same network as the server (reachable by Prometheus, discovered through the orchestrator) | ADR 0002, 0010               |
| Operating systems   | Mix of Windows and Linux machines                                                                            | ADR 0007, 0008, 0019         |
| GPUs                | NVIDIA GPU of compute capability 6.1 (Pascal) or newer; the oldest and weakest is a GTX 1060 6 GB            | ADR 0007, 0011, 0017, 0019   |
| Smallest GPU memory | 6 GB (GTX 1060); must hold graph + matrix + solver state                                                     | ADR 0011                     |

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

| Attribute            | Target                                                                                     | How to check                  |
|----------------------|--------------------------------------------------------------------------------------------|-------------------------------|
| Orchestrator restart | No accepted job is lost; recovery within about 1 minute                                    | Fault-injection test          |
| Worker failure       | Job still finishes; lost task reassigned within the lease timeout                          | Fault-injection test          |
| Result availability  | Results retrievable after client disconnect, for 30 days                                   | Reconnect test                |
| Server disk loss     | Job state restored from the nightly backup; results and matrices lost (accepted, ADR 0019) | Restore test (ADR 0019)       |
| Planned downtime     | Acceptable (single orchestrator, single database)                                          | n/a                           |

## Security

| Attribute           | Target                                                                         | How to check                  |
|---------------------|--------------------------------------------------------------------------------|-------------------------------|
| Transport           | TLS for all control-plane connections                                          | Configuration review          |
| Access              | Named token with role per client, worker and admin; revocable without restart | Negative tests (see ADR 0009) |
| Job ownership       | A client sees, cancels and fetches only the jobs it submitted                  | Negative tests (see ADR 0009) |
| Storage credentials | Held only by orchestrator and preprocessor                                     | Configuration review          |

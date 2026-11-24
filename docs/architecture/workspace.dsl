workspace "vrp-distributed" "Distributed GPU solver for the Vehicle Routing Problem" {

    !identifiers hierarchical

    model {
        user  = person "Dispatcher" "Submits problems and reviews routes."
        admin = person "System Administrator" "Runs the system and graph imports."

        vrp = softwareSystem "VRP-Distributed" "Distributed GPU solver for the Vehicle Routing Problem." {
            app          = container "Client Application" "Submits problems, shows routes and convergence." "Python / Streamlit"
            orchestrator = container "Orchestrator" "Schedules jobs on workers; owns job state and the migrant pool." "Python / grpcio (asyncio)" {
                clientApi   = component "Client API" "Submits and cancels jobs, answers progress polls, returns result URLs; only for the caller's own jobs; limits poll rate." "gRPC service"
                workerApi   = component "Worker API" "One bidirectional session stream per worker; a new session replaces the old one." "gRPC service"
                adminApi    = component "Admin API" "Registers graph versions." "gRPC service"
                auth        = component "Auth" "Wraps all three APIs; checks named tokens and roles from the token file; identifies the caller." "gRPC interceptor"
                scheduler   = component "Scheduler" "Job phases, leases, island groups. Decides every database write; applies it after commit." "asyncio event loop"
                registry    = component "Worker Registry" "Connected workers, their send queues and metrics addresses; serves worker discovery to Prometheus." "In memory, HTTP SD endpoint"
                migrantPool = component "Migrant Pool" "Migrants of registered islands per job; bypasses the Scheduler's queue." "In memory"
                catalogue   = component "Graph Catalogue" "Registered graph versions." "In memory, owned by Scheduler"
                artifacts   = component "Artifact Manager" "Signs presigned URLs, checks uploads, verifies graphs, deletes expired objects." "S3 client"
                repository  = component "State Repository" "All SQL; commits transitions in order, one transaction each; applies schema migrations at start-up." "psycopg 3, commits outside the loop"
            }
            worker       = container "Worker" "Computes distances and runs the solver on one GPU, or on the CPU if built without CUDA." "C++ / CUDA" {
                session       = component "Session Client" "Registration, stream, heartbeats, reconnects, fresh URLs; resends unconfirmed reports." "gRPC client, session thread"
                runner        = component "Task Runner" "Runs one task at a time in short steps; handles cancel." "Compute thread"
                graphCache    = component "Graph Cache" "Verified graphs on disk; one graph resident on the GPU or in memory."
                transfer      = component "Object Transfer" "Presigned GET and PUT with retries." "libcurl"
                snapper       = component "Snapper" "Snaps coordinates to graph nodes." "vrp-core / C++"
                distance      = component "Distance Engine" "Many-source shortest paths." "vrp-core / CUDA or C++"
                solver        = component "Island Solver" "Populations, local search, migrant import and export." "vrp-core / CUDA or C++"
                resultBuilder = component "Result Builder" "Routes legs on the CPU, checks the cost, builds polylines and the result artifact." "vrp-core / C++"
                planRouter    = component "Plan Router" "Routes the latest best plan on the roads for the live map." "Low-priority thread"
            }
            preprocessor = container "Graph Preprocessor" "Turns OSM extracts into versioned graphs." "Python CLI / pyrosm"
            adminTool    = container "Admin Tool" "Creates secrets, issues worker configurations and client tokens, uploads and registers graphs." "Python CLI"
            objectStore  = container "Object Store" "Graphs, distance matrices and results." "SeaweedFS (S3)" {
                tags "Database"
            }
            stateDb      = container "Job State DB" "Jobs, tasks, assignments, graph catalogue." "PostgreSQL" {
                tags "Database"
            }
        }

        osm = softwareSystem "OpenStreetMap" "Road network source (offline import)." {
            tags "External"
        }

        prometheus = softwareSystem "Prometheus" "Collects and stores metrics." {
            tags "External"
        }

        grafana = softwareSystem "Grafana" "Shows dashboards." {
            tags "External"
        }

        // --- CONTEXT relations ---
        // Relations without text are self-explanatory from the element descriptions.
        user -> vrp
        vrp -> osm
        prometheus -> vrp
        grafana -> prometheus
        admin -> grafana

        // --- CONTAINER relations ---
        user -> vrp.app "Signs in with a token" "HTTPS"
        vrp.app -> vrp.orchestrator "Submits jobs, polls progress" "gRPC/TLS"
        vrp.app -> vrp.objectStore "Downloads results" "HTTPS, presigned"
        vrp.orchestrator -> vrp.stateDb
        vrp.orchestrator -> vrp.objectStore "Manages artifacts" "S3 API"
        vrp.worker -> vrp.orchestrator "Gets tasks, reports progress, sends logs" "gRPC/TLS"
        vrp.worker -> vrp.objectStore "Reads graphs, writes results" "HTTPS, presigned"
        vrp.preprocessor -> osm
        vrp.adminTool -> vrp.objectStore "Uploads graphs" "S3 API"
        vrp.adminTool -> vrp.orchestrator "Registers graph versions" "gRPC/TLS"
        prometheus -> vrp.orchestrator "Scrapes metrics, discovers workers" "HTTP"
        prometheus -> vrp.stateDb
        prometheus -> vrp.objectStore
        prometheus -> vrp.worker
        admin -> vrp.preprocessor
        admin -> vrp.adminTool

        // --- COMPONENT relations: Orchestrator ---
        vrp.app -> vrp.orchestrator.clientApi "Submits, cancels and polls jobs" "gRPC/TLS"
        vrp.adminTool -> vrp.orchestrator.adminApi "Registers graph versions" "gRPC/TLS"
        prometheus -> vrp.orchestrator.registry "Discovers worker metrics endpoints" "HTTP"
        vrp.orchestrator.clientApi -> vrp.orchestrator.auth "Checks token and role"
        vrp.orchestrator.workerApi -> vrp.orchestrator.auth "Checks token; gets the worker name for the session"
        vrp.orchestrator.adminApi -> vrp.orchestrator.auth "Checks admin token"
        vrp.orchestrator.clientApi -> vrp.orchestrator.scheduler "Posts commands and progress queries"
        vrp.orchestrator.workerApi -> vrp.orchestrator.scheduler "Posts worker events"
        vrp.orchestrator.workerApi -> vrp.orchestrator.registry "Opens and closes sessions" {
            tags "LabelNearStart"
        }
        vrp.orchestrator.workerApi -> vrp.orchestrator.migrantPool "Pushes and pulls migrants"
        vrp.orchestrator.adminApi -> vrp.orchestrator.artifacts "Verifies uploaded graphs"
        vrp.orchestrator.adminApi -> vrp.orchestrator.scheduler "Posts graph registrations"
        vrp.orchestrator.scheduler -> vrp.orchestrator.registry "Queues assignments"
        vrp.orchestrator.scheduler -> vrp.orchestrator.migrantPool "Manages pools and islands"
        vrp.orchestrator.scheduler -> vrp.orchestrator.catalogue
        vrp.orchestrator.scheduler -> vrp.orchestrator.artifacts "Signs URLs, requests upload checks and deletions"
        vrp.orchestrator.artifacts -> vrp.orchestrator.scheduler "Posts check and deletion results"
        vrp.orchestrator.scheduler -> vrp.orchestrator.repository "Hands over transitions"
        vrp.orchestrator.repository -> vrp.orchestrator.scheduler "Posts commit results"
        vrp.orchestrator.repository -> vrp.stateDb "" "SQL"
        vrp.orchestrator.artifacts -> vrp.objectStore "" "S3 API"

        // --- COMPONENT relations: Worker ---
        vrp.worker.session -> vrp.orchestrator.workerApi "Session stream" "gRPC/TLS"
        vrp.worker.session -> vrp.worker.runner "Hands over assignments and cancels"
        vrp.worker.runner -> vrp.worker.session "Reports progress, migrants, results, log lines"
        vrp.worker.runner -> vrp.worker.graphCache "Ensures graph is loaded"
        vrp.worker.runner -> vrp.worker.transfer "Matrix rows and results"
        vrp.worker.graphCache -> vrp.worker.transfer "Downloads graphs"
        vrp.worker.transfer -> vrp.objectStore "" "HTTPS, presigned"
        vrp.worker.runner -> vrp.worker.snapper "PREPARE"
        vrp.worker.runner -> vrp.worker.distance "DISTANCE"
        vrp.worker.runner -> vrp.worker.solver "SOLVE"
        vrp.worker.runner -> vrp.worker.resultBuilder "FINALIZE"
        vrp.worker.runner -> vrp.worker.planRouter "Offers each new best plan"
        vrp.worker.planRouter -> vrp.worker.session "Sends road plans"

        // --- DEPLOYMENT ---
        deploymentEnvironment "Lab" {
            deploymentNode "Server PC" "Control plane, storage, monitoring." "Linux or Windows" {
                deploymentNode "Docker Compose" "The control plane from one Compose file; data in volumes on the server's disk." "Docker" {
                    containerInstance vrp.orchestrator
                    containerInstance vrp.app
                    containerInstance vrp.adminTool
                    containerInstance vrp.stateDb
                    containerInstance vrp.objectStore
                    softwareSystemInstance prometheus
                    softwareSystemInstance grafana
                }
            }
            deploymentNode "Worker PC" "One NVIDIA GPU, or only a CPU; same LAN as the server. The worker runs as a service." "Windows or Linux" "" "1..8" {
                containerInstance vrp.worker
            }
            deploymentNode "Admin Workstation" "Graph imports; keeps the nightly database backups." "Windows or Linux" {
                containerInstance vrp.preprocessor
            }
        }
    }

    views {
        systemContext vrp "Context" {
            include *
            include grafana admin
            autoLayout lr
        }

        container vrp "Containers" {
            include *
            autoLayout lr
        }

        component vrp.orchestrator "OrchestratorComponents" {
            include *
        }

        component vrp.worker "WorkerComponents" {
            include *
        }

        deployment * "Lab" "Deployment" {
            include *
        }

        styles {
            element "External" {
                background #999999
                color #ffffff
            }
            element "Person" {
                shape person
            }
            element "Database" {
                shape cylinder
            }
            relationship "LabelNearStart" {
                position 25
            }
        }
    }
}

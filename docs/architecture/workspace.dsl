workspace "vrp-distributed" "Distributed GPU solver for the Vehicle Routing Problem" {

    !identifiers hierarchical

    model {
        user  = person "Dispatcher" "Submits problems and reviews routes."
        admin = person "System Administrator" "Runs the system and graph imports."

        vrp = softwareSystem "VRP-Distributed" "Distributed GPU solver for the Vehicle Routing Problem." {
            app          = container "Client Application" "Submits problems, shows routes and convergence." "Python / Streamlit"
            orchestrator = container "Orchestrator" "Schedules jobs on workers; owns job state and the migrant pool." "C++ / gRPC"
            worker       = container "Worker" "Computes distances and runs the solver on one GPU." "C++ / CUDA"
            preprocessor = container "Graph Preprocessor" "Turns OSM extracts into versioned graphs." "Python CLI / pyrosm"
            objectStore  = container "Object Store" "Graphs, distance matrices and results." "MinIO (S3)" {
                tags "Database"
            }
            stateDb      = container "Job State DB" "Jobs, tasks, leases and graph catalogue." "PostgreSQL" {
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
        user -> vrp.app
        vrp.app -> vrp.orchestrator "Submits jobs, gets progress" "gRPC/TLS"
        vrp.app -> vrp.objectStore "Downloads results" "HTTPS, presigned"
        vrp.orchestrator -> vrp.stateDb
        vrp.orchestrator -> vrp.objectStore "Manages artifacts" "S3 API"
        vrp.worker -> vrp.orchestrator "Gets tasks, reports progress" "gRPC/TLS"
        vrp.worker -> vrp.objectStore "Reads graphs, writes results" "HTTPS, presigned"
        vrp.preprocessor -> vrp.objectStore
        vrp.preprocessor -> vrp.orchestrator "Registers graph versions" "gRPC/TLS"
        vrp.preprocessor -> osm
        prometheus -> vrp.orchestrator
        prometheus -> vrp.stateDb
        prometheus -> vrp.objectStore
        prometheus -> vrp.worker
        admin -> vrp.preprocessor

        // --- DEPLOYMENT ---
        deploymentEnvironment "Lab" {
            deploymentNode "Server PC" "Control plane, storage, monitoring." "Linux or Windows" {
                containerInstance vrp.orchestrator
                containerInstance vrp.stateDb
                containerInstance vrp.objectStore
                softwareSystemInstance prometheus
                softwareSystemInstance grafana
            }
            deploymentNode "GPU Worker PC" "One NVIDIA GPU, same LAN as the server." "Windows or Linux" "" "1..8" {
                containerInstance vrp.worker
            }
            deploymentNode "Dispatcher PC" "Reaches the orchestrator and object store." "Windows or Linux" {
                containerInstance vrp.app
            }
            deploymentNode "Admin Workstation" "" "Windows or Linux" {
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
        }
    }
}

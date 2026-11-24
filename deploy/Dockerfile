# The orchestrator's and client's images, from the lock file (ADR 0031).
# Build from the repository root.
FROM python:3.14.10-slim AS base
ENV UV_PYTHON_DOWNLOADS=never UV_LINK_MODE=copy PATH=/app/.venv/bin:$PATH
RUN pip install --no-cache-dir uv==0.13.7 \
    && useradd --uid 10001 --create-home vrp
WORKDIR /app
COPY pyproject.toml uv.lock ./
COPY proto/ proto/
COPY client/pyproject.toml client/hatch_build.py client/README.md client/
COPY orchestrator/pyproject.toml orchestrator/hatch_build.py orchestrator/README.md orchestrator/
COPY preprocessor/pyproject.toml preprocessor/README.md preprocessor/

FROM base AS orchestrator
RUN uv sync --locked --no-dev --group deploy --package vrp-orchestrator --no-install-workspace
COPY orchestrator/src/ orchestrator/src/
COPY deploy/admin.py deploy/
RUN uv sync --locked --no-dev --group deploy --package vrp-orchestrator \
    && install -d -o vrp /var/log/vrp
USER vrp
CMD ["vrp-orchestrator", "--config", "/run/vrp/orchestrator.toml"]

FROM base AS client
RUN uv sync --locked --no-dev --package vrp-client --no-install-workspace
COPY client/src/ client/src/
COPY client/.streamlit/ client/.streamlit/
RUN uv sync --locked --no-dev --package vrp-client
USER vrp
WORKDIR /app/client
EXPOSE 8501
CMD ["streamlit", "run", "src/vrp_client/app.py", "--server.address=0.0.0.0", "--server.headless=true", "--server.fileWatcherType=none"]

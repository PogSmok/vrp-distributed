"""The orchestrator's configuration file (ADR 0019).

A TOML file; relative paths are relative to its directory:

    listen = "0.0.0.0:50051"
    metrics = "0.0.0.0:9100"
    database = "postgresql://orchestrator@localhost:5432/vrp"
    tokens = "tokens.txt"
    log_dir = "logs"
    certificate = "server.pem"
    key = "server.key"

    [storage]
    endpoint = "http://localhost:8333"
    public_endpoint = "https://server.lab:8334"
    bucket = "vrp"

Secrets can stay out of the file, read by the libraries themselves: the
database password from `PGPASSWORD`, and, without `access_key` and
`secret_key`, the store's keys from `AWS_ACCESS_KEY_ID` and
`AWS_SECRET_ACCESS_KEY`.

Without TLS (`insecure = true`, and no certificate) the orchestrator runs
only for local development, and says so at start-up (ADR 0009).
"""

import tomllib
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Storage:
    """The object store (ADR 0029).

    `endpoint` is where the orchestrator reaches it; `public_endpoint` is
    where workers and clients do, and so what presigned URLs are signed for
    (ADR 0009).
    """

    endpoint: str
    public_endpoint: str
    bucket: str
    access_key: str | None = None
    secret_key: str | None = None
    region: str = "us-east-1"


@dataclass(frozen=True)
class Config:
    """What the orchestrator listens on and connects to."""

    listen: str
    metrics: str
    database: str
    tokens: Path
    log_dir: Path
    storage: Storage
    certificate: Path | None = None
    key: Path | None = None
    insecure: bool = False


def load_config(path: Path) -> Config:
    """Reads and checks a configuration file.

    Args:
        path: The TOML file.

    Returns:
        The configuration.

    Raises:
        OSError: If the file cannot be read.
        ValueError: If a setting is missing, of the wrong type, or TLS is
            neither configured nor explicitly turned off.
    """
    with path.open("rb") as stream:
        values = tomllib.load(stream)
    base = path.parent
    try:
        storage = Storage(**values.pop("storage"))
        for name in ("tokens", "log_dir", "certificate", "key"):
            if name in values:
                values[name] = base / values[name]
        config = Config(storage=storage, **values)
    except (KeyError, TypeError) as error:
        msg = f"{path}: {error}"
        raise ValueError(msg) from error
    tls = config.certificate is not None and config.key is not None
    if tls == config.insecure:
        msg = (
            f"{path}: set both certificate and key, or insecure = true for "
            "local development only"
        )
        raise ValueError(msg)
    return config

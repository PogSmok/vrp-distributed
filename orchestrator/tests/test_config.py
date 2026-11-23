from pathlib import Path

import pytest

from vrp_orchestrator.config import load_config

_BASE = """
listen = "0.0.0.0:50051"
metrics = "0.0.0.0:9100"
database = "postgresql://localhost/vrp"
tokens = "tokens.txt"
log_dir = "logs"
{tls}
[storage]
endpoint = "http://localhost:8333"
public_endpoint = "https://server.lab:8333"
bucket = "vrp"
access_key = "a"
secret_key = "s"
"""


# Without TLS the orchestrator runs only when told to (ADR 0009).
@pytest.mark.parametrize(
    ("tls", "valid"),
    [
        ('certificate = "server.pem"\nkey = "server.key"', True),
        ("insecure = true", True),
        ("", False),
        ('certificate = "server.pem"', False),
        (
            'certificate = "server.pem"\nkey = "server.key"\ninsecure = true',
            False,
        ),
    ],
)
def test_tls_must_be_configured_or_explicitly_off(
    tmp_path: Path, tls: str, *, valid: bool
) -> None:
    path = tmp_path / "orchestrator.toml"
    path.write_text(_BASE.format(tls=tls), encoding="utf-8")
    if not valid:
        with pytest.raises(ValueError, match="insecure"):
            load_config(path)
        return
    config = load_config(path)
    assert config.tokens == tmp_path / "tokens.txt"

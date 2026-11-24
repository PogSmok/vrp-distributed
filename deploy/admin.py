"""The deployment's secrets, worker configurations and graphs (ADR 0031).

Runs in the orchestrator's image, on the `secrets` volume:

    admin.py init
    admin.py worker <name> [--metrics-address <host:port>]
    admin.py client <name>
    admin.py ca
    admin.py grafana-password
    admin.py register-graph <directory>

`init` runs on every `docker compose up`. It creates only what is missing,
and renews a certificate within 30 days of its end. After a new CA, each
worker needs the new `ca.pem`.
"""

import argparse
import datetime as dt
import ipaddress
import json
import os
import secrets
import sys
import tomllib
from pathlib import Path
from typing import Final

import boto3
import grpc
from botocore.config import Config as BotoConfig
from cryptography import x509
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

from vrp_orchestrator import vrp_pb2
from vrp_orchestrator.auth import hash_token, new_token

_ROOT: Final = Path("/secrets")
_DEFAULT_HOST: Final = "vrp.localhost"
_CERTIFICATE_DAYS: Final = 365  # ADR 0019
_RENEW_DAYS: Final = 30
_REGISTER_SECONDS: Final = 3600.0  # the orchestrator hashes every file
_INTERNAL_NAMES: Final = ("localhost", "orchestrator", "seaweedfs")


def _now() -> dt.datetime:
    return dt.datetime.now(dt.UTC)


def _write_new(path: Path, make: str) -> str:
    """The file's content, writing `make` first if it does not exist."""
    if not path.exists():
        path.write_text(make, encoding="utf-8")
    return path.read_text(encoding="utf-8").strip()


def _pem(key: ec.EllipticCurvePrivateKey) -> bytes:
    return key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )


def _name(common_name: str) -> x509.Name:
    return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name)])


def _key_usage(*, signs_certificates: bool) -> x509.KeyUsage:
    # Python 3.13+ refuses a CA without these extensions.
    return x509.KeyUsage(
        digital_signature=not signs_certificates,
        content_commitment=False,
        key_encipherment=False,
        data_encipherment=False,
        key_agreement=False,
        key_cert_sign=signs_certificates,
        crl_sign=signs_certificates,
        encipher_only=False,
        decipher_only=False,
    )


def _expiring(certificate: x509.Certificate) -> bool:
    renew_at = certificate.not_valid_after_utc - dt.timedelta(days=_RENEW_DAYS)
    return _now() >= renew_at


def _make_ca(directory: Path) -> None:
    path = directory / "ca.pem"
    if path.exists() and not _expiring(
        x509.load_pem_x509_certificate(path.read_bytes())
    ):
        return
    key = ec.generate_private_key(ec.SECP256R1())
    name = _name("VRP lab CA")
    public = key.public_key()
    certificate = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(public)
        .serial_number(x509.random_serial_number())
        .not_valid_before(_now() - dt.timedelta(minutes=5))
        .not_valid_after(_now() + dt.timedelta(days=_CERTIFICATE_DAYS))
        .add_extension(
            x509.BasicConstraints(ca=True, path_length=0), critical=True
        )
        .add_extension(_key_usage(signs_certificates=True), critical=True)
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(public), critical=False
        )
        .sign(key, hashes.SHA256())
    )
    (directory / "ca.key").write_bytes(_pem(key))
    (directory / "ca.pem").write_bytes(
        certificate.public_bytes(serialization.Encoding.PEM)
    )


def _server_is_current(path: Path, ca: Path, host: str) -> bool:
    if not path.exists():
        return False
    certificate = x509.load_pem_x509_certificate(path.read_bytes())
    authority = x509.load_pem_x509_certificate((ca / "ca.pem").read_bytes())
    try:
        certificate.verify_directly_issued_by(authority)
    except (InvalidSignature, TypeError, ValueError):
        return False
    names = certificate.extensions.get_extension_for_class(
        x509.SubjectAlternativeName
    ).value
    return _host_name(host) in names and not _expiring(certificate)


def _host_name(host: str) -> x509.GeneralName:
    """`host` as TLS checks it: an IP address, or else a DNS name."""
    try:
        return x509.IPAddress(ipaddress.ip_address(host))
    except ValueError:
        return x509.DNSName(host)


def _make_server_certificate(ca: Path, host: str) -> tuple[bytes, bytes]:
    authority = x509.load_pem_x509_certificate((ca / "ca.pem").read_bytes())
    authority_key = serialization.load_pem_private_key(
        (ca / "ca.key").read_bytes(), password=None
    )
    if not isinstance(authority_key, ec.EllipticCurvePrivateKey):
        msg = "the CA key is not an EC key"
        raise TypeError(msg)
    key = ec.generate_private_key(ec.SECP256R1())
    public = key.public_key()
    names: list[x509.GeneralName] = [
        x509.DNSName(name) for name in _INTERNAL_NAMES
    ]
    names.append(x509.IPAddress(ipaddress.ip_address("127.0.0.1")))
    if _host_name(host) not in names:
        names.append(_host_name(host))
    certificate = (
        x509.CertificateBuilder()
        .subject_name(_name(host))
        .issuer_name(authority.subject)
        .public_key(public)
        .serial_number(x509.random_serial_number())
        .not_valid_before(_now() - dt.timedelta(minutes=5))
        .not_valid_after(
            min(
                _now() + dt.timedelta(days=_CERTIFICATE_DAYS),
                authority.not_valid_after_utc,
            )
        )
        .add_extension(
            x509.BasicConstraints(ca=False, path_length=None), critical=True
        )
        .add_extension(_key_usage(signs_certificates=False), critical=True)
        .add_extension(
            x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]),
            critical=False,
        )
        .add_extension(x509.SubjectAlternativeName(names), critical=False)
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(public), critical=False
        )
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_public_key(
                authority_key.public_key()
            ),
            critical=False,
        )
        .sign(authority_key, hashes.SHA256())
    )
    return (
        certificate.public_bytes(serialization.Encoding.PEM),
        _pem(key),
    )


def _add_token(tokens: Path, name: str, role: str) -> str:
    text = tokens.read_text(encoding="utf-8") if tokens.exists() else ""
    if any(line.split()[:1] == [name] for line in text.splitlines()):
        msg = f"a token named {name} already exists"
        raise ValueError(msg)
    token = new_token()
    with tokens.open("a", encoding="utf-8", newline="\n") as stream:
        stream.write(f"{name} {role} {hash_token(token)}\n")
    return token


def _init(_arguments: argparse.Namespace) -> int:
    host = os.environ.get("VRP_HOST", _DEFAULT_HOST)
    folders = {
        name: _ROOT / name
        for name in (
            "ca",
            "postgres",
            "monitor",
            "store",
            "orchestrator",
            "client",
            "grafana",
        )
    }
    for folder in folders.values():
        folder.mkdir(parents=True, exist_ok=True)
    _make_ca(folders["ca"])
    _write_new(folders["grafana"] / "password", secrets.token_urlsafe(18))
    _write_new(folders["monitor"] / "password", secrets.token_urlsafe(32))

    server = folders["orchestrator"] / "server.pem"
    if not _server_is_current(server, folders["ca"], host):
        certificate, key = _make_server_certificate(folders["ca"], host)
        for name in ("orchestrator", "store"):
            (folders[name] / "server.pem").write_bytes(certificate)
            (folders[name] / "server.key").write_bytes(key)
    # The client serves HTTPS with the same certificate (ADR 0038).
    ca = folders["ca"] / "ca.pem"
    for source in (server, server.with_suffix(".key"), ca):
        (folders["client"] / source.name).write_bytes(source.read_bytes())

    database = _write_new(
        folders["postgres"] / "password", secrets.token_urlsafe(32)
    )
    access = _write_new(folders["ca"] / "s3-access", secrets.token_hex(16))
    secret = _write_new(folders["ca"] / "s3-secret", secrets.token_urlsafe(32))
    identity = {
        "name": "orchestrator",
        "credentials": [{"accessKey": access, "secretKey": secret}],
        "actions": ["Admin", "Read", "Write", "List", "Tagging"],
    }
    (folders["store"] / "s3.json").write_text(
        json.dumps({"identities": [identity]}), encoding="utf-8"
    )

    tokens = folders["orchestrator"] / "tokens.txt"
    if not (folders["ca"] / "admin-token").exists():
        token = _add_token(tokens, "admin", "admin")
        (folders["ca"] / "admin-token").write_text(token, encoding="utf-8")
    (folders["ca"] / "host").write_text(host, encoding="utf-8")
    (folders["orchestrator"] / "orchestrator.toml").write_text(
        f"""listen = "0.0.0.0:50051"
metrics = "0.0.0.0:9100"
database = "postgresql://vrp:{database}@postgres:5432/vrp"
tokens = "tokens.txt"
log_dir = "/var/log/vrp"
certificate = "server.pem"
key = "server.key"

[storage]
endpoint = "http://seaweedfs:8333"
public_endpoint = "https://{host}:8334"
bucket = "vrp"
access_key = "{access}"
secret_key = "{secret}"
""",
        encoding="utf-8",
    )
    return 0


def _worker(arguments: argparse.Namespace) -> int:
    host = (_ROOT / "ca" / "host").read_text(encoding="utf-8")
    token = _add_token(
        _ROOT / "orchestrator" / "tokens.txt", arguments.name, "worker"
    )
    address = "localhost" if host == _DEFAULT_HOST else host
    config = {
        "orchestrator": f"{address}:50051",
        "token": token,
        "ca_certificate": "ca.pem",
        "metrics_listen": "0.0.0.0:9101",
        "metrics_address": arguments.metrics_address
        or f"{arguments.name}:9101",
        "cache_dir": "cache",
        "log_dir": "logs",
    }
    sys.stdout.write(json.dumps(config, indent=2) + "\n")
    return 0


def _client(arguments: argparse.Namespace) -> int:
    token = _add_token(
        _ROOT / "orchestrator" / "tokens.txt", arguments.name, "client"
    )
    sys.stdout.write(token + "\n")
    return 0


def _ca(_arguments: argparse.Namespace) -> int:
    sys.stdout.write((_ROOT / "ca" / "ca.pem").read_text(encoding="utf-8"))
    return 0


def _grafana_password(_arguments: argparse.Namespace) -> int:
    password = (_ROOT / "grafana" / "password").read_text(encoding="utf-8")
    sys.stdout.write(password.strip() + "\n")
    return 0


def _register_graph(arguments: argparse.Namespace) -> int:
    directory: Path = arguments.directory
    manifest = json.loads((directory / "manifest.json").read_text("utf-8"))
    version = manifest["graph_version"]
    with (_ROOT / "orchestrator" / "orchestrator.toml").open("rb") as stream:
        storage = tomllib.load(stream)["storage"]
    store = boto3.client(
        "s3",
        endpoint_url=storage["endpoint"],
        aws_access_key_id=storage["access_key"],
        aws_secret_access_key=storage["secret_key"],
        region_name="us-east-1",
        config=BotoConfig(
            signature_version="s3v4", s3={"addressing_style": "path"}
        ),
    )
    # The manifest goes last: without it, a partial upload is no version.
    files = sorted(directory.iterdir(), key=lambda f: f.name == "manifest.json")
    for file in files:
        store.upload_file(
            str(file), storage["bucket"], f"graphs/{version}/{file.name}"
        )
    credentials = grpc.ssl_channel_credentials(
        (_ROOT / "ca" / "ca.pem").read_bytes()
    )
    token = (_ROOT / "ca" / "admin-token").read_text(encoding="utf-8")
    with grpc.secure_channel("orchestrator:50051", credentials) as channel:
        register = channel.unary_unary(
            "/vrp.v1.AdminService/RegisterGraph",
            request_serializer=vrp_pb2.RegisterGraphRequest.SerializeToString,
            response_deserializer=vrp_pb2.RegisterGraphResponse.FromString,
        )
        register(
            vrp_pb2.RegisterGraphRequest(graph_version=version),
            metadata=(("authorization", f"Bearer {token}"),),
            timeout=_REGISTER_SECONDS,
        )
    sys.stdout.write(f"registered {version}\n")
    return 0


def main() -> int:
    """Runs one command.

    Returns:
        The process exit code.
    """
    parser = argparse.ArgumentParser(allow_abbrev=False)
    commands = parser.add_subparsers(required=True)
    commands.add_parser("init").set_defaults(run=_init)
    commands.add_parser("ca").set_defaults(run=_ca)
    commands.add_parser("grafana-password").set_defaults(run=_grafana_password)
    worker = commands.add_parser("worker")
    worker.add_argument("name")
    worker.add_argument("--metrics-address", default="")
    worker.set_defaults(run=_worker)
    client = commands.add_parser("client")
    client.add_argument("name")
    client.set_defaults(run=_client)
    graph = commands.add_parser("register-graph")
    graph.add_argument("directory", type=Path)
    graph.set_defaults(run=_register_graph)
    arguments = parser.parse_args()
    try:
        status: int = arguments.run(arguments)
    except (OSError, ValueError, KeyError, grpc.RpcError) as error:
        sys.stderr.write(f"error: {error}\n")
        return 1
    return status


if __name__ == "__main__":
    sys.exit(main())

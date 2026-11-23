import asyncio
import datetime as dt
import os
from collections.abc import AsyncIterator
from pathlib import Path

import grpc
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

from vrp_orchestrator.auth import (
    CALLER,
    AuthInterceptor,
    Role,
    TokenFile,
    hash_token,
    new_token,
    server_credentials,
)


def _certificate(directory: Path) -> tuple[Path, Path]:
    """A self-signed certificate for localhost, as the lab CA would sign."""
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    now = dt.datetime.now(dt.UTC)
    certificate = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - dt.timedelta(minutes=1))
        .not_valid_after(now + dt.timedelta(days=1))
        .add_extension(
            x509.SubjectAlternativeName([x509.DNSName("localhost")]),
            critical=False,
        )
        .add_extension(
            x509.BasicConstraints(ca=True, path_length=None), critical=True
        )
        .sign(key, hashes.SHA256())
    )
    certificate_path = directory / "server.pem"
    key_path = directory / "server.key"
    certificate_path.write_bytes(
        certificate.public_bytes(serialization.Encoding.PEM)
    )
    key_path.write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    return certificate_path, key_path


async def _who(_request: bytes, _context: object) -> bytes:
    return CALLER.get().name.encode()


async def _stream(
    requests: AsyncIterator[bytes], _context: object
) -> AsyncIterator[bytes]:
    async for _ in requests:
        yield CALLER.get().name.encode()


async def _scenario(directory: Path) -> None:
    alice, worker = new_token(), new_token()
    tokens = directory / "tokens"
    tokens.write_text(
        f"# name role sha256\nalice client {hash_token(alice)}\n"
        f"w1 worker {hash_token(worker)}\n",
        encoding="utf-8",
    )
    certificate, key = _certificate(directory)
    roles = {
        "/test.Echo/Who": frozenset({Role.CLIENT}),
        "/test.Echo/Stream": frozenset({Role.WORKER}),
    }
    server = grpc.aio.server(
        interceptors=[AuthInterceptor(TokenFile(tokens), roles)]
    )
    server.add_generic_rpc_handlers(
        (
            grpc.method_handlers_generic_handler(
                "test.Echo",
                {
                    "Who": grpc.unary_unary_rpc_method_handler(_who),
                    "Stream": grpc.stream_stream_rpc_method_handler(_stream),
                },
            ),
        )
    )
    port = server.add_secure_port(
        "localhost:0", server_credentials(certificate, key)
    )
    await server.start()
    try:
        credentials = grpc.ssl_channel_credentials(certificate.read_bytes())
        async with grpc.aio.secure_channel(
            f"localhost:{port}", credentials
        ) as channel:
            who: grpc.aio.UnaryUnaryMultiCallable[bytes, bytes] = (
                channel.unary_unary("/test.Echo/Who")
            )
            stream: grpc.aio.StreamStreamMultiCallable[bytes, bytes] = (
                channel.stream_stream("/test.Echo/Stream")
            )

            async def call(token: str | None) -> bytes | grpc.StatusCode:
                metadata = (
                    []
                    if token is None
                    else [("authorization", f"Bearer {token}")]
                )
                try:
                    reply: bytes = await who(b"", metadata=metadata)
                except grpc.aio.AioRpcError as error:
                    return error.code()
                return reply

            # ADR 0009's negative tests: no token, a wrong role, a revoked
            # token.
            assert await call(None) is grpc.StatusCode.UNAUTHENTICATED
            assert await call("guess") is grpc.StatusCode.UNAUTHENTICATED
            assert await call(worker) is grpc.StatusCode.PERMISSION_DENIED
            assert await call(alice) == b"alice"
            replies = stream(
                iter([b"", b""]),
                metadata=[("authorization", f"Bearer {worker}")],
            )
            assert [reply async for reply in replies] == [b"w1", b"w1"]

            # Deleting a line revokes its token without a restart.
            tokens.write_text(
                f"w1 worker {hash_token(worker)}\n", encoding="utf-8"
            )
            stat = tokens.stat()
            os.utime(tokens, ns=(stat.st_atime_ns, stat.st_mtime_ns + 10**9))
            assert await call(alice) is grpc.StatusCode.UNAUTHENTICATED
    finally:
        await server.stop(None)


def test_calls_need_a_valid_token_of_an_allowed_role(tmp_path: Path) -> None:
    asyncio.run(_scenario(tmp_path))

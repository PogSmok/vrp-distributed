"""Named tokens with roles, checked on every call (ADR 0009).

The token file has one line per token, `<name> <role> <sha256>`, where the
hash is of the token itself; `#` starts a comment. It is read again when its
modification time changes, so deleting a line revokes a token without a
restart. Tokens are long random strings, so a plain SHA-256 is enough.
"""

# grpc's handler types are generic only in their stubs: Python 3.13 would
# fail evaluating `RpcMethodHandler[...]` in a signature at import time.
from __future__ import annotations

import contextvars
import enum
import hashlib
import inspect
import secrets
from collections.abc import AsyncIterator, Awaitable, Callable, Mapping
from dataclasses import dataclass
from typing import TYPE_CHECKING, Final, cast, override

import grpc

if TYPE_CHECKING:
    from pathlib import Path

_HEADER: Final = "authorization"
_SCHEME: Final = "Bearer "
_FIELDS: Final = 3


class Role(enum.StrEnum):
    """What a token may do."""

    CLIENT = "client"
    WORKER = "worker"
    ADMIN = "admin"


@dataclass(frozen=True)
class Identity:
    """Who is calling: a token's name and role."""

    name: str
    role: Role


CALLER: Final = contextvars.ContextVar[Identity]("caller")
"""The caller of the call being handled, set by `AuthInterceptor`."""


def new_token() -> str:
    """A new random token, to give to one client, worker or administrator.

    Returns:
        The token.
    """
    return secrets.token_urlsafe(32)


def hash_token(token: str) -> str:
    """The token file's hash of a token.

    Args:
        token: The token.

    Returns:
        Its SHA-256, in lower-case hex.
    """
    return hashlib.sha256(token.encode()).hexdigest()


class TokenFile:
    """The named tokens of a file, re-read when it changes."""

    def __init__(self, path: Path) -> None:
        """Reads the file.

        Args:
            path: The token file.

        Raises:
            OSError: If the file cannot be read.
            ValueError: If a line is not `<name> <role> <sha256>`.
        """
        self._path = path
        self._mtime_ns = -1
        self._tokens: dict[str, Identity] = {}
        self._reload()

    def identify(self, token: str) -> Identity | None:
        """The identity of `token`, from the file as it is now.

        Args:
            token: The token a caller presented.

        Returns:
            Its identity, or None if no line holds it.

        Raises:
            OSError: If the changed file cannot be read.
            ValueError: If the changed file has an invalid line.
        """
        self._reload()
        return self._tokens.get(hash_token(token))

    def _reload(self) -> None:
        mtime_ns = self._path.stat().st_mtime_ns
        if mtime_ns == self._mtime_ns:
            return
        tokens: dict[str, Identity] = {}
        text = self._path.read_text(encoding="utf-8")
        for number, line in enumerate(text.splitlines(), start=1):
            fields = line.split("#", 1)[0].split()
            if not fields:
                continue
            if len(fields) != _FIELDS or fields[1] not in Role:
                msg = f"{self._path}:{number}: expected <name> <role> <sha256>"
                raise ValueError(msg)
            name, role, digest = fields
            tokens[digest] = Identity(name, Role(role))
        self._tokens = tokens
        self._mtime_ns = mtime_ns


type _Behaviour = Callable[[object, object], object]


class AuthInterceptor(grpc.aio.ServerInterceptor):
    """Rejects calls without a valid token of a role the method allows.

    The handler finds the caller in `CALLER`.
    """

    def __init__(
        self, tokens: TokenFile, roles: Mapping[str, frozenset[Role]]
    ) -> None:
        """Checks calls against `tokens`.

        Args:
            tokens: The token file.
            roles: The roles allowed on each method, by its full name such
                as `/vrp.v1.ClientService/SubmitJob`; other methods are
                refused.
        """
        self._tokens = tokens
        self._roles = roles

    @override
    async def intercept_service[Request, Response](
        self,
        continuation: Callable[
            [grpc.HandlerCallDetails],
            Awaitable[grpc.RpcMethodHandler[Request, Response] | None],
        ],
        handler_call_details: grpc.HandlerCallDetails,
    ) -> grpc.RpcMethodHandler[Request, Response] | None:
        """Checks the call's token and role.

        Args:
            continuation: Gives the method's handler.
            handler_call_details: The method and metadata of the call.

        Returns:
            The handler, or one that refuses the call.
        """
        handler = await continuation(handler_call_details)
        if handler is None:
            return None
        metadata = dict(handler_call_details.invocation_metadata or ())
        header = metadata.get(_HEADER, "")
        token = header.removeprefix(_SCHEME) if isinstance(header, str) else ""
        identity = self._tokens.identify(token) if token else None
        if identity is None:
            return _refusing(
                handler, grpc.StatusCode.UNAUTHENTICATED, "invalid token"
            )
        allowed = self._roles.get(handler_call_details.method, frozenset())
        if identity.role not in allowed:
            return _refusing(
                handler,
                grpc.StatusCode.PERMISSION_DENIED,
                f"the {identity.role} role may not call this method",
            )
        return _with_caller(handler, identity)


def server_credentials(certificate: Path, key: Path) -> grpc.ServerCredentials:
    """TLS for the orchestrator's port (ADR 0009).

    Args:
        certificate: The server's PEM certificate chain, from the lab CA.
        key: Its PEM private key.

    Returns:
        The credentials.

    Raises:
        OSError: If a file cannot be read.
    """
    return grpc.ssl_server_credentials(
        [(key.read_bytes(), certificate.read_bytes())]
    )


def _behaviours[Request, Response](
    handler: grpc.RpcMethodHandler[Request, Response],
) -> dict[str, _Behaviour]:
    """The handler's behaviour, under its field name."""
    names = ("unary_unary", "unary_stream", "stream_unary", "stream_stream")
    return {
        name: getattr(handler, name)
        for name in names
        if getattr(handler, name) is not None
    }


def _replace[Request, Response](
    handler: grpc.RpcMethodHandler[Request, Response],
    behaviours: dict[str, _Behaviour],
) -> grpc.RpcMethodHandler[Request, Response]:
    # At run time, a handler is a named tuple; the stubs do not say so.
    replaced: grpc.RpcMethodHandler[Request, Response] = handler._replace(  # type: ignore[attr-defined]
        **behaviours
    )
    return replaced


def _refusing[Request, Response](
    handler: grpc.RpcMethodHandler[Request, Response],
    code: grpc.StatusCode,
    message: str,
) -> grpc.RpcMethodHandler[Request, Response]:
    async def refuse(_request: object, context: object) -> None:
        servicer = cast("grpc.aio.ServicerContext[object, object]", context)
        await servicer.abort(code, message)

    return _replace(handler, dict.fromkeys(_behaviours(handler), refuse))


def _with_caller[Request, Response](
    handler: grpc.RpcMethodHandler[Request, Response], identity: Identity
) -> grpc.RpcMethodHandler[Request, Response]:
    """The handler, with `CALLER` set while each call runs.

    Each call runs in its own task, so the value is the call's alone.
    """

    def wrap(behaviour: _Behaviour) -> _Behaviour:
        # The stubs type aio behaviours as synchronous; they are not.
        if inspect.isasyncgenfunction(behaviour):

            async def generator(
                request: object, context: object
            ) -> AsyncIterator[object]:
                CALLER.set(identity)
                items = cast(
                    "AsyncIterator[object]", behaviour(request, context)
                )
                async for item in items:
                    yield item

            return generator

        async def coroutine(request: object, context: object) -> object:
            CALLER.set(identity)
            return await cast("Awaitable[object]", behaviour(request, context))

        return coroutine

    behaviours = _behaviours(handler)
    return _replace(
        handler,
        {name: wrap(behaviour) for name, behaviour in behaviours.items()},
    )

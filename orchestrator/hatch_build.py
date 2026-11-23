"""Generates the contract's protobuf code when the package is built."""

from pathlib import Path

from grpc_tools import protoc
from hatchling.builders.hooks.plugin.interface import BuildHookInterface


class CustomBuildHook(BuildHookInterface):  # type: ignore[type-arg]
    """Writes `vrp_pb2.py` and its type stub from `proto/vrp/v1/vrp.proto`."""

    def initialize(self, version: str, build_data: dict[str, object]) -> None:  # noqa: ARG002
        """Runs protoc before the build.

        Args:
            version: The build version; unused.
            build_data: The build's data; unused.

        Raises:
            RuntimeError: If protoc fails.
        """
        root = Path(self.root)
        source = root.parent / "proto" / "vrp" / "v1"
        output = root / "src" / "vrp_orchestrator"
        arguments = [
            "protoc",
            f"--proto_path={source}",
            f"--python_out={output}",
            f"--pyi_out={output}",
            str(source / "vrp.proto"),
        ]
        if protoc.main(arguments) != 0:
            msg = "protoc failed on vrp.proto"
            raise RuntimeError(msg)

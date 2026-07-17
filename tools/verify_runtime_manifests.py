#!/usr/bin/env python3
"""Verify exact ROS1 Tools Adapter artifact and canonical manifest digests."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
from typing import Any

from generate_runtime_manifests import build_contracts


def canonical(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def digest(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def file_digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return "sha256:" + hasher.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def verify(args: argparse.Namespace) -> None:
    executable = Path(args.executable)
    adapter = json.loads(Path(args.adapter_manifest).read_text(encoding="utf-8"))
    process = json.loads(Path(args.process_manifest).read_text(encoding="utf-8"))
    references, expected_contracts = build_contracts(Path(args.schema_dir))
    require(
        Path(args.artifact_path).is_absolute(),
        "installed Adapter artifact path must be absolute",
    )
    require(
        set(adapter) == {"apiVersion", "adapters"},
        "Adapter manifest fields do not match the exact schema",
    )
    require(
        adapter["apiVersion"] == "xgc.adapter.definition/v1",
        "unsupported Adapter Runtime manifest API version",
    )
    require(len(adapter["adapters"]) == 1, "exactly one Adapter is required")
    installed = adapter["adapters"][0]
    require(
        set(installed) == {"definition", "capabilityManifest"},
        "installed Adapter fields do not match the exact schema",
    )
    definition = installed["definition"]
    require(
        set(definition)
        == {
            "id",
            "version",
            "processDefinitionId",
            "buildDigest",
            "trustedManifestDigest",
            "configuration",
            "activation",
            "scope",
        },
        "Adapter definition fields do not match the exact schema",
    )
    require(
        definition["id"] == "xgc2-ros1-tools-adapter",
        "unexpected Adapter definition identity",
    )
    require(
        definition["processDefinitionId"] == "xgc2-ros1-tools-adapter",
        "unexpected process definition identity",
    )
    require(
        isinstance(definition["version"], str)
        and re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", definition["version"])
        is not None,
        "Adapter version must use MAJOR.MINOR.PATCH",
    )
    require(
        definition["buildDigest"] == file_digest(executable),
        "Adapter executable digest mismatch",
    )
    require(
        definition["configuration"]
        == {"schema": references["configuration"], "allowedEncodings": ["json"]},
        "Adapter configuration contract mismatch",
    )
    require(
        definition["activation"]
        == {"mode": "on-demand", "idleTimeoutNanos": 300000000000},
        "Adapter activation policy mismatch",
    )
    require(
        definition["scope"]
        == {
            "kind": "ros1-native-context",
            "requiredAttributes": ["master-uri"],
            "optionalAttributes": ["hostname", "ip"],
            "allowAdditionalAttributes": False,
            "sharing": "shared",
        },
        "Adapter scope policy mismatch",
    )
    manifest = installed["capabilityManifest"]
    require(
        manifest == {"formatVersion": 1, "capabilities": expected_contracts},
        "capability manifest differs from repository schemas and policy",
    )
    expected_pairs = {
        ("xgc.ros1.topic.publish", "publish"),
        ("xgc.ros1.service.call", "call"),
    }
    actual_pairs: set[tuple[str, str]] = set()
    for contract in manifest["capabilities"]:
        endpoints = sorted(contract["endpoints"], key=lambda item: item["endpointId"])
        body = {"ref": contract["ref"], "endpoints": endpoints}
        require(
            contract["contractDigest"] == digest(canonical(body)),
            "capability contract digest mismatch",
        )
        require(len(endpoints) == 1, "each ROS1 Tools capability needs one endpoint")
        endpoint = endpoints[0]
        actual_pairs.add((contract["ref"]["id"], endpoint["endpointId"]))
        require(contract["ref"]["version"] == 1, "unexpected capability version")
        require(endpoint["interaction"] == "operation", "unexpected interaction")
        require(endpoint["sideEffect"] == "non-idempotent", "unexpected side effect")
        require(endpoint["idempotency"] == "required", "idempotency must be required")
        require(endpoint["deadlineRequired"] is True, "deadline must be required")
        require(
            endpoint["cancellationSupported"] is True,
            "cancellation must be supported",
        )
    require(actual_pairs == expected_pairs, "ROS1 Tools capability set mismatch")

    canonical_capabilities = sorted(
        manifest["capabilities"],
        key=lambda item: f"{item['ref']['id']}@{item['ref']['version']}",
    )
    canonical_manifest = {
        "formatVersion": manifest["formatVersion"],
        "capabilities": canonical_capabilities,
    }
    require(
        definition["trustedManifestDigest"] == digest(canonical(canonical_manifest)),
        "trusted capability manifest digest mismatch",
    )

    require(
        set(process) == {"apiVersion", "definitions"},
        "process manifest fields do not match the exact schema",
    )
    require(
        process["apiVersion"] == "xgc.execution.process/v1",
        "unsupported process manifest API version",
    )
    require(len(process["definitions"]) == 1, "exactly one process is required")
    process_definition = process["definitions"][0]
    require(
        process_definition
        == {
            "id": definition["processDefinitionId"],
            "version": definition["version"],
            "label": "XGC2 ROS1 Tools Adapter",
            "description": "Generic typed ROS1 topic publish and service call capabilities for Adapter Runtime.",
            "drivers": ["host"],
            "parameters": {
                "properties": {
                    "adapterBootstrapFile": {
                        "type": "string",
                        "description": "Trusted mode-0600 binary AdapterProcessBootstrap path.",
                    }
                },
                "required": ["adapterBootstrapFile"],
                "additionalProperties": False,
            },
            "command": {
                "executable": args.artifact_path,
                "args": ["--adapter-bootstrap-file", "${adapterBootstrapFile}"],
                "directExecutable": True,
            },
            "readiness": {"kind": "process"},
            "liveness": {"kind": "process"},
            "stop": {"gracePeriod": 5000000000},
            "restart": {"mode": "never"},
            "internal": True,
        },
        "process definition differs from the exact internal launch contract",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--artifact-path", required=True)
    parser.add_argument("--schema-dir", required=True)
    parser.add_argument("--adapter-manifest", required=True)
    parser.add_argument("--process-manifest", required=True)
    verify(parser.parse_args())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

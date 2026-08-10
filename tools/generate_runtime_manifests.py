#!/usr/bin/env python3
"""Generate exact process and Adapter Runtime manifests for one built binary."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
from typing import Any


SCHEMAS = {
    "configuration": (4201, "xgc.ros1.tools.v1.NativeContext", "native-context.schema.json"),
    "publish_input": (4212, "xgc.ros1.tools.v2.PublishRequest", "publish-request.schema.json"),
    "publish_output": (4213, "xgc.ros1.tools.v2.PublishResult", "publish-result.schema.json"),
    "service_input": (4204, "xgc.ros1.tools.v1.ServiceCallRequest", "service-call-request.schema.json"),
    "service_output": (4205, "xgc.ros1.tools.v1.ServiceCallResult", "service-call-result.schema.json"),
}


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return "sha256:" + digest.hexdigest()


def schema_reference(schema_dir: Path, key: str) -> dict[str, Any]:
    message_id, type_name, filename = SCHEMAS[key]
    value = json.loads((schema_dir / filename).read_text(encoding="utf-8"))
    if value.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
        raise ValueError(f"{filename} must use JSON Schema draft 2020-12")
    if value.get("$id") != type_name:
        raise ValueError(f"{filename} $id does not match {type_name}")
    if value.get("type") != "object":
        raise ValueError(f"{filename} root must be an object schema")
    normalized = json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    fingerprint = int.from_bytes(hashlib.sha256(normalized).digest()[:8], "big")
    if fingerprint == 0:
        raise ValueError(f"zero schema fingerprint for {filename}")
    return {
        "messageId": message_id,
        "typeName": type_name,
        "schemaVersion": 1,
        "schemaFingerprint": fingerprint,
    }


def endpoint(
    endpoint_id: str,
    input_schema: dict[str, Any],
    output_schema: dict[str, Any],
    maximum_response_bytes: int,
) -> dict[str, Any]:
    return {
        "endpointId": endpoint_id,
        "interaction": "operation",
        "inputSchema": input_schema,
        "outputSchema": output_schema,
        "sideEffect": "non-idempotent",
        "idempotency": "required",
        "cancellationSupported": True,
        "deadlineRequired": True,
        "defaultTimeoutMillis": 30000,
        "maximumTimeoutMillis": 300000,
        "limits": {
            "maximumRequestBytes": 8388608,
            "maximumResponseBytes": maximum_response_bytes,
            "maximumConcurrency": 16,
            "maximumStreams": 0,
            "maximumStreamChunkBytes": 0,
            "maximumStreamChunkMessages": 0,
        },
    }


def contract(capability_id: str, version: int, contract_endpoint: dict[str, Any]) -> dict[str, Any]:
    body = {
        "ref": {"id": capability_id, "version": version},
        "endpoints": [contract_endpoint],
    }
    return {
        "ref": body["ref"],
        "contractDigest": sha256_bytes(canonical_json(body)),
        "endpoints": body["endpoints"],
    }


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def build_contracts(
    schema_dir: Path,
) -> tuple[dict[str, dict[str, Any]], list[dict[str, Any]]]:
    references = {key: schema_reference(schema_dir, key) for key in SCHEMAS}
    contracts = [
        contract(
            "xgc.ros1.topic.publish",
            2,
            endpoint("publish", references["publish_input"], references["publish_output"], 1048576),
        ),
        contract(
            "xgc.ros1.service.call",
            1,
            endpoint("call", references["service_input"], references["service_output"], 8388608),
        ),
    ]
    contracts.sort(key=lambda item: f"{item['ref']['id']}@{item['ref']['version']}")
    return references, contracts


def build_manifests(args: argparse.Namespace) -> tuple[dict[str, Any], dict[str, Any]]:
    executable = Path(args.executable)
    if not executable.is_file():
        raise ValueError(f"Adapter executable does not exist: {executable}")
    if not re.fullmatch(r"[a-z][a-z0-9_]*", args.ros_package):
        raise ValueError("ROS package name must be canonical")
    if not re.fullmatch(r"[a-z][a-z0-9_]*", args.ros_executable):
        raise ValueError("ROS executable name must be canonical")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        raise ValueError("Adapter version must use MAJOR.MINOR.PATCH")
    references, contracts = build_contracts(Path(args.schema_dir))
    capability_manifest = {"formatVersion": 1, "capabilities": contracts}
    manifest_digest = sha256_bytes(canonical_json(capability_manifest))

    adapter_manifest = {
        "apiVersion": "xgc.adapter.definition/v1",
        "adapters": [
            {
                "definition": {
                    "id": "xgc2-ros1-tools-adapter",
                    "version": args.version,
                    "processDefinitionId": "xgc2-ros1-tools-adapter",
                    "buildDigest": sha256_file(executable),
                    "trustedManifestDigest": manifest_digest,
                    "configuration": {
                        "schema": references["configuration"],
                        "allowedEncodings": ["json"],
                    },
                    "activation": {
                        "mode": "on-demand",
                        "idleTimeoutNanos": 300000000000,
                    },
                    "scope": {
                        "kind": "ros1-native-context",
                        "requiredAttributes": ["master-uri"],
                        "optionalAttributes": ["hostname", "ip"],
                        "allowAdditionalAttributes": False,
                        "sharing": "shared",
                    },
                },
                "capabilityManifest": capability_manifest,
            }
        ],
    }
    process_manifest = {
        "apiVersion": "xgc.execution.process/v1",
        "definitions": [
            {
                "id": "xgc2-ros1-tools-adapter",
                "version": args.version,
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
                    "executable": "rosrun",
                    "args": [
                        args.ros_package,
                        args.ros_executable,
                        "--adapter-bootstrap-file",
                        "${adapterBootstrapFile}",
                    ],
                },
                "readiness": {"kind": "process"},
                "liveness": {"kind": "process"},
                "stop": {"gracePeriod": 5000000000},
                "restart": {"mode": "never"},
                "internal": True,
            }
        ],
    }
    return adapter_manifest, process_manifest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--ros-package", required=True)
    parser.add_argument("--ros-executable", required=True)
    parser.add_argument("--schema-dir", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--adapter-output", required=True)
    parser.add_argument("--process-output", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    adapter_manifest, process_manifest = build_manifests(args)
    write_json(Path(args.adapter_output), adapter_manifest)
    write_json(Path(args.process_output), process_manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

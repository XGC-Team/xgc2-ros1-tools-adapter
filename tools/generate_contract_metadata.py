#!/usr/bin/env python3
"""Generate the exact C++ contract compiled into the ROS1 Tools Adapter."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from generate_runtime_manifests import build_contracts


def cpp_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def schema_initializer(schema: dict[str, Any]) -> str:
    return (
        "{" + ", ".join(
            [
                f"{schema['messageId']}u",
                cpp_string(schema["typeName"]),
                f"{schema['schemaVersion']}u",
                f"{schema['schemaFingerprint']}ULL",
            ]
        ) + "}"
    )


def endpoint_initializer(contract: dict[str, Any]) -> str:
    endpoint = contract["endpoints"][0]
    limits = endpoint["limits"]
    values = [
        cpp_string(contract["ref"]["id"]),
        f"{contract['ref']['version']}u",
        cpp_string(contract["contractDigest"]),
        cpp_string(endpoint["endpointId"]),
        schema_initializer(endpoint["inputSchema"]),
        schema_initializer(endpoint["outputSchema"]),
        f"{endpoint['defaultTimeoutMillis']}u",
        f"{endpoint['maximumTimeoutMillis']}u",
        "{" + ", ".join(
            [
                f"{limits['maximumRequestBytes']}u",
                f"{limits['maximumResponseBytes']}u",
                f"{limits['maximumConcurrency']}u",
                f"{limits['maximumStreams']}u",
                f"{limits['maximumStreamChunkBytes']}u",
                f"{limits['maximumStreamChunkMessages']}u",
            ]
        ) + "}",
    ]
    return "{" + ", ".join(values) + "}"


def generate(schema_dir: Path) -> str:
    references, contracts = build_contracts(schema_dir)
    by_id = {item["ref"]["id"]: item for item in contracts}
    publish = by_id["xgc.ros1.topic.publish"]
    service = by_id["xgc.ros1.service.call"]
    return f"""#pragma once

#include <cstdint>

namespace xgc_ros1_tools_adapter {{
namespace contract {{

struct Schema {{
  std::uint32_t message_id;
  const char* type_name;
  std::uint32_t version;
  std::uint64_t fingerprint;
}};

struct Limits {{
  std::uint32_t maximum_request_bytes;
  std::uint32_t maximum_response_bytes;
  std::uint32_t maximum_concurrency;
  std::uint32_t maximum_streams;
  std::uint32_t maximum_stream_chunk_bytes;
  std::uint32_t maximum_stream_chunk_messages;
}};

struct Endpoint {{
  const char* capability_id;
  std::uint32_t contract_version;
  const char* contract_digest;
  const char* endpoint_id;
  Schema input_schema;
  Schema output_schema;
  std::uint32_t default_timeout_ms;
  std::uint32_t maximum_timeout_ms;
  Limits limits;
}};

inline constexpr Schema kConfiguration = {schema_initializer(references['configuration'])};
inline constexpr Endpoint kPublish = {endpoint_initializer(publish)};
inline constexpr Endpoint kService = {endpoint_initializer(service)};

}}  // namespace contract
}}  // namespace xgc_ros1_tools_adapter
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--schema-dir", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(generate(Path(args.schema_dir)), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

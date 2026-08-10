#!/usr/bin/env python3
"""Contract-generation and exact Runtime-manifest regression tests."""

from __future__ import annotations

import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SCHEMA_DIR = REPOSITORY_ROOT / "schemas"
TOOLS_DIR = REPOSITORY_ROOT / "tools"
sys.path.insert(0, str(TOOLS_DIR))

import generate_contract_metadata  # noqa: E402
import generate_runtime_manifests  # noqa: E402
import verify_runtime_manifests  # noqa: E402


class RuntimeManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.executable = self.root / "adapter"
        self.executable.write_bytes(b"exact-adapter-binary")
        self.ros_package = "xgc_ros1_tools_adapter"
        self.ros_executable = "xgc_ros1_tools_adapter_node"
        build_args = argparse.Namespace(
            executable=str(self.executable),
            ros_package=self.ros_package,
            ros_executable=self.ros_executable,
            schema_dir=str(SCHEMA_DIR),
            version="0.2.0",
        )
        self.adapter, self.process = generate_runtime_manifests.build_manifests(
            build_args
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_and_verify(self, adapter: dict, process: dict) -> None:
        adapter_path = self.root / "adapter.json"
        process_path = self.root / "process.json"
        adapter_path.write_text(json.dumps(adapter), encoding="utf-8")
        process_path.write_text(json.dumps(process), encoding="utf-8")
        verify_runtime_manifests.verify(
            argparse.Namespace(
                executable=str(self.executable),
                ros_package=self.ros_package,
                ros_executable=self.ros_executable,
                schema_dir=str(SCHEMA_DIR),
                adapter_manifest=str(adapter_path),
                process_manifest=str(process_path),
            )
        )

    def test_exact_generated_manifests_are_accepted(self) -> None:
        self.write_and_verify(self.adapter, self.process)

    def test_compiled_metadata_uses_the_same_contracts(self) -> None:
        references, contracts = generate_runtime_manifests.build_contracts(SCHEMA_DIR)
        header = generate_contract_metadata.generate(SCHEMA_DIR)
        self.assertIn(str(references["configuration"]["schemaFingerprint"]), header)
        for contract in contracts:
            self.assertIn(contract["ref"]["id"], header)
            self.assertIn(contract["contractDigest"], header)
            endpoint = contract["endpoints"][0]
            self.assertIn(str(endpoint["inputSchema"]["schemaFingerprint"]), header)
            self.assertIn(str(endpoint["outputSchema"]["schemaFingerprint"]), header)

    def test_contract_and_process_mutations_are_rejected(self) -> None:
        mutations = {
            "definition-extra-field": lambda adapter, process: adapter["adapters"][
                0
            ]["definition"].__setitem__("artifact", "/tmp/bypass"),
            "invalid-version": lambda adapter, process: (
                adapter["adapters"][0]["definition"].__setitem__("version", "latest"),
                process["definitions"][0].__setitem__("version", "latest"),
            ),
            "configuration-schema": lambda adapter, process: adapter["adapters"][
                0
            ]["definition"]["configuration"]["schema"].__setitem__(
                "schemaFingerprint", 1
            ),
            "contract-digest": lambda adapter, process: adapter["adapters"][0][
                "capabilityManifest"
            ]["capabilities"][0].__setitem__("contractDigest", "sha256:" + "0" * 64),
            "endpoint-limit": lambda adapter, process: adapter["adapters"][0][
                "capabilityManifest"
            ]["capabilities"][0]["endpoints"][0]["limits"].__setitem__(
                "maximumConcurrency", 17
            ),
            "process-command": lambda adapter, process: process["definitions"][0][
                "command"
            ].__setitem__("executable", "/tmp/bypass"),
            "process-external": lambda adapter, process: process["definitions"][0].__setitem__(
                "internal", False
            ),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                adapter = copy.deepcopy(self.adapter)
                process = copy.deepcopy(self.process)
                mutate(adapter, process)
                with self.assertRaises((KeyError, TypeError, ValueError)):
                    self.write_and_verify(adapter, process)

    def test_optimized_python_rejects_tampering(self) -> None:
        adapter = copy.deepcopy(self.adapter)
        adapter["adapters"][0]["capabilityManifest"]["capabilities"][0][
            "endpoints"
        ][0]["limits"]["maximumRequestBytes"] += 1
        adapter_path = self.root / "tampered-adapter.json"
        process_path = self.root / "process.json"
        adapter_path.write_text(json.dumps(adapter), encoding="utf-8")
        process_path.write_text(json.dumps(self.process), encoding="utf-8")
        environment = dict(os.environ)
        environment["PYTHONOPTIMIZE"] = "1"
        result = subprocess.run(
            [
                sys.executable,
                str(TOOLS_DIR / "verify_runtime_manifests.py"),
                "--executable",
                str(self.executable),
                "--ros-package",
                self.ros_package,
                "--ros-executable",
                self.ros_executable,
                "--schema-dir",
                str(SCHEMA_DIR),
                "--adapter-manifest",
                str(adapter_path),
                "--process-manifest",
                str(process_path),
            ],
            check=False,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertNotEqual(0, result.returncode)


if __name__ == "__main__":
    unittest.main()

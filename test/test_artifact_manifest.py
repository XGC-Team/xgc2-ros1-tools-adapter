#!/usr/bin/env python3
"""Release-artifact identity and packaging-boundary regression tests."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MANIFEST_TOOL = ROOT / ".xgc2/scripts/xgc2_artifact_manifest.py"
BUILD_SCRIPT = ROOT / ".xgc2/scripts/build_debs_in_docker.sh"
APT_SCRIPT = ROOT / ".xgc2/scripts/configure_xgc2_apt.sh"
PRODUCT = "xgc2-ros1-tools-adapter"
PACKAGE = "ros-noetic-xgc2-ros1-tools-adapter"
VERSION = "0.2.0-1"


class ArtifactManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def build_deb(
        self,
        directory: Path,
        *,
        package: str = PACKAGE,
        version: str = VERSION,
        architecture: str = "amd64",
    ) -> Path:
        package_root = self.root / f"package-{len(list(self.root.glob('package-*')))}"
        (package_root / "DEBIAN").mkdir(parents=True)
        (package_root / "usr/share/xgc2-tests").mkdir(parents=True)
        (package_root / "DEBIAN/control").write_text(
            "\n".join(
                [
                    f"Package: {package}",
                    f"Version: {version}",
                    "Section: misc",
                    "Priority: optional",
                    f"Architecture: {architecture}",
                    "Maintainer: XGC2 Tests <tests@example.com>",
                    "Description: ROS1 Tools artifact contract test",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        (package_root / "usr/share/xgc2-tests/identity").write_text(
            f"{package}={version}\n", encoding="utf-8"
        )
        directory.mkdir(parents=True, exist_ok=True)
        output = directory / f"{package}_{version}_{architecture}.deb"
        subprocess.run(
            ["dpkg-deb", "--root-owner-group", "--build", package_root, output],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        return output

    def run_manifest_build(
        self, deb_dir: Path, *, expected_returncode: int = 0
    ) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [
                sys.executable,
                str(MANIFEST_TOOL),
                "build",
                "--deb-dir",
                str(deb_dir),
                "--output-dir",
                str(self.root / "manifests"),
                "--product",
                PRODUCT,
                "--expected-package",
                PACKAGE,
                "--product-version",
                VERSION,
                "--distribution",
                "focal",
                "--architecture",
                "amd64",
                "--source-sha",
                "1" * 40,
                "--ci-run-id",
                "12345",
                "--ci-workflow",
                "ci",
                "--ci-workflow-ref",
                "repo/.github/workflows/ci.yml@refs/heads/noetic",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertEqual(
            expected_returncode,
            result.returncode,
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )
        return result

    def test_build_accepts_exactly_one_expected_deb(self) -> None:
        deb_dir = self.root / "debs"
        deb = self.build_deb(deb_dir)
        self.run_manifest_build(deb_dir)
        manifest_path = self.root / "manifests" / f"{PRODUCT}_focal_amd64.build.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual("xgc2.build-artifact.v1", manifest["schema"])
        self.assertEqual(
            [(PACKAGE, VERSION, deb.name)],
            [
                (entry["package"], entry["version"], entry["file"])
                for entry in manifest["debs"]
            ],
        )

    def test_build_rejects_additional_deb(self) -> None:
        deb_dir = self.root / "debs"
        self.build_deb(deb_dir)
        self.build_deb(deb_dir, package="unrelated-package")
        result = self.run_manifest_build(deb_dir, expected_returncode=1)
        self.assertIn("expected exactly one Deb", result.stderr)

    def test_build_rejects_wrong_package_or_version(self) -> None:
        cases = (
            ("wrong-package", VERSION, "package"),
            (PACKAGE, "0.2.0-2", "version"),
        )
        for index, (package, version, expected_message) in enumerate(cases):
            with self.subTest(package=package, version=version):
                deb_dir = self.root / f"debs-{index}"
                self.build_deb(deb_dir, package=package, version=version)
                result = self.run_manifest_build(deb_dir, expected_returncode=1)
                self.assertIn(expected_message, result.stderr)

    def test_verify_rechecks_the_expected_package_identity(self) -> None:
        deb_dir = self.root / "debs"
        deb = self.build_deb(deb_dir)
        self.run_manifest_build(deb_dir)
        arguments = [
            sys.executable,
            str(MANIFEST_TOOL),
            "verify-build",
            "--artifact-dir",
            str(self.root),
            "--deb-output-dir",
            str(self.root / "verified/debs"),
            "--manifest-output-dir",
            str(self.root / "verified/manifests"),
            "--product",
            PRODUCT,
            "--expected-package",
            PACKAGE,
            "--product-version",
            VERSION,
            "--distribution",
            "focal",
            "--architecture",
            "amd64",
            "--source-sha",
            "1" * 40,
            "--ci-run-id",
            "12345",
        ]
        wrong_arguments = list(arguments)
        wrong_arguments[wrong_arguments.index(PACKAGE)] = "wrong-package"
        result = subprocess.run(
            wrong_arguments,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("package", result.stderr)

        subprocess.run(arguments, check=True)
        self.assertEqual(
            deb.read_bytes(),
            (self.root / "verified/debs" / deb.name).read_bytes(),
        )

    def test_build_script_refuses_a_dirty_output_directory(self) -> None:
        output = self.root / "output"
        output.mkdir()
        (output / "stale.deb").write_bytes(b"not a package")
        environment = dict(os.environ)
        environment.pop("XGC2_APT_OVERLAY_URL", None)
        environment.pop("XGC2_DEPENDENCY_SET_DIGEST", None)
        result = subprocess.run(
            [str(BUILD_SCRIPT), "--output-dir", str(output)],
            check=False,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("not isolated", result.stderr)

    def test_build_script_rejects_an_invalid_dependency_digest_before_docker(self) -> None:
        result = subprocess.run(
            [str(BUILD_SCRIPT), "--output-dir", str(self.root / "output")],
            check=False,
            env={"PATH": str(Path("/usr/bin")), "XGC2_DEPENDENCY_SET_DIGEST": "bad"},
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("64 lowercase hex", result.stderr)

    def test_overlay_requires_a_dependency_digest_before_docker(self) -> None:
        environment = dict(os.environ)
        environment["XGC2_APT_OVERLAY_URL"] = "https://staging.example.test/train-1"
        environment.pop("XGC2_DEPENDENCY_SET_DIGEST", None)
        result = subprocess.run(
            [str(BUILD_SCRIPT), "--output-dir", str(self.root / "output")],
            check=False,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("requires XGC2_DEPENDENCY_SET_DIGEST", result.stderr)

    def test_dependency_digest_is_forwarded_to_the_build_container(self) -> None:
        fake_bin = self.root / "fake-bin"
        fake_bin.mkdir()
        docker_log = self.root / "docker.log"
        fake_docker = fake_bin / "docker"
        fake_docker.write_text(
            "#!/usr/bin/env bash\n"
            "printf '%s\\n' \"$*\" >> \"$DOCKER_LOG\"\n",
            encoding="utf-8",
        )
        fake_docker.chmod(0o755)
        dependency_digest = "a" * 64
        environment = dict(os.environ)
        environment.update(
            {
                "DOCKER_LOG": str(docker_log),
                "PATH": f"{fake_bin}:{environment['PATH']}",
                "XGC2_DEPENDENCY_SET_DIGEST": dependency_digest,
            }
        )
        result = subprocess.run(
            [str(BUILD_SCRIPT), "--output-dir", str(self.root / "output")],
            check=False,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.assertNotEqual(0, result.returncode)
        log = docker_log.read_text(encoding="utf-8")
        self.assertIn(
            f"XGC2_DEPENDENCY_SET_DIGEST={dependency_digest}",
            log,
        )


class AptUrlValidationTest(unittest.TestCase):
    def validate(self, url: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(APT_SCRIPT), "--validate-url", url],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def test_accepts_strict_https_repository_url(self) -> None:
        self.assertEqual(
            0,
            self.validate("https://staging.example.test:8443/releases/train-1").returncode,
        )

    def test_rejects_unsafe_repository_urls(self) -> None:
        unsafe = (
            "http://staging.example.test/releases/train-1",
            "https://user:secret@staging.example.test/releases/train-1",
            "https://staging.example.test/releases/train-1?token=secret",
            "https://staging.example.test/releases/train-1#fragment",
            "https://staging.example.test/releases/train-1\n"
            "deb [trusted=yes] https://evil.invalid focal main",
        )
        for url in unsafe:
            with self.subTest(url=url):
                result = self.validate(url)
                self.assertNotEqual(0, result.returncode)

    def test_keeps_production_apt_when_overlay_is_set(self) -> None:
        script = APT_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("/etc/apt/sources.list.d/xgc2.list", script)
        self.assertIn("00-xgc2-release-train.list", script)
        self.assertIn("https://xgc2.apt.xiaokang.ink", script)
        self.assertIn("XGC2_APT_OVERLAY_URL", script)


if __name__ == "__main__":
    unittest.main()

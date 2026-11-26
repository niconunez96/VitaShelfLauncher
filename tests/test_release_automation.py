#!/usr/bin/env python3
"""Regression tests for the release-automation helpers."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from commit_conventions import (  # noqa: E402
    bump_for_message,
    highest_bump,
    next_semver,
    validate_commit_message,
)


class CommitConventionTests(unittest.TestCase):
    def test_accepts_each_release_level(self) -> None:
        for level in ("PATCH", "MINOR", "MAJOR"):
            message = f"{level}(release): describe the change\n\nDetails.\n"
            self.assertEqual([], validate_commit_message(message))
            self.assertEqual(level, bump_for_message(message))

    def test_rejects_missing_or_invalid_release_level(self) -> None:
        self.assertTrue(validate_commit_message("feat: add a feature\n"))
        self.assertTrue(validate_commit_message("MINOR add a feature\n"))
        self.assertTrue(validate_commit_message("PATCH: \n"))

    def test_selects_the_highest_bump(self) -> None:
        messages = [
            "PATCH: fix crash\n",
            "MINOR(ui): add new view\n",
            "MAJOR(api)!: remove deprecated endpoint\n",
        ]
        self.assertEqual("MAJOR", highest_bump(messages))


class SemanticVersionTests(unittest.TestCase):
    def test_increments_each_semantic_component(self) -> None:
        self.assertEqual("v2.4.6", next_semver("v2.4.5", "PATCH"))
        self.assertEqual("v2.5.0", next_semver("v2.4.5", "MINOR"))
        self.assertEqual("v3.0.0", next_semver("v2.4.5", "MAJOR"))

    def test_rejects_an_invalid_current_version(self) -> None:
        with self.assertRaises(ValueError):
            next_semver("2.4", "PATCH")


class CommitMsgHookTests(unittest.TestCase):
    def test_hook_script_accepts_and_rejects_messages(self) -> None:
        hook = ROOT / "githooks" / "commit-msg"
        with tempfile.TemporaryDirectory() as temp_dir:
            message_file = Path(temp_dir) / "COMMIT_EDITMSG"
            message_file.write_text("PATCH: correct release tooling\n", encoding="utf-8")
            accepted = subprocess.run([str(hook), str(message_file)], cwd=ROOT, text=True, capture_output=True)
            self.assertEqual(0, accepted.returncode, accepted.stderr)

            message_file.write_text("fix: missing release intent\n", encoding="utf-8")
            rejected = subprocess.run([str(hook), str(message_file)], cwd=ROOT, text=True, capture_output=True)
            self.assertNotEqual(0, rejected.returncode)
            self.assertIn("invalid commit message", rejected.stderr)


class PullRequestTitleValidationTests(unittest.TestCase):
    def test_validator_accepts_release_intent_titles_and_rejects_invalid_ones(self) -> None:
        validator = ROOT / "scripts" / "validate_pr_title.py"

        accepted = subprocess.run(
            [str(validator), "--title", "MINOR(ui): add carousel"],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        self.assertEqual(0, accepted.returncode, accepted.stderr)

        rejected = subprocess.run(
            [str(validator), "--title", "add carousel"],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        self.assertNotEqual(0, rejected.returncode)
        self.assertIn("invalid pull request title", rejected.stderr)


class WorkflowContractTests(unittest.TestCase):
    def test_ci_workflow_gates_release_and_uploads_the_vpk(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        self.assertIn("pull_request:", workflow)
        self.assertIn("name: Validate pull request title", workflow)
        self.assertIn("python3 scripts/validate_pr_title.py --title \"$PR_TITLE\"", workflow)
        self.assertIn("make test", workflow)
        self.assertIn("./scripts/build-vpk.sh --clean", workflow)
        self.assertIn("needs: [validate, host-tests, build-vpk]", workflow)
        self.assertIn("gh release create", workflow)
        self.assertIn("--generate-notes", workflow)

    def test_ci_workflow_never_exposes_image_service_key_to_pull_requests(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        steps = []
        current = None
        for line in workflow.splitlines():
            if line.startswith("      - "):
                if current is not None:
                    steps.append(current)
                current = [line]
            elif current is not None and (line.startswith("        ") or not line.strip()):
                current.append(line)
            elif current is not None:
                steps.append(current)
                current = None
        if current is not None:
            steps.append(current)

        secret_steps = [step for step in steps if "secrets.VP_IMAGE_SERVICE_KEY" in "\n".join(step)]
        self.assertEqual(1, len(secret_steps))
        body = "\n".join(secret_steps[0])
        self.assertIn("if: github.event_name == 'push' && github.ref == 'refs/heads/main'", body)
        self.assertIn('>> "$GITHUB_ENV"', body)
        build_steps = [step for step in steps if "Build VPK with VitaSDK" in "\n".join(step)]
        self.assertEqual(1, len(build_steps))
        self.assertNotIn("secrets.VP_IMAGE_SERVICE_KEY", "\n".join(build_steps[0]))

    def test_local_vpk_builder_exposes_a_docker_free_interface(self) -> None:
        script = ROOT / "scripts" / "build-vpk-local.sh"
        result = subprocess.run([str(script), "--help"], cwd=ROOT, text=True, capture_output=True)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("--clean", result.stdout)
        self.assertIn("--build-type", result.stdout)
        self.assertIn("VITASDK", result.stdout)
        self.assertNotIn("docker", script.read_text(encoding="utf-8").lower())


if __name__ == "__main__":
    unittest.main()

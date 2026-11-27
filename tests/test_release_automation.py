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


class LegacyReleaseVersionTests(unittest.TestCase):
    def run_release_tools(self, messages: list[str]) -> tuple[subprocess.CompletedProcess, subprocess.CompletedProcess]:
        with tempfile.TemporaryDirectory() as temp_dir:
            def git(*args: str) -> str:
                result = subprocess.run(
                    ["git", "-c", "user.name=Release Test", "-c",
                     "user.email=release-test@example.invalid", *args],
                    cwd=temp_dir, text=True, capture_output=True, check=True,
                )
                return result.stdout.strip()

            git("init", "--quiet")
            git("commit", "--allow-empty", "-m", "PATCH: initial fixture")
            base = git("rev-parse", "HEAD")
            for message in messages:
                git("commit", "--allow-empty", "-m", message)
            revision_range = f"{base}..HEAD"
            version = subprocess.run(
                [sys.executable, str(ROOT / "scripts" / "next_version.py"),
                 "--current", "v0.14.0", "--range", revision_range],
                cwd=temp_dir, text=True, capture_output=True,
            )
            validation = subprocess.run(
                [sys.executable, str(ROOT / "scripts" / "validate_commits.py"), revision_range],
                cwd=temp_dir, text=True, capture_output=True,
            )
            return version, validation

    def test_legacy_subjects_select_versions_but_remain_invalid_new_commits(self) -> None:
        for message, expected in (
            ("feat(vita): add analog navigation", "v0.15.0"),
            ("fix(vita): restore footer", "v0.14.1"),
            ("perf(vita): raise worker priority", "v0.14.1"),
            ("feat: add navigation", "v0.15.0"),
            ("fix(api)!: remove old endpoint", "v1.0.0"),
            ("feat(api): replace endpoint\n\nBREAKING CHANGE: old endpoint removed", "v1.0.0"),
            ("perf: replace cache\n\nBREAKING-CHANGE: incompatible layout", "v1.0.0"),
        ):
            with self.subTest(message=message):
                version, validation = self.run_release_tools([message])
                self.assertEqual(0, version.returncode, version.stderr)
                self.assertEqual(expected + "\n", version.stdout)
                self.assertEqual("", version.stderr)
                self.assertNotEqual(0, validation.returncode)
                self.assertIn("subject must be", validation.stderr)

    def test_mixed_history_selects_highest_release_level(self) -> None:
        version, validation = self.run_release_tools([
            "fix: correct legacy bug", "feat: add legacy feature", "PATCH(ci): repair release",
        ])
        self.assertEqual(0, version.returncode, version.stderr)
        self.assertEqual("v0.15.0\n", version.stdout)
        self.assertEqual("", version.stderr)
        self.assertNotEqual(0, validation.returncode)

    def test_explicit_release_intent_still_works(self) -> None:
        version, validation = self.run_release_tools([
            "PATCH: fix crash", "MINOR(ui): add view", "MAJOR(api)!: replace endpoint",
        ])
        self.assertEqual(0, version.returncode, version.stderr)
        self.assertEqual("v1.0.0\n", version.stdout)
        self.assertEqual("", version.stderr)
        self.assertEqual(0, validation.returncode, validation.stderr)
        self.assertIn("validated 3 commit(s)", validation.stdout)

    def test_explicit_patch_level_is_not_reinterpreted(self) -> None:
        for message in (
            "PATCH!: preserve explicit level",
            "PATCH: change\n\nBREAKING CHANGE: preserve explicit level",
        ):
            with self.subTest(message=message):
                version, validation = self.run_release_tools([message])
                self.assertEqual((0, "v0.14.1\n", ""),
                                 (version.returncode, version.stdout, version.stderr))
                self.assertEqual(0, validation.returncode, validation.stderr)

    def test_unknown_or_malformed_subjects_are_not_silently_ignored(self) -> None:
        for message in ("add an endpoint", "feat: ", "chore: unrelated task"):
            with self.subTest(message=message):
                version, validation = self.run_release_tools([message])
                self.assertNotEqual(0, version.returncode)
                self.assertEqual("", version.stdout)
                self.assertIn("subject must be", version.stderr)
                self.assertNotEqual(0, validation.returncode)


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

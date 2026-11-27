#!/usr/bin/env python3
"""Calculate a semantic release tag, including legacy feat/fix/perf history."""

from __future__ import annotations

import argparse
import re
import subprocess

from commit_conventions import highest_bump, next_semver, subject


LEGACY_HEADER = re.compile(
    r"^(?P<type>feat|fix|perf)(?:\([a-z0-9][a-z0-9._/-]*\))?(?P<breaking>!)?: (?P<summary>\S.*)$"
)


def release_intent_message(message: str) -> str:
    """Translate legacy history for versioning without relaxing commit validation."""
    match = LEGACY_HEADER.fullmatch(subject(message))
    if match is None:
        return message
    breaking_footer = re.search(r"^BREAKING(?: CHANGE|-CHANGE): \S", message, re.MULTILINE)
    if match.group("breaking") or breaking_footer:
        level = "MAJOR"
    else:
        level = "MINOR" if match.group("type") == "feat" else "PATCH"
    return f"{level}: {match.group('summary')}"


def messages_in_range(revision_range: str) -> list[str]:
    result = subprocess.run(
        ["git", "log", "--no-merges", "--format=%B%x00", revision_range],
        check=True,
        capture_output=True,
        text=True,
    )
    return [message for message in result.stdout.split("\0") if message.strip()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--current", required=True, help="current v-prefixed semantic tag")
    parser.add_argument("--range", dest="revision_range", required=True, help="Git revision range")
    args = parser.parse_args()

    bump = highest_bump(release_intent_message(message) for message in messages_in_range(args.revision_range))
    print(next_semver(args.current, bump))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

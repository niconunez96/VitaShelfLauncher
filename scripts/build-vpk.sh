#!/usr/bin/env bash
# Build the VitaShelf Launcher VPK inside the VitaSDK Docker image.
set -euo pipefail

root_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$root_dir/dist/build-vita"
vpk_path="$root_dir/dist/vitashelflauncher.vpk"
image="${VITASDK_IMAGE:-vitasdk/vitasdk:2026.08}"
build_type="${CMAKE_BUILD_TYPE:-}"
clean=0

usage() {
    cat <<'EOF'
Usage: scripts/build-vpk.sh [options]

Builds dist/vitashelflauncher.vpk with Docker. No VitaSDK install is required on the host.

Options:
  --clean             Remove dist/build-vita before configuring.
  --build-type TYPE   Pass -DCMAKE_BUILD_TYPE=TYPE. Debug works; Release currently
                      trips a pre-existing -Wformat-truncation warning (-Werror).
  --image IMAGE       VitaSDK image to use.
  -h, --help          Show this help.

Environment:
  VITASDK_IMAGE       Same as --image (default: vitasdk/vitasdk:2026.08).
  CMAKE_BUILD_TYPE    Same as --build-type.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            clean=1
            ;;
        --build-type)
            build_type="${2:?missing value for --build-type}"
            shift
            ;;
        --build-type=*)
            build_type="${1#*=}"
            ;;
        --image)
            image="${2:?missing value for --image}"
            shift
            ;;
        --image=*)
            image="${1#*=}"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option '$1'" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker is not in PATH" >&2
    exit 1
fi

if ! docker image inspect "$image" >/dev/null 2>&1; then
    echo "==> Pulling $image"
    docker pull "$image"
fi

mkdir -p "$root_dir/dist"
relative_build_dir="${build_dir#"$root_dir"/}"
docker_args=(--rm -e HOME=/tmp -v "$root_dir":/build -w /build)
# Pass values by name only; the secret never appears in the command line or logs.
for name in VP_IMAGE_SERVICE_KEY VP_IMAGE_SERVICE_KEY_ID VP_IMAGE_SERVICE_URL; do
    if [[ -n "${!name:-}" ]]; then
        docker_args+=("-e" "$name")
    fi
done

# Removing the build directory from the host leaves stale directory entries in Colima's
# shared filesystem, so CMake then fails reading its own freshly written cache. Run every
# mutation of the build tree inside the container instead.
remote_cmd=""
if ((clean)); then
    remote_cmd="rm -rf $(printf '%q' "$relative_build_dir") && "
fi
remote_cmd+="cmake -S . -B $(printf '%q' "$relative_build_dir")"
if [[ -n "$build_type" ]]; then
    remote_cmd+=" -DCMAKE_BUILD_TYPE=$(printf '%q' "$build_type")"
fi
remote_cmd+=" && cmake --build $(printf '%q' "$relative_build_dir") -j\$(nproc)"

echo "==> Building with $image"
docker run "${docker_args[@]}" "$image" bash -lc "$remote_cmd"

if [[ ! -f "$build_dir/vitashelflauncher.vpk" ]]; then
    echo "error: $build_dir/vitashelflauncher.vpk was not produced" >&2
    exit 1
fi

# Best effort: a rootful Docker daemon leaves root-owned files behind. Colima already
# maps container root to the host user, where this is a no-op.
docker run "${docker_args[@]}" "$image" \
    chown -R "$(id -u):$(id -g)" "$relative_build_dir" >/dev/null 2>&1 || true

cp "$build_dir/vitashelflauncher.vpk" "$vpk_path"

echo
echo "VPK: $vpk_path"
ls -lh "$vpk_path"
sha256sum "$vpk_path"

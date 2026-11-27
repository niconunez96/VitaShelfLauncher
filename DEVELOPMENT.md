# Development

## Requirements

- `gcc`, `make`, Python 3, and Go for host checks.
- A local VitaSDK installation (default location: `~/.local/vitasdk`) or Docker to build the VPK.

## Test

```sh
make test
make test-assets
```

The host suite does not require VitaSDK or network access.

## Prepare console logos

Start the local image service with alpha-trimming support:

```sh
cd services/imageproxy
IMAGE_PROXY_ADDR=127.0.0.1:8090 go run .
```

From the repository root, in another terminal (Pillow, CairoSVG, and fontTools required):

```sh
python3 scripts/prepare-blueglass.py --logo-service http://127.0.0.1:8090/image
```

This uses the retained sources without downloading replacements. The service trims
transparent margins and resizes the original artwork into 128×128 card plates and
tightly fitted header PNGs (up to 128×64). The VPK bundles these assets; the launcher
does not need the service at runtime to display console logos.

## Build a VPK

With a local VitaSDK (recommended):

```sh
./scripts/build-vpk-local.sh --clean
```

It defaults to `~/.local/vitasdk`; set `VITASDK` if your SDK is elsewhere. The artifact is `dist/vitashelflauncher.vpk`.

Docker alternative:

```sh
./scripts/build-vpk.sh --clean
```

For manual VitaSDK configuration:

```sh
cmake -S . -B build/vita -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"
cmake --build build/vita
```

The direct-build artifact is `build/vita/vitashelflauncher.vpk`.

## Before committing

Install the versioned commit-message hook once:

```sh
./scripts/install-git-hooks.sh
```

Commit subjects must use `<PATCH|MINOR|MAJOR>(optional-scope): summary`; CI enforces
the same rule. For example:

```text
PATCH(ci): handle legacy release history
MINOR(image): add Tailscale Funnel deployment
MAJOR(api): remove an incompatible endpoint
```

Release version calculation also recognizes historical `feat` (MINOR), `fix`
(PATCH), and `perf` (PATCH) subjects. A legacy `!` marker or `BREAKING CHANGE:` /
`BREAKING-CHANGE:` footer selects MAJOR. This compatibility applies only to
`scripts/next_version.py`: new commits and pull request titles must still use
explicit uppercase release levels. Unknown or malformed subjects are rejected,
not silently skipped.

To preview a release from the last tag without creating a tag or publishing:

```sh
python3 scripts/next_version.py --current v0.14.0 --range v0.14.0..HEAD
```

Replace `v0.14.0` with the latest release tag. To validate only the latest commit:

```sh
python3 scripts/validate_commits.py HEAD~1..HEAD
```

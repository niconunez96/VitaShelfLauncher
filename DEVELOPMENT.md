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

Commit subjects must start with `PATCH`, `MINOR`, or `MAJOR`; CI enforces the same rule.

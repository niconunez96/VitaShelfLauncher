# imageproxy

Tiny HTTP service that receives image bytes, center-crops them, resizes them, and returns a PNG.

For complete game covers, send `mode=contain`: it preserves the entire source and
returns its natural aspect ratio within the requested size. The default mode
(`cover`) retains the existing exact-size center crop for backgrounds.

It has no database, job queue, cache, framework, or remote URL fetcher. A caller can place a normal HTTP cache in front of it if needed.

## Run

```sh
go run .
```

Optional `IMAGE_PROXY_ADDR` defaults to `:8090`.

`GET /health` returns `200 OK` with `ok` and is intended for container health checks.
`GET /` returns `200 OK` with `imageproxy is running`, which is useful for checking the
public Dokploy domain before configuring VitaShelf Launcher.

## Deploy with Dokploy

Create a **Docker Compose** application in Dokploy from this repository, set its compose
file to `services/imageproxy/compose.dokploy.yml`, and set its build context to
`services/imageproxy`. In Dokploy's environment variables, define:

```text
IMAGE_PROXY_DOMAIN=images.example.com
```

Point that hostname's DNS A/AAAA record at the Dokploy server, then deploy. The compose
file joins Dokploy's external `dokploy-network`, enables TLS through Traefik, and has no
persistent volume because the service is stateless. It also publishes port `8090` directly,
so `http://<server-ip>:8090/` is available for connectivity checks when the server firewall
allows it.

Set VitaShelf Launcher's `image_proxy_url` to the public endpoint, including its path:

```text
image_proxy_url=https://images.example.com/image
```

## Request

```text
POST /image?width=256&height=320
Content-Type: application/octet-stream

<source image bytes>
```

The response is `image/png`, exactly `width × height`, with a center crop that preserves the source aspect ratio. PNG, JPEG, and GIF input are supported through Go's standard library. Output dimensions are limited to `1024×1024`; input bodies to `8 MiB`; and source dimensions to 16 megapixels.

Example: `/image?width=256&height=320&mode=contain` returns a square source as
`256×256`, a 1:2 portrait as `160×320`, or a 2:1 landscape as `256×128`.
`mode=cover` returns exactly the requested dimensions. Other mode values return 400.

### Trim transparent logo margins

Add `trim=alpha` to remove fully transparent outer margins **before** resizing:

```text
POST /image?width=128&height=128&mode=contain&trim=alpha
Content-Type: application/octet-stream

<source PNG bytes>
```

The complete visible logo is fitted inside the requested bounds without stretching.
Partially transparent pixels, including antialiased edges, are retained. Opaque and
fully transparent images are unchanged by trimming. Without `trim` (or with an empty
value), processing is identical to earlier versions; existing Vita clients need no
changes. Unsupported trim values return 400. The existing input/output limits apply.

The caller downloads the source image and sends its raw bytes. This keeps the service independent from RomM authentication and prevents it from becoming an SSRF proxy.

## Cache note

Cover output is downscaled with a quality resampler and returned as PNG. Blueglass
writes complete covers to `ux0:data/vitashelflauncher/cache/covers/fit-v2-*.png` and game
backgrounds to `art-v1-*.png`. Existing `processed-*.png` files remain a fallback.
Redeploy this service, synchronize the catalog, then run **Sincronizar metadata**
in Settings/RomM to replace old cropped images and prepare available game backgrounds.

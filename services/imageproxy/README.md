# imageproxy

Tiny HTTP service that receives image bytes, center-crops them, resizes them, and returns a PNG.

For complete game covers, send `mode=contain`: it preserves the entire source and
returns its natural aspect ratio within the requested size. The default mode
(`cover`) retains the existing exact-size center crop for backgrounds.

It has no database, job queue, cache, framework, or remote URL fetcher. **Caller-supplied
URLs are never fetched by this service**: the client downloads the source image and sends
its raw bytes, which keeps the service independent from RomM authentication and prevents
it from becoming an SSRF proxy. A caller can place a normal HTTP cache in front of it if needed.

## Modes and environment

The mode is selected by the two authentication variables:

- **Public mode** — both `IMAGE_PROXY_KEY` and `IMAGE_PROXY_KEY_ID` are set. Protocol v1
  authentication and quotas are enabled, and image responses use
  `Cache-Control: private, max-age=86400`.
- **Local mode** — both are empty (the default). `POST /image` stays unauthenticated,
  the handshake endpoints are not exposed, no new quotas apply, and image responses keep
  `Cache-Control: public, max-age=86400`.

Setting exactly one of the two variables is a startup error: the process exits with a log
message naming both variables. `GET /health` and `GET /` stay public in both modes.

| Variable | Purpose | Default |
| --- | --- | --- |
| `IMAGE_PROXY_ADDR` | Listen address | `:8090` |
| `IMAGE_PROXY_KEY` | Raw HMAC key used to verify proofs; secret, never sent to clients | empty (local mode) |
| `IMAGE_PROXY_KEY_ID` | Public identifier accepted by `/v1/session` | empty (local mode) |

Quotas use fixed defaults; there are no environment overrides in this version.

## Protocol v1 endpoints (public mode only)

| Endpoint | Request | Success |
| --- | --- | --- |
| `POST /v1/challenge` | No body | `200 {"challenge":"<b64url 32 bytes>","expires_in":60}` |
| `POST /v1/session` | `application/json` with `key_id`, `installation_id`, `challenge`, `proof` | `200 {"token":"<b64url 32 bytes>","expires_in":3600}` |
| `POST /image?...` | Raw image bytes plus `Authorization: Bearer <token>` | `200 image/png` |

The proof is base64url (without padding) of
`HMAC-SHA256(key = raw IMAGE_PROXY_KEY bytes, message = "vitashelf-image-auth-v1\n" + key_id + "\n" + installation_id + "\n" + challenge + "\n")`.
Comparison is constant time. `key_id`, `installation_id` and `challenge` must contain only
`[A-Za-z0-9._-]` and be at most 128 bytes. A challenge lives for 60 seconds and is consumed
by the first `/v1/session` attempt, even if that attempt fails; tokens live for 3600 seconds.
Malformed input returns 400, invalid proof, challenge, key ID or token returns 401, and quota
rejections return 429. Missing or invalid tokens are rejected before the request body is read.
Session tokens are indexed in memory by SHA-256 digest and are never written to logs.

## Quotas and hardening (public mode only)

- `/v1/challenge` and `/v1/session` share a bucket: 5 requests per minute per IP, burst 3.
- `/image`: 60 requests per minute per installation, burst 10.
- `/image`: 300 requests per minute per IP, burst 50.
- At most 2 concurrent `/image` requests per installation and 4 globally.
- Rejections return `429` with a positive integer `Retry-After` in seconds and happen before
  the image body is read or decoded. Per-IP buckets use the socket peer address; Traefik adds
  an edge-level IP rate limit in the provided compose file.
- In-memory buckets, challenges and sessions are bounded and expired entries are cleaned up.

The HTTP server uses a 5 second header read timeout, 10 second read timeout, 60 second write
timeout, 30 second idle timeout, and a 1 MiB maximum header size.

## Run

```sh
go run .
```

`GET /health` returns `200 OK` with `ok` and is intended for container health checks.
`GET /` returns `200 OK` with `imageproxy is running`, which is useful for checking the
public Dokploy domain before configuring VitaShelf Launcher.

## Deploy with Dokploy

Create a **Docker Compose** application in Dokploy from this repository, set its compose
file to `services/imageproxy/compose.dokploy.yml`, and set its build context to
`services/imageproxy`. In Dokploy's environment variables, define:

```text
IMAGE_PROXY_DOMAIN=images.example.com
IMAGE_PROXY_KEY=<generated secret>
IMAGE_PROXY_KEY_ID=<public identifier>
```

The compose file passes the two authentication variables through to the container. Omit both
(or leave both empty) to run an unauthenticated local service. The compose file joins
Dokploy's external `dokploy-network`, enables TLS through Traefik, attaches a Traefik IP
rate-limit middleware (average 300/minute, burst 50) to the router, and has no persistent
volume because the service is stateless. It does **not** publish port `8090` directly: the
service is reachable through the Traefik route only, while `expose: 8090` keeps it available
on the internal network.

Set VitaShelf Launcher's `image_proxy_url` to the public endpoint, including its path:

```text
image_proxy_url=https://images.example.com/image
```

## Request

```text
POST /image?width=256&height=320
Content-Type: application/octet-stream
Authorization: Bearer <token>   # public mode only

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

## Cache note

Cover output is downscaled with a quality resampler and returned as PNG. Blueglass
writes complete covers to `ux0:data/vitashelflauncher/cache/covers/fit-v2-*.png` and game
backgrounds to `art-v1-*.png`. Existing `processed-*.png` files remain a fallback.
Redeploy this service, synchronize the catalog, then run **Sincronizar metadata**
in Settings/RomM to replace old cropped images and prepare available game backgrounds.

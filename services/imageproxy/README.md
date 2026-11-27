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

### Free stable hostname with Tailscale Funnel

For Tailscale installed **on the Dokploy server host**, use
`services/imageproxy/compose.dokploy-funnel.yml` **instead of**
`compose.dokploy.yml`. This is a standalone alternative, not a Compose override.
It keeps imageproxy off host ports and routes all public requests through Traefik's
rate limiter. A small TCP relay publishes only host loopback port `8091`, so no
additional port mapping is needed on Dokploy's Traefik container. The original
domain/Let's Encrypt deployment remains unchanged.

```text
Internet -> Funnel HTTPS -> host loopback:8091 -> TCP relay -> Traefik:8091 -> imageproxy:8090
```

#### 1. Prepare Traefik's private entrypoint

This deployment requires Dokploy's Traefik container, named `dokploy-traefik`, to
use **bridge networking** and join `dokploy-network`. Host networking is not
supported by this Compose variant.

In Dokploy's **Traefik File System** editor, back up `traefik.yml`, then add
`funnel` under its existing `entryPoints` mapping. Preserve all existing entries,
ports, providers, and other settings; do not replace the whole file:

```yaml
entryPoints:
  funnel:
    address: ":8091"
```

Apply this static configuration with **Settings → Web Server → Traefik → Reload**.
Reloading Traefik can briefly interrupt other applications on the server.

**Do not add `8091` in Traefik's Additional Port Mappings dialog.** The relay in
this repository owns `127.0.0.1:8091`; Traefik listens on `8091` only inside its
Docker network. If you previously published that Traefik port, remove its mapping
before deploying the relay, without changing the existing `80` and `443` mappings.

Do not enable TLS, HTTP-to-HTTPS redirects, or dashboard forwarding on the
`funnel` entrypoint. Funnel supplies public HTTPS; the unencrypted hops stay on
this server. Keep `8091` blocked from external interfaces, including IPv6, and do
not enable `forwardedHeaders.insecure` or trust client-supplied forwarded headers.
The relay forwards TCP bytes unchanged, preserving the Host header and application
authentication. It resolves `dokploy-traefik` for each new connection, so new
requests can recover after Traefik is recreated with a different container IP.

#### 2. Deploy the Funnel variant

Enable Tailscale Funnel for this device in your tailnet and obtain its full DNS
name from `tailscale status --json` (`Self.DNSName`, without the trailing dot).
In the existing Dokploy Compose application, select the Funnel compose file, keep
the build context as `services/imageproxy`, and set:

```text
IMAGE_PROXY_FUNNEL_HOST=image-server.your-tailnet.ts.net
IMAGE_PROXY_KEY=<existing secret>
IMAGE_PROXY_KEY_ID=<existing public identifier>
```

Use the real hostname without `https://` or a path. This variant requires both auth
variables to be nonempty: it must not publish the unauthenticated local mode.
`IMAGE_PROXY_DOMAIN` is not needed, and no Let's Encrypt resolver is used.
Replace the existing deployment rather than running both variants simultaneously;
they deliberately reuse Traefik service and middleware names.

Redeploy, then start Funnel **on the server host**:

```sh
sudo tailscale funnel --bg 8091
tailscale funnel status
```

If you already use public port `8443`, keep that port instead:

```sh
sudo tailscale funnel --bg --https=8443 http://127.0.0.1:8091
```

Include `:8443` in public test URLs and the launcher's `image_proxy_url`, but not
in `IMAGE_PROXY_FUNNEL_HOST`.

Follow any Tailscale approval prompts. The background configuration resumes when
Tailscale starts after a reboot. Do not point Funnel at imageproxy's `8090` port or
at a general-purpose Traefik/dashboard entrypoint.

#### 3. Verify before using the public endpoint

```sh
# On the server: the expected host reaches the private Traefik router.
curl --fail -H 'Host: image-server.your-tailnet.ts.net' http://127.0.0.1:8091/health
# An unrelated host must not reach imageproxy (normally Traefik returns 404).
curl -i -H 'Host: unrelated.invalid' http://127.0.0.1:8091/health
# From a separate internet connection: expect 200 and "ok".
curl --fail https://image-server.your-tailnet.ts.net/health
# Expect 401 without an application session token.
curl -i -X POST 'https://image-server.your-tailnet.ts.net/image?width=1&height=1'
```

If the local health check reports connection refused, check the `funnel-relay`
container's status and logs in Dokploy. A `502` from the public URL alone does not
identify which upstream hop failed. Check relay logs for DNS/connect errors and
Traefik logs for a missing `funnel` entrypoint, then repeat the local health check.
After a planned Traefik recreation, verify it again without restarting the relay;
in-flight requests may fail during recreation, but new requests should recover.

Also verify from another machine that direct access to the server's ports `8090`
and `8091` fails. Confirm Traefik emits `429` when its configured request limit is
exceeded, using a controlled test before production traffic. The reverse-proxy
hops can place visitors in a shared IP bucket; the configured 300/minute limit
must not be assumed to be independent for each internet client. Application
per-installation quotas still apply. Funnel itself also has bandwidth limits.

Set the launcher endpoint to
`https://image-server.your-tailnet.ts.net/image`. Keep the device name and tailnet
DNS name unchanged to retain the hostname. No browser/email authentication gate
is needed: the launcher uses the service's existing challenge/session protocol.

To stop public sharing, run `sudo tailscale funnel off`. To revert the deployment,
select `compose.dokploy.yml` again with a valid `IMAGE_PROXY_DOMAIN`, remove the
orphaned `funnel-relay` container (which releases loopback port `8091`), and remove
the private Traefik entrypoint when no longer needed.

Validate both Compose variants locally without starting containers:

```sh
python3 tests/test_imageproxy_deployment.py
```

This check requires Docker Compose v2; it does not require a Docker daemon.
Live Funnel reachability, host firewall rules, and Traefik routing must be checked
on the deployment server using the steps above.

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

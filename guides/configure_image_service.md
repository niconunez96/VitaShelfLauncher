# Configure Image Service

The image service resizes artwork from RomM and SteamGridDB before VitaShelf caches it on the Vita. It is separate from your RomM server.

## Use the default service

A fresh release installation configures the public HTTPS service automatically. No image-service key or manual authentication setup is required.

Open **Settings → Integrations → Image Service → Test connection** to check it.

## Use your own service

1. Deploy the [image proxy](../services/imageproxy/README.md).
2. In **Settings → Integrations → Image Service**, set **Host** to its reachable URL, including any non-default port.
3. Select **Test connection**.

Use HTTPS for public access. Plain HTTP is suitable only for a trusted private network. If using Dokploy/Traefik, a LAN IP is not interchangeable with the configured hostname: routing and exposed ports must also match.

**Expected result:** `/health` returns HTTP `200` with `ok`. The official service also checks its authentication session.

## What the settings do

| Setting | Purpose |
| --- | --- |
| Host / `image_proxy_url` | Artwork-service address. Both a host URL and the legacy `/image` suffix are supported. |
| `image_service_host` | Alternative INI key for entering a host without `/image`. |
| `image_install_id` | Locally generated installation identifier. Leave it alone; it is not a user-supplied API key or proof of a successful connection. |

Custom hosts use the launcher's unauthenticated mode. An authenticated deployment must be configured compatibly; pasting a RomM token here will not authenticate it.

**Next:** [Configure SteamGridDB covers](configure_steamgriddb.md). Failures: [Problems and debugging](problems_debugging.md).

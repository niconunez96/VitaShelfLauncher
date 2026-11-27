# Configure RomM

RomM provides your server-hosted game catalog and downloads. It is optional: installed Vita games work without it.

## Connect and pair

1. Open **Settings → Integrations → RomM** in VitaShelf.
2. Set **Server URL** to your RomM base URL, for example `http://192.168.1.100:8080`.
3. In RomM, use **Pair Device** to generate a code with the minimum read and download permissions you need.
4. Enter that code in VitaShelf's **Pair code** field.
5. Press `Triangle` to redeem it and sync the catalog.

**Expected result:** your server's games appear in the library. **Settings → About** shows the server, synchronized game count, and last sync.

## What the settings do

| Setting | Purpose |
| --- | --- |
| Server URL / `romm_url` | The RomM server to browse and download from; not the image-service URL. |
| Pair code | A one-time code that grants this device access. |
| `romm_token` | The access token saved after pairing. Keep it private. |

Use a URL reachable from the Vita's network. `localhost` points to the Vita itself, not your computer. Use HTTPS when accessing your server over an untrusted network.

Do not share your real `config.ini`: it contains the saved token.

**Next:** [Download and launch games](initialize_games.md). Connection failed? See [Problems and debugging](problems_debugging.md).

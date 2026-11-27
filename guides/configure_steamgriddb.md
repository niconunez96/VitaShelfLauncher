# Configure SteamGridDB

SteamGridDB finds cover artwork for installed Vita games. It is optional; without it, unmatched games keep their installed application icons.

## Sync covers

1. Check that [Image Service](configure_image_service.md) connects successfully.
2. Get an API key from your [SteamGridDB preferences](https://www.steamgriddb.com/profile/preferences/api).
3. Open **Settings → Integrations → SteamGridDB** and enter the key.
4. Select **Test connection**, then **Sync covers**.

**Expected result:** unambiguous matches gain cached cover artwork. Missing or ambiguous matches keep their existing icons. Successful local-title matches are remembered by title ID.

## What the setting does

`steamgriddb_api_key` authorizes searches against SteamGridDB. It is separate from RomM pairing and image-service authentication. The UI masks it, but the saved configuration still contains it: keep the file private.

Covers already cached on the Vita remain available offline.

**Next:** [Browse your games](initialize_games.md). Failures: [Problems and debugging](problems_debugging.md).

# Problems and debugging

Start with the symptom below. Detailed TLS capture is optional and should be enabled only for a short investigation.

## Quick fixes

| Problem | Check first |
| --- | --- |
| RomM will not connect | Check Wi-Fi, the server URL/port, and whether that address is reachable from the Vita. Pair again if access was revoked. [RomM setup](configure_romm.md). |
| Image Service will not connect | Use the intended hostname and exposed port. Its `/health` must return HTTP `200` and `ok`; official-service authentication can still fail afterward. [Image setup](configure_image_service.md). |
| Covers do not change | Test Image Service and SteamGridDB, then sync covers. Ambiguous matches intentionally keep the installed icon. |
| PSP launch/plugin installation fails | Check Adrenaline and `ux0:data/vitashelflauncher/psp-plugin.log`. The screen includes the Vita error code. |
| NDS download does not start | Create `ux0:/data/dsvita/` and verify the emulator/dependencies. |
| 3DS or Dreamcast will not launch | Install the correct launcher and use a supported single-file ROM. [Game preparation](initialize_games.md). |
| A title is missing | Check hidden/downloaded-only filters. The library has a 256-entry limit; installed Vita titles take priority. |

## Read the connection log

Run **Test connection**, then open `ux0:data/vitashelflauncher/network.log` in VitaShell. Logging needs writable storage and a build containing these diagnostics.

| Record / field | Meaning |
| --- | --- |
| `health-start`, `health-ok`, `health-failed` | The image health test and its result. |
| `service-http`, `challenge`, `session` | Transport result for a health/service request or authentication step. |
| `curl`, `http`, `truncated` | Transfer error, HTTP status, and oversized-response indicator. `http=0` means no HTTP response was received. |
| `auth-ok`, `auth-cached`, `auth-failed` | Session established, reused, or not obtained. A successful health check alone does not prove authentication worked. |
| `curl_version`, `ssl_version`, `tls_detail`, `reason` | Runtime versions and recognized TLS error details. `unavailable` means the detail was absent or not recognized. |
| `openssl_error_present` | Distinguishes an extracted zero error code from no extracted code; neither proves success. |
| `errorbuffer_nonempty`, `errorbuffer_result` | Whether curl supplied detail and accepted its buffer (`result=0`). |
| `rng_ready` | OpenSSL RNG readiness before the request (`1` ready). Never work around failure with predictable randomness. |
| `os_errno`, `os_errno_result`, `trace_result` | Saved socket error and diagnostic setup/query results; zero socket error does not prove TLS succeeded. |

`curl=35` means TLS negotiation failed; it does not identify the cause by itself. Use a current build: a missing SNI hostname in older builds was resolved by the Vita IPv4 parser compatibility fix. Do not disable certificate or hostname verification.

## Capture TLS details

Close VitaShelf and edit `ux0:data/vitashelflauncher/config.ini`:

```ini
tls_debug=1
tls12=0
```

| Setting | What it does |
| --- | --- |
| `tls_debug=1` | Captures original curl text and the first complete outgoing ClientHello per request. Default: `0` (off). |
| `tls12=1` | Diagnostic comparison restricted to TLS 1.2, with verification still enabled. `0` restores normal negotiation. Not a general connection fix. |

1. Relaunch and run **Test connection** once.
2. Inspect these files in `ux0:data/vitashelflauncher/`:
   - `tls-debug.log`: original curl text, capped at 4096 bytes per message.
   - `tls-clienthello.log`: initial ClientHello hex, capped at 16 KiB per message; requires backend callback support.
3. Close the app, set both switches to `0`, and relaunch.
4. Delete private captures when no longer needed. Disabling capture does not erase them; logs can grow while enabled.

Only the exact INI value `1` enables a switch. Saves preserve both settings; the old `.enabled` marker files are ignored. RomM requests are not affected by these TLS switches.

**Keep logs private.** Raw text can contain URLs, addresses, and paths. ClientHello data can contain SNI hostnames, random/session identifiers, and resumption tickets. These captures exclude HTTP headers/bodies and TLS private keys, but still need inspection before sharing. Older RomM records in `network.log` may contain response bodies or URLs too. Never share the real `config.ini` or API tokens.

## PSP-specific logs

`ms0:` is Adrenaline's alias, not a VitaShell folder. Check `ux0:/pspemu/seplugins/` for `vitashelf.prx`; Adrenaline 8.x can use EPI entries in `plugins.txt`.

The boot request and runtime log are under `ux0:/pspemu/VitaShelfLauncher/` (inside Adrenaline: `ms0:/VitaShelfLauncher/`). Installation failures use the separate `psp-plugin.log` path above.

## Report a problem

Include the app version, reproduction steps, expected/actual result, and only the relevant redacted log lines. **Settings → About** shows the version, free space, storage paths, and server sync details. A computer reaching the server does not prove the Vita can reach it.

[Back to setup guides](../README.md#setup-guides)

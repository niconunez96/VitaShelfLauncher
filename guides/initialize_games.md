# Initialize games

VitaShelf discovers installed Vita titles and browses games from RomM. Other platforms need their own launcher or emulator; VitaShelf does not install those for you.

## Installed Vita games

Open VitaShelf after installing a title. Games are discovered on startup and resume under **PS Vita**; press `X` to launch one. Discovery and cached artwork work offline.

The shared library holds up to 256 entries, prioritizing installed Vita titles. Enable [SteamGridDB](configure_steamgriddb.md) if you want additional covers.

## Games from RomM

1. [Connect and sync RomM](configure_romm.md).
2. Install the required platform launcher below.
3. Download a game from its library entry, then press `X` to launch it.
4. For paused downloads, press `X` to resume or `Square` to remove the queued item.

| Platform | Preparation | Download location |
| --- | --- | --- |
| PSP | Install Adrenaline. On first launch, confirm installation/enabling of the bundled `vitashelf.prx` if prompted; no extra VPK or bubble is needed. | `ux0:/pspemu/ISO/` |
| NDS | Install [DSVita](https://github.com/Grarak/DSVita) and its documented dependencies (`libshacccg.suprx`, `kubridge.skprx` 0.3.1+). Create `ux0:/data/dsvita/` **before downloading**. | `ux0:/data/dsvita/` |
| 3DS | Install [Azahar Vita](https://github.com/SaNch0sE/azahar-vita-mh4u). The default launcher is `AZAHARPSV`. | `ux0:/data/vitashelflauncher/downloads/romm/3ds/` |
| Dreamcast | Install Flycast Vita (`FLYCASTDC`) with `libshacccg.suprx`, `kubridge.skprx`, and `fd_fix.skprx` or `repatch_ex.skprx`. | `ux0:/data/vitashelflauncher/downloads/romm/dreamcast/` |

PSP supports `.iso` / `.cso`; a ZIP containing exactly one image is extracted and then removed. NDS does the same for a ZIP containing exactly one `.nds` ROM. 3DS launches single-file ROMs. Dreamcast supports single-file `.chd`, `.cdi`, `.gdi`, or `.cue`; multi-file disc sets must be downloaded outside the app and placed in Flycast's content folder.

## Launch settings

Default NDS, 3DS, and Dreamcast launch profiles are included; no INI override is needed for normal use. `launch_<platform>` entries override the installed launcher/title ID and optional core. Use the [example configuration](../config.example.ini) only if your setup needs a different profile.

## Browse and organize

- `Triangle`: switch between grid, list, and carousel; the view is saved.
- `SELECT`: open filters from the library or a platform page. Downloaded-only and hidden-game filters persist across platforms and restarts.
- `Square` inside a platform: favorite/unfavorite or hide/show a game. Hidden titles appear dimmed when shown; **Favourites** is the first library card.

Launch failed? See [Problems and debugging](problems_debugging.md).

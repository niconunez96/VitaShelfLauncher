# VitaShelf Launcher

A homebrew launcher for PS Vita/PSTV. Browse installed Vita games and your personal [RomM](https://romm.app/) library in one place.

> VitaShelf does not include games, emulators, or a RomM server. Use only software you are entitled to use.

## Screenshots

Screenshots captured in Vita3K.

### Library

![VitaShelf library synchronized with RomM](docs/images/vitashelf-home.png)

### Game views

| Grid | List | Carousel |
| --- | --- | --- |
| ![Grid view](docs/images/vitashelf-roms-grid.png) | ![List view](docs/images/vitashelf-roms-list.png) | ![Carousel view](docs/images/vitashelf-roms-carousel.png) |

### RomM connection

![RomM connection settings](docs/images/vitashelf-romm-connection.png)

## Install

**Required:** a homebrew-enabled PS Vita/PSTV with [VitaShell](https://github.com/TheOfficialFloW/VitaShell).

1. Download `vitashelflauncher.vpk` from the [latest release](https://github.com/niconunez96/VitaShelfLauncher/releases/latest).
2. Transfer the VPK to any folder in `ux0:` using VitaShell over USB or FTP.
3. Select the VPK in VitaShell, press `X`, and confirm installation.
4. Open **VitaShelf Launcher** from LiveArea.

## Setup guides

| I want to… | Guide |
| --- | --- |
| Connect my server and sync its catalog | [Configure RomM](guides/configure_romm.md) |
| Set up the shared artwork service | [Configure Image Service](guides/configure_image_service.md) |
| Find covers for installed Vita games | [Configure SteamGridDB](guides/configure_steamgriddb.md) |
| Discover, download, and launch games | [Initialize games](guides/initialize_games.md) |
| Change the wallpaper, text, or game view | [Customize appearance](guides/customize_appearance.md) |
| Fix a connection or launch problem | [Problems and debugging](guides/problems_debugging.md) |

## Contribute and license

Contributors: [Development guide](DEVELOPMENT.md). AI agents: [agents.md](agents.md).

Licensed under [GPL-3.0](LICENSE). Third-party notices are included in [artwork assets](assets/blueglass/) and [PSP assets](assets/bin/psp/).

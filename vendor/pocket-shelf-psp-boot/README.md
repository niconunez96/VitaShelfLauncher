# VitaShelf PSP boot plugin source

This directory vendors and renames the `psp-boot/` plugin from Pocket Shelf.

- Upstream project: `fthdgn-gh/pocket-shelf`
- Upstream tag: `shelf-v0.6.1-beta.3`
- Upstream commit: `2af22cc2013707a9e1ab76cdfc32fb603cb92df6`
- Upstream license: MIT, preserved verbatim in `LICENSE`
- Vendored scope: `psp-boot/` only. The GPL-3.0 `updater/` directory is not included.

Changes made by VitaShelf Launcher:

- PRX target renamed from `pocketshelf` to `vitashelflauncher_pspboot`.
- PSP module renamed from `PocketShelfBoot` to `VitaShelfPspBoot`.
- Request/log folder renamed from `ms0:/PocketShelf/` to `ms0:/VitaShelfLauncher/`.
- Comments updated to describe VitaShelf Launcher.
- No upstream functional logic was changed.

Rebuild from the repository root with:

```sh
./scripts/build-psp-plugin.sh
```

The build copies `vitashelflauncher_pspboot.prx` to `assets/psp/vitashelflauncher_pspboot.prx`.

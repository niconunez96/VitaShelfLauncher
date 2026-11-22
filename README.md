# VitaShelf Launcher

Homebrew launcher for PS Vita/PSTV. Browse your local library and connect your personal [RomM](https://romm.app/) server from a single interface.

> Use only with games and software you are entitled to use. VitaShelf Launcher does not include games, emulators, or a RomM server.

## Capturas / Screenshots

Capturas reales de la app ejecutándose en Vita3K. La primera muestra una biblioteca sincronizada desde RomM; la segunda indica dónde configurar tu servidor RomM. / Real screenshots of the app running in Vita3K. The first shows a library synchronized from RomM; the second points to the RomM server setup.

### Biblioteca / Library

![Biblioteca de VitaShelf Launcher sincronizada desde RomM](docs/images/vitashelf-home.png)

### Vistas de ROMs / ROM views

La Biblioteca ofrece tres maneras de explorar cada plataforma: grilla, lista con detalle y carrusel. / The Library offers three ways to browse each platform: grid, detailed list, and carousel.

#### Grilla / Grid

![Vista de grilla de ROMs de VitaShelf Launcher](docs/images/vitashelf-roms-grid.png)

#### Lista / List

![Vista de lista de ROMs de VitaShelf Launcher](docs/images/vitashelf-roms-list.png)

#### Carrusel / Carousel

![Vista de carrusel de ROMs de VitaShelf Launcher](docs/images/vitashelf-roms-carousel.png)

### Integraciones → RomM / Integrations → RomM

![Pantalla de conexión a RomM de VitaShelf Launcher](docs/images/vitashelf-romm-connection.png)

## Español

### 1. Instalar

**Necesitás:** una PS Vita o PSTV con homebrew y [VitaShell](https://github.com/TheOfficialFloW/VitaShell) instalado.

1. Descargá `vitashelflauncher.vpk` desde la [última release](https://github.com/niconunez96/VitaShelfLauncher/releases/latest).
2. Copialo a la consola con VitaShell: conectala por USB o FTP y transferí el archivo a cualquier carpeta de `ux0:`.
3. En VitaShell, seleccioná el archivo `.vpk` y presioná `X`. Confirmá la instalación.
4. Volvé a LiveArea y abrí **VitaShelf Launcher**.

### 2. Configurar RomM

1. En la app, abrí **Ajustes → Integraciones → RomM**.
2. Elegí **URL del servidor**, presioná `X` e ingresá la URL base de tu RomM; por ejemplo, `http://192.168.1.100:8080`.
3. En RomM, creá un código con **Pair Device** para un cliente con los permisos mínimos de lectura y descarga.
4. En VitaShelf, elegí **Pair code**, ingresá el código y presioná `Triángulo` para canjearlo y sincronizar el catálogo.

El código se usa una sola vez. No compartas el archivo de configuración de la app: contiene el token de acceso recibido. Para descargas pausadas, presioná `X` para reanudar o `Cuadrado` para quitarlas de la cola.

Los ajustes se ordenan en **Integraciones**, **Descargas**, **Apariencia** y **Acerca de**. En **Apariencia**, elige uno de los cuatro fondos con la cruceta y `X`, o toca su miniatura. **Fondo animado** activa o desactiva el movimiento sutil; ambos ajustes se guardan automáticamente. La vista de ROM elegida con `Triángulo` (cuadrícula, lista o carrusel) también se guarda y se restaura al reiniciar. En **Acerca de** puedes consultar la versión, el espacio libre y las rutas de almacenamiento; si RomM está conectado, también verás el servidor, la cantidad de juegos de su catálogo y la última sincronización. Si aún no hay un catálogo sincronizado para ese servidor, se indica sin mostrar datos de otro servidor.

## English

### 1. Install

**You need:** a homebrew-enabled PS Vita or PSTV with [VitaShell](https://github.com/TheOfficialFloW/VitaShell) installed.

1. Download `vitashelflauncher.vpk` from the [latest release](https://github.com/niconunez96/VitaShelfLauncher/releases/latest).
2. Copy it to the console with VitaShell: connect over USB or FTP and transfer the file to any folder in `ux0:`.
3. In VitaShell, select the `.vpk` file and press `X`. Confirm the installation.
4. Return to LiveArea and open **VitaShelf Launcher**.

### 2. Configure RomM

1. In the app, open **Settings → Integrations → RomM**.
2. Select **Server URL**, press `X`, and enter your RomM base URL; for example, `http://192.168.1.100:8080`.
3. In RomM, create a code with **Pair Device** for a client with the minimum read and download permissions.
4. In VitaShelf, select **Pair code**, enter the code, then press `Triangle` to redeem it and sync your catalog.

The code is one-time use. Do not share the app configuration file: it contains the access token received. For paused downloads, press `X` to resume or `Square` to remove them from the queue.

Settings are ordered as **Integrations**, **Downloads**, **Appearance**, and **About**. In **Appearance**, choose one of four wallpapers with the directional pad and `X`, or tap its thumbnail. **Text color** lets you pick white, light, dark, or black for better readability on light or dark wallpapers. **Animated wallpaper** toggles subtle movement; all appearance preferences save automatically. The ROM view picked with `Triangle` (grid, list, or carousel) is saved and restored on the next launch too. **About** shows the version, free space, and storage paths; when RomM is connected, it also shows the server, its catalog game count, and the last synchronization. A server without a synchronized catalog is identified explicitly rather than showing another server's data.

## Development

Human contributors: see [DEVELOPMENT.md](DEVELOPMENT.md). AI agents: begin with [agents.md](agents.md).

## License and acknowledgements

Third-party asset notices are included under `assets/blueglass/`.

# Hydra

Hydra is an experimental Nintendo Switch emulator for macOS.

## Status

The emulator is still in very early stages. A few games and homebrew apps work perfectly, but most will either crash or have issues that make them unplayable.

![Showcase](img/showcase.png)

NRO, decrypted NCA, NX and XCI formats are supported. You can convert an NSP file to NCA with [this tool](https://github.com/SamoZ256/switch-extract-macos), and NSP to NX with [this tool](https://github.com/SamoZ256/herakles).

For XCI gamecards, use **Import Keys** in the SwiftUI toolbar or Settings > General to import `prod.keys` from your console, then use **Add Game** to select the XCI. The loader reads and decrypts the base game directly from the cartridge file, without extracting a second copy. Keys are stored locally at `~/Library/Application Support/Hydra/keys/prod.keys`; `~/.switch/prod.keys` is also supported. The key file must contain `header_key` and the matching `key_area_key_application_XX` (or ocean/system) entries. NCA3 with AES-CTR, AES-XTS or plaintext sections is supported. Common tickets in NSP/XCI packages are supported with the matching titlekek in `prod.keys`; personalized tickets, compressed/sparse NCA sections and multi-game selection are not supported. Loading a gamecard does not guarantee emulation compatibility.

In order to run official games, you will need to download a set of patches to prevent crashes. You can get the patches together with a guide on how to install them [here](https://github.com/SamoZ256/hydra-patches).

## Usage

You can download the latest release from [here](https://github.com/SamoZ256/hydra/releases/latest).

### Dependencies

You can install Hydra dependencies with a package manager of your choice, like `brew`.

```sh
brew install cmake ninja llvm boost fmt sdl3
```

### Building

First, clone the repository and update submodules.

```sh
git clone https://github.com/SamoZ256/hydra.git
cd hydra
git submodule update --init --recursive
```

Now configure CMake and build with Ninja.

```sh
cmake . -B build -G Ninja -DMACOS_BUNDLE=ON
ninja -C build
```

You will find a macOS app at `build/bin/Hydra.app`. If you want to use the SwiftUI frontend instead of `SDL3`, you can use the `-DFRONTEND=SwiftUI` option.

For live texture-cache code updates, shader overrides and frame captures, see
[Live graphics debugging](tests/README-hot-debug.md).

### Configuring

You can find a config file at `~/Library/Application Support/Hydra/config.toml` after launching the emulator at least once.

#### Firmware

Some games require firmware files from a Nintendo Switch console to function properly. The firmware files need to be decrypted, which can be done with [this tool](https://github.com/SamoZ256/switch-extract-macos).

On macOS, open Settings > System and use **Choose Firmware Folder…** to select an extracted folder containing `.nca` files. You can also set the `firmware_path` option in the config file to that directory. Firmware errors are shown in Settings while keeping the firmware controls available. Encrypted NCA3 firmware can also be read after importing `prod.keys`, provided it contains the keys for that firmware's generation. A missing key is reported when starting emulation.

### XCI loader tests

Build with `-DHYDRA_XCI_TESTS=ON`, install Python `cryptography` for the test fixture generator, then run:

```sh
python3 tests/generate_xci_fixtures.py build/xci-fixtures
./build/bin/hydra-xci-tests build/xci-fixtures
```

The tests use synthetic data and independent AES fixtures to check random reads, decryption cache boundaries, malformed input, wrong/missing keys and base-game selection. An optional second argument tests a local XCI's filesystems, control metadata and icon.

On macOS, the boot regression tests need the app bundle's resources and your configured firmware. Run the test executable inside the bundle:

```sh
cp build/bin/hydra-xci-tests build/bin/Hydra.app/Contents/MacOS/
./build/bin/Hydra.app/Contents/MacOS/hydra-xci-tests build/xci-fixtures /path/to/game.xci --check-firmware-lifetime
```

This also checks firmware font lifetime, incomplete firmware errors, heap accounting, Hypervisor physical address limits, controller availability, GPU map handle reuse and GPU address lookup boundaries/overlap precedence. For performance testing, disable **Debug logging** in Settings > Debug; normal and error logs remain available. Use a Release build to measure gameplay speed. The Debug build is intended for diagnostics.

### Game updates and DLC

Stop emulation, then choose **Install Update / DLC** in the game library toolbar
and select an uncompressed `.nsp` or `.xci` package. Hydra validates its content
metadata and SHA-256 content hashes, copies it to the app data directory under
`content/packages`, and automatically applies the newest matching installed
update and DLC when the base XCI/NSP (including XCZ/NSZ) is next opened. The base
game stays in its original location. Reinstalling the same package is harmless.
Updates and DLC bundled in the base package are also selected through their metadata.

NCA3 updates support full replacement sections and BKTR RomFS overlays with
AES-CTREx subsections; DLC Data/PublicData RomFS is exposed through `aoc:u` and
filesystem data storage. A DLC requiring a newer game update reports the required
version. Import the matching keys before installing encrypted packages.

Installation currently excludes compressed update/DLC packages, personalized
tickets, sparse/compressed sections, multi-program updates and DLC data patches.
Installed content is applied to container-based games; extracted folders and
standalone NCA/NX games do not use this content store. Compatibility with every
game is not guaranteed. See [update/DLC tests and limitations](tests/README-content.md).

## Contributing

Pull requests are very welcome. Feel free to join the official [Discord server](https://discord.gg/FZ742SWv8f) for better communication.

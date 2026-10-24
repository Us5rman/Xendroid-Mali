<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/xendroid-plus-logo-dark.png">
    <img src="docs/assets/xendroid-plus-logo.png" alt="Xendroid+" width="300">
  </picture>
</p>

<p align="center"><strong>Xbox 360 emulation on Android, tuned for Mali.</strong></p>

<p align="center">
  <img alt="Android 10+" src="https://img.shields.io/badge/Android-10%2B-3DBB2E?logo=android&logoColor=white">
  <img alt="arm64-v8a" src="https://img.shields.io/badge/ABI-arm64--v8a-2B2D31">
  <img alt="Vulkan" src="https://img.shields.io/badge/Vulkan-Adreno%20%7C%20Turnip-AC162C?logo=vulkan&logoColor=white">
  <a href="https://discord.gg/AT8Bswv62"><img alt="Discord" src="https://img.shields.io/badge/Discord-community-5865F2?logo=discord&logoColor=white"></a>
</p>

<p align="center"><b>English</b> · <a href="README.pt-BR.md">Português (Brasil)</a></p>

---

**Xendroid-Mali** is an Xbox 360 emulator for Mali GPU's. Its based on the XenDroid+ project [Xenia](https://github.com/xenia-project/xenia)

> [!NOTE]
> Xendroid-Mali is experimental. How a game runs depends on the game, the device and the
> GPU driver: some titles run well, others still show glitches or do not boot.

## Highlights

### A new interface
- **Library** with search by name or Title ID, favorites, sorting, play time and
  *Recently played*, fully usable with a controller.
- **In-game menu** with graphics, HUD, controls and session pages, opened from the
  touch handle, Back/Guide or the shoulder buttons.
- **First-run assistant**, gamer profiles, save backup and restore (with a recovery
  journal and a trash), title update and DLC installation.
- **Touch controls** with a layout editor and optional adaptive sticks.

### Performance, measured on device
- **Per-game settings** applied when a title starts, for the games that need them:
  Forza Horizon, Forza Horizon 2, Gears of War 3, Crysis 3, Need for Speed: Most
  Wanted, Rise of the Tomb Raider and more.
- **Fewer EDRAM transfers** on the host GPU: transfers a draw overwrites are skipped,
  and in the games that allow it, depth drawn at 4× MSAA lands straight in its 1×
  surface and predicated tiling bands are drawn once.
- **Exact shader fast paths** (texture sign decoding, 21-bit rounding), texture loads
  straight into images, direct host resolves, and Turnip's shader compiler set up for
  many small draws.
- **A lighter command thread**: resolves are copied back into game memory only once
  the game reads them, commands go out in small batches when the GPU runs out of work,
  and on Turnip the constant buffers use dynamic offsets and texture descriptor sets
  are reused.
- **ARM64 JIT work**: cheap exact NaN handling, ABA-safe guest reservations and
  VMX denormal flushing.
- **Asynchronous shader compilation** with a persistent pipeline cache.

### Accuracy and stability
- **Rendering fixes** found with frame dumps and checked on device: Crysis 3's lighting
  and textures, Rise of the Tomb Raider's main menu and first level, and Gears of War
  3's colors.
- **The image no longer freezes** after minutes of play on Android (FIFO presentation).
- **Audio hiccups** fade out and back in instead of clicking, on AAudio and OpenSL ES.

### Tools
- **Performance HUD** (FPS, frame time, CPU, GPU, RAM, temperatures) and per-game
  frame-rate limits (unlimited, or 30 to 120 FPS).
- **Output scaling and sharpening**, including Snapdragon Game Super Resolution.
- **Driver manager** for PanVK/Turnip drivers, global or per game, with SHA-256
  checks.
- **Play history** with frame-time percentiles, a diagnostics bundle to share,
  local compatibility notes and community settings profiles.
- **Frame generation** (experimental, off by default): ZeroFG and LSFG (with your own
  `Lossless.dll`) turn a 30 fps game into 60 on the screen, and the HUD shows both
  rates.

## Performance snapshot

Measured on a **POCO F7** (Snapdragon 8s Gen 4, Adreno 825) with Turnip, in
development builds (October 2026). Your device, driver and game version will change
these numbers.

| Game | Tested in | Frame rate | Notes |
|---|---|---|---|
| Forza Horizon | Racing | 30 fps (game cap) | ~18 ms of GPU time per frame |
| Forza Horizon 2 | Racing | 27–30 fps | Forza Horizon's settings applied |
| Gears of War 3 | Campaign, Versus | 30 fps (game cap) | GPU 40 → 28 ms per frame in its heaviest scene; washed-out colors fixed |
| Crysis 3 | First checkpoint | 30 fps (game cap) | Up from ~22 fps; lighting and textures fixed |
| Halo 4 | Campaign | ~26–30 fps | Video cutscenes fixed; a small shadow flicker remains |
| Need for Speed: Most Wanted | Racing | ~21 fps | Without forcing the GPU clock (was ~12 fps); boot and gameplay crashes fixed |
| Rise of the Tomb Raider | Main menu, first level | ~20 fps | Renders now: the menu was a flat blue screen |
| Sonic Unleashed | Gameplay | 50–52 fps | |
| Halo: Reach | Menus, intro | 30 fps | Dips to 19 fps |
| Grand Theft Auto IV | Menus, intro | ~22 fps | Limited by the GPU command thread |
| Red Dead Redemption | Menus, intro | ~25 fps | Limited by the GPU command thread |

The measurements, methods and optimization plans are in
[`performance-tests/`](performance-tests); per-game settings are explained in
[GAME_COMPAT.md](GAME_COMPAT.md).

## Requirements

- Android 10 or newer on 64-bit ARM (arm64-v8a).
- A Vulkan GPU. a Mali G615 MC6 or newer is recommended.
- Your own Xbox 360 games, dumped from discs or consoles you own: disc images (ISO),
  ZAR archives or Games on Demand (GOD) packages.

## Getting started

1. Install the Xendroid-Mali APK.
2. Open it and follow the first-run assistant: pick your games folder, or let it create one along with folders for title updates and DLC.
3. Choose a game in the library. While playing, open the in-game menu from the touch
   handle, or with Back/Guide on a controller.

## Building from source

Xendroid-Mali builds with JDK 21, the Android SDK and NDK, and the SPIR-V shader
toolchain. The full setup for Linux and Windows hosts is in [BUILD.md](BUILD.md):

```sh
./gradlew :app:assembleRelease
```

## Documentation

- [BUILD.md](BUILD.md): building, testing and installing.
- [GAME_COMPAT.md](GAME_COMPAT.md): per-game settings and how they are loaded.
- [`performance-tests/`](performance-tests): device measurements and the optimization
  plans behind each change.
- [docs/ui-redesign](docs/ui-redesign): the interface redesign;
  [docs/experiencia-em-jogo-status.md](docs/experiencia-em-jogo-status.md): feature
  status and pending validation.

## Credits

- **Xendroid+**: created and maintained by **[phforner0](https://github.com/phforner0)**.
- **XenDroid**, the original Android port, by **[rfandango](https://github.com/rfandango)**.
- **[Xenia](https://github.com/xenia-project/xenia)** and Xenia Canary, the Xbox 360 emulator research project the emulation core comes from.
- Frame generation, fonts, upscaling and the other third-party components keep their
  own licenses, listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## License

Xendroid-Mali's own code is Copyright (C) 2026 phforner0 and the Xendroid+ contributors,
licensed under the [GNU Affero General Public License v3.0 or later](LICENSE). Anyone
who distributes a modified version, or lets people use one over a network, has to
publish its source code under the same license. Code that came from Xenia, XenDroid and
the third-party components keeps its own license; see
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

<h1 align="center">Kodi — Blu-ray Menu Edition</h1>

<p align="center">
  <strong>Blu-ray and 4K Blu-ray discs with their own BD-J menus, on Android.</strong>
</p>

<p align="center">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Android%20%C2%B7%20Shield-blue?style=flat-square">
  <img alt="Base" src="https://img.shields.io/badge/based%20on-Kodi%2022%20Piers-blue?style=flat-square">
  <a href="https://github.com/fandangos/Kodi-HDR-Edition/releases/tag/android-bluray-menu-2026-v4"><img alt="Latest" src="https://img.shields.io/badge/latest-v4-brightgreen?style=flat-square"></a>
  <a href="https://forum.kodi.tv/showthread.php?tid=360250"><img alt="Forum" src="https://img.shields.io/badge/support-forum%20thread-orange?style=flat-square"></a>
</p>

<p align="center">
  <a href="https://github.com/fandangos/Kodi-HDR-Edition/releases/tag/android-bluray-menu-2026-v4"><strong>⬇ Download the APK</strong></a>
</p>

<!-- Screenshots: Kodi's own UI is safe to show. A disc menu capture is not, unless
     the disc is openly licensed (the Blender open movies have Blu-ray releases).
     ![Disc listing](docs/discs.png)
-->

---

## What it does

- **BD-J menus** on Blu-ray and 4K Blu-ray, running on a bundled Java runtime
- **Dolby Vision from disc**
- **Tone mapping for PGS subtitles** and for the disc's own overlays, so neither
  is blown out on an HDR disc
- **Disc asset caching**, so a disc that has been opened once starts faster
- **Subtitle files** sitting beside a disc are offered alongside the disc's own

Everything not about discs is stock Kodi 22 (Piers).

## Known limitations

- **Dolby Vision profile 7 FEL is not supported.** Enable Dolby Vision
  compatibility mode.
- Menus are only as good as the disc's own BD-J; some discs use it in ways that
  do not work here.

## Installing

Download the APK for your device and install it. Nothing else is needed — the
Java runtime is bundled.

Both a 32-bit and a 64-bit build are published; take the one matching your device.

## Support

The [forum thread](https://forum.kodi.tv/showthread.php?tid=360250) is the place —
there is no issue tracker here.

## Building it

This is a Kodi fork and builds as Kodi does for Android. The Java runtime is built
separately, from these:

- aarch32 — <https://github.com/fandangos/openjdk-aarch32-jdk8u>
- aarch64 — <https://github.com/fandangos/openjdk-multiarch-jdk8u>

## Credits

Kodi is made by [Team Kodi](https://kodi.tv/).

This exists because of **Petri Hintukainen**, who wrote
[libbluray](https://www.videolan.org/developers/libbluray.html), helped from the
very start, built the Java image this ships, and solved most of the hard problems
along the way. Thanks also to **Shaya Potter**, who helped throughout development,
and to the [PojavLauncher](https://github.com/PojavLauncherTeam) developers, whose
work on running a JVM under Android made the rest possible.

---

<sub>
An unofficial build. Not made, supported or endorsed by Team Kodi — please do not
report problems with it to them. GPLv2, like Kodi itself.
</sub>

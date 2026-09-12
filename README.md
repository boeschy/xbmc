<a href="https://play.google.com/store/apps/details?id=org.xbmc.kodi" target="_blank">
  <img src="https://play.google.com/intl/en_us/badges/images/generic/en-play-badge.png" height="80"/>
</a>

<h1 align="center">
  Welcome to Kodi Home Theater Software!
</h1>

This is a Kodi fork with the **[Fandangos Anroid Blu-Ray JRE menu patches](https://github.com/fandangos/Kodi-HDR-Edition)**, spiced up with the **[popcornmix patches](https://github.com/popcornmix/xbmc)** for proper 3D Blu-Ray menu handling and an implementation of the **[edge264-mvc 3D software decoder](https://github.com/jens-duttke/edge264-mvc)** for 3D Blu-Ray ISO and 3D MVC MKV playback on Android devices without a 3D MVC hardware decoder. Claude Fable helped me to make it work. It is tested on my Nvidia Shield pro 2019, I can't tell if it works on other Android devices.

## What works:
- Blu-Ray JRE menus work (almost) perfectly (THX2 Fandangos), even with 3D Blu-Rays (THX2 popcornmix)
- 3D MVC MKV files created with MakeMKV playback with SBS/TAB output (THX2 Jens Duttke for the MVC software decoder)
- 3D BD ISO playback with SBS/TAB output (THX2 Jens Duttke for the MVC software decoder)
- 3D BD subtitles have the correct depth like authored (THX2 popcornmix/cinema-ONE)
- Changing audio/subtitles with UI or hotkeys works in BD nav-mode now

## What doesn't work:
- 3D framepacked HDMI output (not supported by the underlying Android)
- 3D display needs to be set to the correct 3D mode manually, because HDMI auto toggle is not supported by the underlying Android. But all content of a 3D Blu-Ray, 2D and 3D, is shown in the configured 3D output mode to avoid having to toggle the display between 2D and 3D several times.

Kodi is an award-winning **free and open source** software media player and entertainment hub for digital media. Available as a native application for **Android, Linux, BSD, macOS, iOS, tvOS and Windows operating systems**, Kodi runs on most common processor architectures.

Created in 2003 by a group of like minded programmers, Kodi is a non-profit project run by the XBMC Foundation and developed by volunteers located around the world. More than 500 software developers have contributed to Kodi to date, and 100-plus translators have worked to expand its reach, making it available in more than 70 languages.

While Kodi functions very well as a standard media player application for your computer, it has been designed to be the perfect companion for your HTPC. With its **beautiful interface and powerful skinning engine**, Kodi feels very natural to use from the couch with a remote control and is the ideal solution for your home theater.

## Give your media the love it deserves
Kodi can be used to play almost all popular audio and video formats around. It was designed for network playback, so you can stream your multimedia from anywhere in the house or directly from the internet using practically any protocol available.

Point Kodi to your media and watch it **scan and automagically create a personalized library** complete with box covers, descriptions, and fanart. There are playlist and slideshow functions, a weather forecast feature and many audio visualizations. Once installed, your computer or HTPC will become a fully functional multimedia jukebox.

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

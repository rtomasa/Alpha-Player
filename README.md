# Alpha-Player

A port of the powerful audio/video encoding/decoding library FFmpeg to libretro. This core allows playback of a variety of audio and video formats, with a fancy audio visualizer and the ability to do interframe blending for smoother scrolling of non-native framerates.

# Important Note

This is a project based on libretro FFmpeg core used by Retroarch. Since the original core is included inside the main Retroarch project, it is not feasible to directly fork the same. I've reorganized the file and folder structure and performed some code refactoring to allow its isolation into a sepparated new project.

# Author/License

Media-Player FFmpeg based project is authored by

* Fabrice Bellard
* FFmpeg team
* Rubén Tomás (RTA)

# Original FFmpeg Core Information

https://docs.libretro.com/library/ffmpeg/
https://github.com/libretro/docs/blob/master/docs/library/ffmpeg.md

# Compilation
just execute `make` or `make DEBUG=1` if you want a more verbose execution

# IMPORTANT NOTE!!!
This core has been modified focusing on Raspberry Pi devices using a development version of RePlay OS, so it is not guarantee that it works in other systems or platforms (Linux only).

# Controls

* JOYPAD_LEFT - seek -15s
* JOYPAD_RIGHT - seek +15s
* JOYPAD_UP - seek +180s
* JOYPAD_DOWN - seek -180s
* JOYPAD_START - play/pause
* JOYPAD_A - display progress
* JOYPAD_B - display media title
* JOYPAD_X - enable/disable video subtitles
* JOYPAD_Y - change video lang audio track
* JOYPAD_L - NOT SET (reserved for future m3u functionality)
* JOYPAD_R - NOT SET (reserved for future m3u functionality)
* JOYPAD_L2 - seek -300s
* JOYPAD_R2 - seek +300s

# Changelog

# v2.0.0
- [X] Upgraded code base to make use of modern ffmpeg API
- [X] Upgraded code base to make use of modern libretro API v2 instead of old v0
- [X] Added new option to enable loop content
- [X] Added compilation flag to enable/disable FFmpeg debug messages
- [X] Added ability to display video title on start
- [X] Added ability to display music title on start
- [X] Added ability to display video audio track title when changing
- [X] Added ability to display music or video title when pressing B
- [X] Added ability to display current progress time when pressing A
- [X] Added ability to display current progress % in addition to time stamps
- [X] Disabled audio and subtitle track change when playing music
- [X] Changed default software decoder threads to 1 since it performs the same as multitrhead and avoids crashes
- [X] Changed API shutdown request on media finish to blank screen
- [X] Changed video audio track button mapping from L1 to Y
- [X] Changed video subtitle track button mapping from R1 to X
- [X] Changed some option labels and categorized by Music and Video
- [X] Changed Audio Visualizer Resolution (FFT Resolution) option values:
    * 320x240 for 4:3
    * 320x180 for 16:9
- [X] Changed HW decoder default value to Auto
- [X] Changed seek time from 10 to 15 (left/right) and from 60 to 180 (up/down)
- [X] Changed audio track OSD message to also display the track name
- [X] Fixed Fast Fourier Transform (FFT) bug preventing frontend from being displayed

# v1.0.0

- This version was based on the original media player from retroarch with some quick fixes

# TODO

- [ ] Add M3U support
- [ ] Add L2/R2 functionality to do next/last song/video in m3u lists
- [ ] Add aspect ratio for CRT

<!--
## TODO
- [ ] Fix issue in CRT when videos do not follow standards (e.g. 288@60Hz). This causes A/V desync due to timings generated with different refresh rates (e.g. 288@60Hz is transformed to 288@50Hz)
- [ ] Check HW h264 `decoders "ffmpeg -decoders | grep h264` -> h264_v4l2m2m
- [?] Fix bug
    [dca @ 0x558a792630] Not a valid DCA frame
    ERROR] [LRCORE] [FFMPEG] Can't decode audio packet: Invalid data found when processing input
-->
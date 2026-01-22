# Alpha-Player

A port of the powerful audio/video encoding/decoding library FFmpeg to libretro. This core allows playback of a variety of audio and video formats, with a fancy audio visualizer and the ability to do interframe blending for smoother scrolling of non-native framerates.

# Important Note

This is a project based on libretro FFmpeg core used by Retroarch. Since the original core is included inside the main Retroarch project, it is not feasible to directly fork the same. I've reorganized the file and folder structure and performed some code refactoring to allow its isolation into a sepparated new project.

# Author/License

Original Media-Player FFmpeg based project is authored by

* Fabrice Bellard
* FFmpeg team

An fully refactored by

* Rubén Tomás (RTA)

# Original FFmpeg Core Information

https://docs.libretro.com/library/ffmpeg/
https://github.com/libretro/docs/blob/master/docs/library/ffmpeg.md

# Compilation
just execute `make` or `make DEBUG=1` if you want a more verbose execution

# IMPORTANT NOTE!!!
This core has been modified focusing on Raspberry Pi devices using a development version of RePlay OS, so it is not guarantee that it works in other systems or platforms (Linux only).

# Controls

* JOYPAD_START - play/pause
* JOYPAD_A - display progress
* JOYPAD_B - display media title
* JOYPAD_X - enable/disable video subtitles
* JOYPAD_Y - change video lang audio track
* JOYPAD_L - previous track (m3u)
* JOYPAD_R - next track (m3u)
* JOYPAD_LEFT - seek -15s
* JOYPAD_RIGHT - seek +15s
* JOYPAD_UP - seek +180s (3 min)
* JOYPAD_DOWN - seek -180s (3 min)
* JOYPAD_L2 - seek -300s (5 min)
* JOYPAD_R2 - seek +300s (5 min)

# Subtitles

If a video has an external subtitle file with the same name and a `.srt` extension, it will be loaded automatically.

# Changelog

# v2.2.0
- [X] Added support for external .srt files
- [X] Added option to disable subtitles
- [X] Removed unused hardware decoder path
- [X] Optimized software video path by eliminating an extra full-frame CPU copy (use video_buffer RGB output directly).
- [X] Improved GL upload performance by allocating textures once per size and updating via glTexSubImage2D instead of glTexImage2D each frame.
- [X] Reduced decode-thread allocation churn by reusing AVPacket and reusing a drain AVFrame on EAGAIN.
- [X] Fixed video_buffer ring-buffer head wraparound when returning an open slot (avoids negative modulus / potential stalls).

# v2.1.0
- [X] Changed POINT to BILINEAR scaling for better image quality
- [X] Fixed seek functionality
- [X] Fixed random crash when changing audio tracks

# v2.0.4
- [X] Reverted back changes made in v2.0.2 causing video issues

# v2.0.3
- [X] Added audio gain for videos having 6 channel (5.1) audio tracks

# v2.0.2
- [X] Fixed crash when seeking in many videos

# v2.0.1
- [X] Fixed crash when music contains embeded image information in GIF or BMP formats

# v2.0.0
- [X] Upgraded code base to make use of modern FFmpeg API
- [X] Upgraded code base to make use of modern libretro API v2
- [X] Added M3U support for creating playlists
- [X] Added new option to enable different loop modes (track, loop and shuffle)
- [X] Added compilation flag to enable/disable FFmpeg debug messages
- [X] Added ability to display video title on start (when available)
- [X] Added ability to display music title on start (when available)
- [X] Added ability to display video audio track title when changing
- [X] Added ability to display music or video title when pressing B (when available)
- [X] Added ability to display current progress time when pressing A
- [X] Added ability to display current progress % in addition to time stamps
- [X] Disabled audio and subtitle track change when playing music
- [X] Changed API shutdown request on media finish to blank screen / last frame
- [X] Changed video audio track button mapping from L1 to Y
- [X] Changed video subtitle track button mapping from R1 to X
- [X] Changed some option labels and categorized by Music and Video
- [X] Changed Audio Visualizer Resolution (FFT Resolution) option values:
    * 320x240 for 4:3
    * 320x180 for 16:9
- [X] Changed HW decoder default value to Auto
- [X] Changed seek time from 10s to 15s (left/right) and from 60s to 180s (up/down) and added new one 300s (L2/R2)
- [X] Changed audio track OSD message to also display the track name
- [X] Fixed Fast Fourier Transform (FFT) OpenGL bug preventing the frontend from being displayed
- [X] Fixed critical memory leak causing crashes randomly

# v1.0.0
- This version was based on the original media player from retroarch with some quick fixes

# TODO

- [ ] Find workaround for videos using non standard timings (i.e 288p PAL 60Hz) when played in CRT

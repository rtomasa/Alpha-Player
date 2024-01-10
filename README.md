# RePlay-Player

A port of the powerful audio/video encoding/decoding library FFmpeg to libretro. This core allows playback of a variety of audio and video formats, with a fancy audio visualizer and the ability to do interframe blending for smoother scrolling of non-native framerates.

# Important Note

This is a project based on libretro FFmpeg core used by Retroarch. Since the original core is included inside the main Retroarch project, it is not feasible to directly fork the same. I've reorganized the file and folder structure and performed some code refactoring to allow its isolation into a sepparated new project.

# Author/License

RePlay-Player FFmpeg based project is authored by

* Fabrice Bellard
* FFmpeg team
* Rubén Tomás (RTA)

# Original FFmpeg Core Information

https://docs.libretro.com/library/ffmpeg/
https://github.com/libretro/docs/blob/master/docs/library/ffmpeg.md

# Changelog

- [X] Added ability to display video title on start
- [X] Added ability to display music title on start
- [X] Added ability to display audio track title on change (L1)
- [X] Removed audio and subtitle track change for music (L1/R1)
- [X] Changed API core option v0 to v2
- [X] Changed some option labels and categorized by Music and Video
- [X] Changed Audio Visualizer Resolution (FFT Resolution) option values:
    * 320x240 for 4:3
    * 320x180 for 16:9
- [X] Changed HW decoder default value to Auto
- [X] Changed seek time from 10 to 15 (left/right) and from 60 to 180 (up/down)
- [X] Changed audio track OSD message to also display the track name
- [X] Fixed Fast Fourier Transform (FFT) bug preventing frontend from being displayed

<!--
## TODO
 - [ ] FFMPEG FORK
    - Controlar fin de video
    - Aspect ratio PIXEL PERFECT not working (need a new option for controlling in video)
    - Check HW h264 `decoders "ffmpeg -decoders | grep h264` -> h264_v4l2m2m
    
    - [ ] Use L2/R2 for next/last song/video in m3u
    - [ ] Use B to display internal title
    - [ ] Use A to display current time
    - [ ] Use Y to change audio track
    - [ ] Use X to display suntitles
    - [ ] Add M3U support
    - [ ] Add option to loop
    - [ ] Add option to set resolution (Native, PAL, NTSC, 240p)
    - [ ] Add suport for aspect ratios (via custom libretro command?)
    - [ ] Remove FFmpeg messages
-->
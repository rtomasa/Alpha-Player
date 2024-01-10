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

- [X] Added libretro API core option v2 support (removed v0)
- [X] Added ability to display video title on start
- [X] Added ability to display music title on start
- [X] Added ability to display audio track title on change
- [X] Added ability to display music or video title when pressing B
- [X] Added ability to display current progress when pressing A
- [X] Removed audio and subtitle track change when playing music
- [X] Changed audio track mapping button from L1 to Y
- [X] Changed subtitle track mapping button from R1 to X
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
    
    - [ ] Add M3U support
    - [ ] Use L2/R2 for next/last song/video in m3u
    - [ ] Add option to loop
    - [ ] Add option to set resolution (Native, PAL, NTSC, 240p)
    - [ ] Add suport for aspect ratios (via custom libretro command?)
    - [ ] Remove FFmpeg messages
    - [ ] Fix bug
        [dca @ 0x558a792630] Not a valid DCA frame
        ERROR] [LRCORE] [FFMPEG] Can't decode audio packet: Invalid data found when processing input
-->
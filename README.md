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

- [X] Changed from core option API v0 to v2
- [X] Changed FFT Resolution option to Audio Visualizer Resolution with lowres values. 320x240 for 4:3 and 320x180 for 16:9
- [X] Change seek time from 10 to 15 (left/right) and from 60 to 180 (up/down)
- [X] Fixed Fast Fourier Transform (FFT) breaking frontend OpenGL state

<!--
## TODO
 - [ ] FFMPEG FORK
    - Controlar fin de video
    - Mostrar mensajes SET_MESSAGE > RETRO_MESSAGE_TARGET_LOG > RETRO_MESSAGE_TYPE_NOTIFICATION
                       SET_MESSAGE > RETRO_MESSAGE_TARGET_OSD > RETRO_LOG_INFO
    - Introducir lista de resoluciones o ponerlas fijas
    - Compilar KO con make HAVE_CODEC_HW=0 OPENGL=1
    - Compilar KO con make HAVE_CODEC_HW=1 OPENGL=1
    - Compilar OK con make HAVE_CODEC_HW=0 OPENGL=0
    - Compilar OK con make HAVE_CODEC_HW=1 OPENGL=0
    - ifeq ($(OPENGL),1)
        HAVE_OPENGL = 1
        HAVE_GL_FFT := 1
        GL_LIB := -lGLESv2
        GLES = 1
        CFLAGS += -DHAVE_OPENGLES -DHAVE_OPENGLES3
    - Ver mapeos de mandos para play/pause (libretro docs)
    - Add support for Video4Linux (V4L2) which is the only supported HW decoder in FFmpeg and Raspberry
    - Aspect ratio PIXEL PERFECT not working (need a new option for controlling in video)
    - Check HW h264 `decoders "ffmpeg -decoders | grep h264` -> h264_v4l2m2m
    
    - [ ] Use L2/R2 for next/last song/video in m3u
    - [ ] Use B to display internal title
    - [ ] Use A to display current time
    - [ ] Use Y to change audio track
    - [ ] Use X to display suntitles
    - [ ] Show internal title on start
    - [ ] Show audio track name instead of track number
    - [ ] Add M3U support
    - [ ] Add option to loop
    - [ ] Add option to set resolution (Native, PAL, NTSC, 240p)
    - [ ] Add option to select shader filters (VHS, etc.)
    - [ ] Test video shaders
    - [ ] Test audio and visualizers
    - [ ] Set HW decoder to Auto by default
    - [ ] Add suport for aspect ratios (via custom libretro command?)
    - [ ] Fix menu not displayed on music visualizer
-->
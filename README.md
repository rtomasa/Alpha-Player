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
* JOYPAD_X - cycle video subtitle tracks/off
* JOYPAD_Y - change video lang audio track
* JOYPAD_L - previous track (m3u)
* JOYPAD_R - next track (m3u)
* JOYPAD_LEFT - seek -15s
* JOYPAD_RIGHT - seek +15s
* JOYPAD_UP - seek +180s (3 min)
* JOYPAD_DOWN - seek -180s (3 min)
* JOYPAD_L2 - seek -300s (5 min)
* JOYPAD_R2 - seek +300s (5 min)

# General Options

* Auto Resume - ON/OFF, stores the current position for supported seekable files on unload and resumes on the next load

# Audio Options

* Preferred Language - `Default`, `English`, `Japanese`, `Spanish`, `Spanish (Latin America)`, `French`, `German`, `Italian`, `Portuguese`, `Portuguese (Brazil)`, `Dutch`, `Russian`, `Ukrainian`, `Polish`, `Czech`, `Hungarian`, `Romanian`, `Turkish`, `Arabic`, `Hebrew`, `Hindi`, `Korean`, `Chinese (Simplified)`, `Chinese (Traditional)`, `Cantonese`, `Thai`, `Vietnamese`
* `Default` uses the file default audio track when flagged, otherwise the first audio track
* If the selected language is not available, playback falls back to the `Default` behavior

# MIDI Playback

Standard MIDI files (`.mid`, `.midi` and `.kar`) can be played through FluidSynth, the embedded fallback renderer or the frontend MIDI output.

* MIDI Output - `Default SoundFont`, `Roland SC-55`, `GM Roland`, `FluidR3 GM`, `UHD3` or `Frontend MIDI (Raw)`
* SoundFonts are searched below the frontend system directory in `scummvm/soundfonts`, `scummvm/extra` and the system directory root
* Missing selected SoundFonts automatically fall back to `Default SoundFont`
* FluidSynth is loaded dynamically when available; an embedded SF2 renderer is used as a fallback
* Raw mode forwards scheduled MIDI channel messages to `RETRO_ENVIRONMENT_GET_MIDI_INTERFACE`
* Raw mode does not require FluidSynth and uses the default SoundFont, when available, only to drive the FFT visualizer; synthesized PCM is muted
* MIDI supports play/pause, loop modes, M3U playlists, EOF handling and the FFT visualizer
* MIDI duration, seeking, auto-resume, arbitrary SoundFont file selection and time-based tempo-map operations are not supported

# Subtitle Options

* Subtitle Mode - `Off`, `Forced only`, `Preferred language`, `Always show preferred language`
* `Off` starts playback with subtitles disabled; manual subtitle cycling is still available
* `Forced only` selects a forced subtitle track, preferring the configured language when available
* `Preferred language` shows the preferred subtitle language unless the active audio track already matches it, in which case it shows forced subtitles only
* `Always show preferred language` shows the preferred subtitle language regardless of the active audio track
* Preferred Language - `Default`, `English`, `Spanish`, `Japanese`, `Spanish (Latin America)`, `French`, `German`, `Italian`, `Portuguese`, `Portuguese (Brazil)`, `Dutch`, `Russian`, `Ukrainian`, `Polish`, `Czech`, `Hungarian`, `Romanian`, `Turkish`, `Arabic`, `Hebrew`, `Hindi`, `Korean`, `Chinese (Simplified)`, `Chinese (Traditional)`, `Cantonese`, `Thai`, `Vietnamese`
* `Default` uses the file default subtitle track, a same-name external subtitle, or the first subtitle track

# Video Options

* Frame Blending - Off, Low, Medium, High or Full
* Zoom - `0.75x` to `1.35x` in `0.05x` increments
* Deinterlace - Off, `Auto`, `Always`
* `Auto` only deinterlaces frames marked as interlaced by FFmpeg and leaves progressive frames unchanged
* `Always` forces deinterlace on every decoded frame and is mainly intended for broken/misflagged sources
* Playback timing is deterministic: PAL-like video streams use `50 Hz`; all other content defaults to `60 Hz`
* Up to `1.00x`, zoom scales the image uniformly while preserving the source aspect
* Above `1.00x`, the player progressively crops toward the current frontend display aspect when `RETRO_ENVIRONMENT_GET_DISPLAY_INFO` is available, falling back to the viewport aspect only when display data is incomplete

# Subtitles

If a video has an external subtitle file with the same name and a `.srt` extension, it will be loaded automatically.

# Changelog

# v2.9.1
- [X] Fixed MIDI content failing to load when frontend raw MIDI output is selected but no MIDI device is available
- [X] Added fallback from unavailable frontend raw MIDI output to the default SoundFont renderer
- [X] Fixed audio-only seek timeouts causing MP3 playback to rush toward EOF
- [X] Changed media title OSD to display the file name instead of embedded metadata title

# v2.9.0
- [X] Added Standard MIDI file playback through dynamically loaded FluidSynth with an embedded fallback renderer
- [X] Added selectable RePlay SoundFonts and frontend raw MIDI output
- [X] Added automatic default SoundFont fallback when the selected SoundFont is unavailable
- [X] Added MIDI play/pause, loop modes, M3U playlist advancement, EOF handling and FFT visualization
- [X] Fixed music FFT visualization disappearing while playback is paused

# v2.8.0
- [X] Added EOF controls so media title and playback progress remain available after playback ends
- [X] Added rewind support from the ended state by reloading the current media and seeking back from the end
- [X] Fixed ended-state progress display to report the full duration and `100%`
- [X] Fixed video pause rendering so the cached frame remains visible instead of a black screen
- [X] Fixed repeated audio-only seeks leaving stale MP3 decoder/resampler timing state

# v2.7.0
- [X] Added `SUBTITLE MODE` and subtitle `PREFERRED LANGUAGE` options

# v2.6.0
- [X] Added perfromance improvements:
    - Moved RGB conversion to GL shader
    - Subtitles now render into a separate RGBA overlay texture
    - Added ARM64 NEON compositing for the CPU subtitle overlay
- [X] Fixed EOF playback shutdown stalls that could leave the frontend UI unresponsive
- [X] Fixed libretro reset after EOF to restart the current video from the beginning
- [X] Fixed DVD/VobSub subtitle packet handling that could hang playback on a black screen
- [X] Fixed the startup video stall path
- [X] Removed frontend target refresh timing dependency; PAL-like video streams now use 50 Hz and all other content defaults to 60 Hz

# v2.5.0
- [X] Added automatic `YADIF` deinterlacing for interlaced video
- [X] Added video `DEINTERLACE` option with `Off`, `Auto` and `Always`
- [X] Improved audio track switching to preserve video decode state and reduce sync jumps
- [X] Fixed loop mode playback stalls and restart timing near the end of media
- [X] Fixed seeking stalls and PTS resets after timeline jumps
- [X] Fixed some OSD notifications

# v2.4.0
- [X] Added new video `ZOOM` option
- [X] Added new `AUTO RESUME` option (when supported)
- [X] Added new `FRAME BLENDING` with configurable strength: Off, Low, Medium, High, Full
- [X] Added new `PREFERRED LANGUAGE` option to select the prefferred audio track
- [X] Added check for videos with no alternate audio tracks
- [X] Added fallback to display the file name when no metadata exists
- [X] Added `DISABLED SUBTITLES` virtual track cycling state
- [X] Added support for embedded VobSub/DVD subtitles
- [X] Improved support for SSA subtitles
- [X] Fixed controller port initialization so input works correctly on direct boot
- [X] Removed video thread decoder cores option
- [X] Removed subtitle enable/disable core option
- [X] Removed subtitle size option (breaks subs compatibility)
- [X] Removed subtitle font option and now honoring font metadata (fallback to `sans-serif`)

# v2.3.0
- [X] Refactored code to support libavutil >= 57

# v2.2.0
- [X] Added support for tracker music (s3m|it|xm|mod)
- [X] Added support for external .srt files
- [X] Added option to disable subtitles
- [X] Added option to disable visualizer
- [X] Replaced FFT visualizer
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

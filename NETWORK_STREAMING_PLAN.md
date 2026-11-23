# Alpha Player network streaming plan

Status: proposed implementation; no player changes made by this document.

Issue: https://github.com/rtomasa/RePlay-issues/issues/493

## 1. Objective and scope

Allow a local channel playlist to mix local files and network streams. Users load
the playlist through the existing frontend or REST flow, then use L/R to change
entries and B to display the current entry's name.

Deliver all seven discussed requirements:

1. Play HTTP/HTTPS URLs, including HLS stream URLs, from M3U playlists.
2. Use L/R to navigate the URLs/files inside that channel playlist.
3. Use optional outer `#EXTINF` titles for channel names.
4. Disable unsupported seeking, hide live progress, and skip position resume for
   live streams without a meaningful duration.
5. Bound network connection, probing, and stalled-read waits.
6. Show actionable OSD messages for unavailable streams.
7. Skip failed entries with bounded attempts, keeping channel navigation usable.

UDP URLs are secondary, conditional on the linked FFmpeg build. Handle their
addresses correctly and use protocol-appropriate timeout behavior, but do not
delay the HTTP/HTTPS/HLS milestone for multicast or other advanced UDP setup.

## 2. Terminology and intended use cases

- **M3U channel list:** the list Alpha Player navigates. Each entry represents a
  complete local media item or network channel.
- **HLS:** HTTP Live Streaming. An HLS manifest, commonly `.m3u8`, identifies
  media segments and possibly alternative quality streams. FFmpeg handles these
  internal playlists and segments as one playable item. HLS supports both live
  broadcasts and on-demand content; a network URL alone does not imply live TV.
- **MPEG-TS:** MPEG Transport Stream, a container carrying audio, video, and
  timing. It can contain H.264/AAC and is not limited to MPEG-2 video. It can be
  delivered continuously over HTTP/UDP or used for individual HLS segments.
- **Reported use case:** Dispatcharr sends continuous MPEG-TS over an HTTP URL
  with no useful filename extension. HLS is an additional supported input type.

Applicable home-server sources include ErsatzTV for scheduled library channels,
Owncast for livestreaming, MediaMTX for relaying streams, and FFmpeg plus a web
server for custom HLS output. Jellyfin HLS URLs may also work when the caller
supplies a usable authenticated playback URL; session management is separate.

This feature does not implement YouTube webpage resolution, Jellyfin/Plex library
browsing or login, DRM, EPG/XMLTV, channel logos/groups, or a server/encoder inside
Alpha Player. It consumes streams supplied by those systems.

## 3. Current implementation and affected code

Primary implementation file: `ffmpeg_core.c`.

- `playlist`, `playlist_count`, `playlist_index`: store paths only; no titles.
- `parse_m3u_playlist()`: prefixes every entry with the playlist directory and
  requires `access(..., R_OK)`. This rejects URLs and mishandles absolute paths.
- `aplayer_path_is_m3u()` / `retro_get_system_info()`: currently recognize `.m3u`
  as the outer playlist extension; `.m3u8` needs explicit classification.
- `retro_load_game()`: opens/probes synchronously through FFmpeg without network
  options or an interrupt callback.
- `display_media_title()` and L/R OSD: derive names from filenames.
- `seek_frame()`, `dispaly_time()`, bookmark helpers: already have some duration
  guards, but duration alone is not sufficient to determine seekability.
- Decode loop: every negative `av_read_frame()` result currently becomes EOF.
- `retro_unload_game()`: signals decoder shutdown and joins its thread; signals
  alone cannot interrupt a blocked FFmpeg network read.
- Existing playlist retry logic uses internal reload flags consumed by
  `retro_load_game()`. Preserve playlist context explicitly across failed loads.
- Fix the existing open-input error log's two `%s` placeholders with one argument
  while implementing network diagnostics.

## 4. Playlist representation and parsing

Introduce an entry structure containing the source string, optional display
title, and source kind (local or supported network protocol). Keep the existing
256-entry limit for this milestone and report when additional entries are ignored.
Update all navigation, bookmark matching, shuffle, and reload uses consistently.

Parsing rules:

- Resolve relative local paths against the playlist directory.
- Preserve absolute local paths; retain local readability checks.
- Recognize supported schemes at the start of the entry, case-insensitively.
  Preserve URLs, query parameters, and escaping exactly; skip filesystem checks
  for them. Reject unsupported schemes with a clear diagnostic.
- Accept LF/CRLF and a UTF-8 BOM. Ignore blank lines and comments. Detect overlong
  lines and reject the entire entry instead of accepting a truncated URL/path.
- Capture `#EXTINF` metadata for the following media entry. Find the first comma
  outside quoted attributes and preserve the complete following title, including
  any commas in that title. Clear pending metadata after consuming or rejecting
  its entry so it cannot attach to a later channel.
- Treat `-1` as unknown declared duration. Do not use it as the sole authority
  for live/seek capabilities; actual input properties take precedence.
- Additional `tvg-id`, `tvg-name`, `group-title`, and logo attributes may be
  ignored. Their presence must not break title parsing.

Example outer playlist:

```m3u
#EXTM3U
#EXTINF:-1 tvg-id="news" group-title="TV",News
https://server.example/news/live.m3u8
#EXTINF:-1,Cartoons
http://192.168.1.20:9191/proxy/ts/stream/channel-id
#EXTINF:120,Local clip
../videos/clip.mp4
```

## 5. HLS handling and extension classification

Do not feed HLS segment or variant lists into Alpha Player's channel parser.
Pass HLS input to `avformat_open_input()` and let FFmpeg manage it.

- A URL inside an outer playlist goes directly to FFmpeg, regardless of extension.
- Classify URLs before applying local extension-based handling; query strings
  must not turn a stream into a local playlist or MIDI input.
- Add `.m3u8` to advertised content extensions. For local playlist files, inspect
  HLS `#EXT-X-` tags: an HLS manifest goes to FFmpeg; an ordinary channel list
  goes to the channel parser. Apply this distinction to `.m3u` too, since the
  extension alone does not define the playlist's meaning.
- Local HLS manifests can reference remote segments. Network limits and
  cancellation must cover nested FFmpeg requests as well as top-level URLs.
- Never present HLS segments or quality variants as separate L/R channels.

## 6. Network opening, cancellation, and thread ownership

Allocate an `AVFormatContext` explicitly and install `AVIOInterruptCB` before
opening. Give each load a cancellation token and monotonic deadline, separate
from `decode_thread_dead`, whose current lifecycle is tied to active decoding.
Use proper synchronization for state shared with the callback.

Proposed initial limits, implemented as named constants:

- 10 seconds total for opening plus stream probing of one network candidate.
- 10 seconds without successful network read progress before declaring a stall.
- Cancellation requested immediately on channel change or unload.

Set `rw_timeout` where applicable and use protocol-specific options where needed.
Use interrupt deadlines to cover overall open/probe work; a socket timeout alone
does not bound all probing or HLS retries. Refresh read deadlines around active
read operations, not while paused or blocked by a full decoded-data queue.
Verify nested HTTP/HLS requests inherit the intended behavior. Audit resolver and
TLS behavior on the shipped FFmpeg build before claiming a strict wall-clock
bound for every possible failure.

Initial `retro_load_game()` may remain synchronous with a bounded total startup
budget, since the frontend needs a success/failure result. Use a 10-second total
budget across initial candidates, rather than 10 seconds multiplied by 256.
If exhausted, report failure clearly and return false.

During playback, perform network open/probe for channel switching in a background
loader. Keep the frontend loop available to poll input, display loading/error
state, and cancel or replace the pending selection. Stage candidate FFmpeg state
separately; publish it only after success. Keep frontend environment callbacks,
OSD, GL setup, and final playback-state installation on the frontend thread.

On unload, cancel pending loading and active reads, wake FIFO/video waiters, join
threads, then release their contexts. Never free state still used by an interrupt
callback or join a worker while holding a lock it needs to finish.

## 7. Names, live controls, and resume

Use one title helper for initial load, B, channel switching, and error messages:
outer `#EXTINF` title first, then useful stream metadata for network media, then
a safe URL basename/host fallback. Preserve current filename behavior for local
files without an explicit playlist title. Do not expose URL credentials or query
tokens in fallback titles or diagnostics.

Determine duration validity, seekability, and live behavior separately. Combine
FFmpeg input/demuxer capabilities, meaningful duration, and known live/HLS state;
do not rely solely on duration or `fctx->pb->seekable`, since HLS manages nested
inputs and can have a sliding duration window.

- For live/unknown-duration streams: disable time seeking and position resume.
  A should display `Live` and optionally elapsed playback time as a notification,
  with no percentage or progress-bar message.
- B and L/R remain available, including after a failed channel or ended stream.
  Move navigation outside the current `!decode_thread_dead` gate as needed.
- Seekable on-demand streams may retain progress, seeking, and resume.
- Preserve existing local-file, MIDI, and game-music restrictions.
- A stale playlist bookmark must not seek a newly live entry. Restoring a channel
  selection can remain independent from restoring a playback timestamp; live
  timestamp saving/restoration stays disabled.
- Live pause is a local pause with bounded buffers, not a DVR promise. On resume,
  reconnect to the live edge if the session/window is no longer usable. Do not
  treat deliberate pause/backpressure as network failure or accumulate media
  indefinitely. Local-file pause behavior remains unchanged.

## 8. Failure handling and channel navigation

Use an explicit result/state for loading, playing, switching, failed, and ended;
do not represent every network error as ordinary EOF. Distinguish EOF, temporary
`EAGAIN`, cancellation, timeout, and permanent open/read errors.

- Show `Connecting: <name>` during network channel changes.
- Show `Stream unavailable: <name>` or `Stream timed out: <name>` on failure;
  retain FFmpeg error details in sanitized logs.
- On failure, try another channel with a visited-entry set and finite attempt
  limit. R and forward advancement move forward; L moves backward. Respect
  Play Once boundaries, wrapping modes, and Shuffle without retrying the same
  failed candidate indefinitely. Repeat Track must not trap the user on a dead
  channel; failure skipping overrides repetition for the current attempt.
- Use the same candidate-selection logic for initial load, manual switching,
  automatic advancement, and runtime stream failure, with the startup budget
  described above. A new user selection cancels the previous attempt sequence.
- After all eligible entries fail, show `No playable channels` and retain an idle
  playlist state for L/R retry and exit if content was already loaded. Initial
  startup failure instead returns false after its error notification.
- Do not add unlimited reconnect loops. Explicit user cancellation must not be
  reported as a stream failure or trigger automatic skipping.

## 9. Implementation order

1. Add playlist entry/title representation, URL classification, robust parsing,
   and HLS-versus-channel-list detection. Preserve local playlist behavior.
2. Add interruptible FFmpeg opening/probing/reads, cancellation ownership,
   timeout diagnostics, and safe cleanup, including nested HLS requests.
3. Replace fragile reload retries with shared candidate-selection logic and add
   cancellable background network switching plus frontend loading/error states.
4. Add shared title display, live progress/seek/resume rules, and live pause policy.
5. Verify shipped FFmpeg protocol/demuxer support and document playlist examples,
   limits, supported sources, and controls in `README.md`.

The default build links system FFmpeg. Verify the actual RePlay/Pi libraries for
HTTP, HTTPS/TLS, TCP, HLS demuxing, and the requested codecs; UDP is conditional.
The optional bundled configuration currently disables HTTPS, so update and verify
its TLS dependency configuration before advertising HTTPS for that build.

## 10. Focused validation and completion criteria

Keep validation limited to the changed behavior; no broad new test framework.

- Small parser checks: relative/absolute local paths; mixed URLs/local files;
  BOM/CRLF; missing titles; quoted commas and title commas; query strings;
  overlong entries; local HLS versus ordinary M3U/M3U8 channel lists.
- A controlled LAN source: continuous MPEG-TS over HTTP without an extension,
  HLS live, and HLS on-demand. Include a local HLS manifest with remote segments.
- An unreachable/stalled endpoint: bounded initial load, readable OSD, working
  L/R during channel connection, and cancellation during reads and exit.
- Mixed good/bad channels: broken first entry, mid-playback disconnect, all failed,
  backward skipping, Play Once boundaries, wrapping, Shuffle, and Repeat Track.
- Live controls: title, no progress bar, disabled time seek, no timestamp resume,
  and bounded pause/resume behavior. Confirm on-demand seek/resume still works.
- Compile with the normal project configuration and perform final playback checks
  on Raspberry Pi 5/RePlayOS, the reporter's target environment.

Complete when the issue's HTTP MPEG-TS example and an HLS channel both work through
the existing playlist flow, titles and L/R work, failures are bounded and visible,
and local media playback remains functional. HLS support does not imply seamless
adaptive bitrate switching or guaranteed playback of every service's protected
or session-dependent URLs.

## References

- HLS structure and MPEG-TS segments: https://datatracker.ietf.org/doc/html/rfc8216
- FFmpeg network options: https://ffmpeg.org/ffmpeg-protocols.html
- FFmpeg interruption: https://ffmpeg.org/doxygen/trunk/structAVIOInterruptCB.html
- FFmpeg HLS generation: https://ffmpeg.org/ffmpeg-formats.html#hls
- ErsatzTV channels: https://ersatztv.org/docs/channels/
- Owncast external playback: https://owncast.online/docs/watching-streams/
- MediaMTX: https://mediamtx.org/docs/features/read
- Jellyfin playback: https://jellyfin.org/docs/general/clients/codec-support/

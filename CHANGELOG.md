# Changelog

## [Unreleased]

### Changed

- **Left no longer jumps to the sidebar from inside a tab.** The sidebar sits left of
  everything, so a stick drifting left at any depth landed on a sidebar item — and
  merely *focusing* one switches the tab (`AutoSidebarItem::setActive` fires the
  switch callback), which destroyed the whole detail stack: series and season pages
  gone, back to the library root, and pressing right did not bring them back. Left is
  now a wall inside the content (symmetric to the existing "no down" guard); the
  sidebar is reached with **B**, which walks up one page at a time and lands there at
  the top.
- **Holding B retraces the whole stack.** The back action now repeats (the platform's
  250 ms trigger, then every 100 ms), so one hold from a season page ends with the
  sidebar focused. Popped pages are the ones already in memory, so nothing is
  refetched on the way.
- **Settings → Playback is one mode instead of four knobs.** A single three-way
  choice — *Auto* (direct, transcode only above the limit), *Always direct*,
  *Always transcode* — replaces "force direct play" and "also limit direct play",
  which gated each other and left "maximum transcoding resolution" doing nothing on
  its own. The resolution limit is now that mode's parameter (a threshold under
  Auto, the output height under Transcode) and is hidden in Always direct, together
  with the codec row.
- **The quality tier picked in the player is remembered per title** — the whole show,
  for an episode. A resume, the next episode, or a fresh open from the library all
  play the way it was chosen. It used to be one global value in `config.json`, which
  turned every later video into a transcode; the override now lives in
  `playback_quality_overrides`, keyed by connection + title id, and picking "Auto"
  forgets it. The store keeps itself in order: an entry records the title it was
  chosen for (a recycled Plex ratingKey is dropped instead of being applied to a
  different film), entries of a deleted connection are pruned, and the map is capped
  at 200 — the oldest write leaves first.
- **The codec row is wired**: H.264 or HEVC reaches the transcoder (`opts.videoCodec`
  was hard-coded to h264). AV1 is gone — the console has no AV1 decoder, so choosing
  it only ever meant software decoding.
- **Cloud files are shown as a read-only row** in that group: "always direct", with
  a dialog explaining why (the server has no encoder for a file it would first have
  to pull through itself). The rule was previously invisible — only a hidden menu
  and a toast.

### Fixed

- **A refused direct play is retried once through PlaybackInfo.** Some servers never
  flag a `.strm` source as remote, so the player went straight to
  `/Videos/{id}/stream` and gave up when the server refused it. The error path now
  asks the server for the URL it hands out (`PlaybackOptions::forcePlaybackInfo`),
  one attempt per load and only for Jellyfin/Emby — the first try still costs no
  extra request.
- **A cloud download now fetches the file from its own URL.** When PlaybackInfo
  handed out no `DirectStreamUrl` the download fell back to `/Videos/{id}/stream`,
  i.e. the server pulling the cloud file through itself — the same 20 s
  `HttpClient` timeout that produced the playback 500. `sourceUrls()` now returns
  the media source's own absolute `Path` next to `DirectStreamUrl`, and the download
  uses it before any proxy attempt (no token appended: it is not the server).
- **The download button respects the account.** Jellyfin/Emby report
  `Policy.EnableContentDownloading` at login; it is stored per profile (default
  true, so an older config never hides a working button) and gates the three places
  that show the button. Courtesy rather than enforcement — our download reads the
  playback stream, which the server's download policy does not cover.
- Home's "Continue watching" row now refetches when a playback ends. Tabs are
  cached (`AutoSidebarItem::getAttachedView()` hands back the same view on every
  re-attach) and nothing re-requested on re-attach, so the row kept the answer it had
  when the tab was built: a film just watched was missing from it until a manual
  refresh or a restart. The player now fires `playbackEndedEvent()` when it closes and
  the row refetches itself — even while the home tab is off screen.
- The player announces the session before the first bytes
  (`POST /Sessions/Playing` on Jellyfin/Emby). Progress reports already stored the
  position on stop, which is why playback looked fine, but the server never listed
  this device under "now playing" and its progress had no session behind it.
- A cloud source whose gateway sits on the LAN now plays directly. The rule that
  drops a private `DirectStreamUrl` ("unreachable from a console elsewhere") also
  dropped it when the server itself was on that LAN — the normal setup for a 115/123
  front-end — so playback fell back to `/Videos/{id}/stream`, i.e. the server
  fetching the cloud file through itself. Emby gives up after its 20 s HttpClient
  timeout and answers **HTTP 500**. A private URL is now accepted when the server is
  private too, and still refused when the console is genuinely elsewhere.
- A `.strm`-backed source that Emby does not flag `IsRemote` is now recognised by
  its MediaSource `Path` (an absolute http(s) URL), and that URL is used directly
  when PlaybackInfo hands out no `DirectStreamUrl` — or when the one it hands out
  points back at the server, which is the same proxy under another name. Instead of
  the server's proxy, in both cases.
- A cloud source (`.strm` behind a CDN) is no longer sent to the transcoder when its
  direct play fails. The recovery path retried every failed direct play at 8 Mbps
  regardless of the source, so a CDN-backed item — which has no encoder behind it —
  replaced its real error (a 403/404 from the CDN, the one worth reading) with a
  server-side HTTP 500 from a URL that never had a chance, after the seconds the
  re-resolve and reconnect cost. Direct play is now the only path for a remote
  source, and the dialog shows the first error. A one-line log per attempt
  (`playback <id>: method=… remote=… cap=… maxHeight=…`) makes the three paths
  distinguishable in a user's log.

### Added

- The player's OSD title now ends with the path it took — `· Direct` or
  `· Transcode 8 Mbps` — so "why is this one slower?" no longer needs a log. The
  badge follows a quality switch, which reloads through the same code.
- The account policy is re-read at launch (`/Users/{id}`), so revoking a user's
  download permission now hides the button without a re-login. A failed refresh
  keeps the stored value: a network blip must not hide a working button.
- A series page now says what Play will do and which episode it means: the button
  reads *Resume* when it continues a half-watched episode (and *Play again* after the
  show is finished), with `S8E9 · title` under it. The data was already fetched for
  that button (`getNextUp`); it simply was not shown.
- Home-video libraries. Emby's "Photos & videos" library (`CollectionType:
  homevideos`) and Jellyfin's "Home videos" now get a sidebar tab, list their
  videos and play one on **A** like a clip — no detail page, because a home video
  has no overview and no seasons. Its videos also reach *Continue Watching* and the
  per-library "Recently added" home row. The **photos** in such a library are not
  shown: the grid asks the server for `IncludeItemTypes=Video` only, since there is
  no photo viewer yet.

### Fixed

- Settings → Others no longer carries three switches that do nothing. "Fullscreen",
  "Always on top" and "Single instance" lost their handlers when the desktop code
  path was removed, but their cells stayed in the XML: three untitled toggles that
  changed nothing (two more sat hidden in the in-player settings). The cells, their
  bindings, the settings behind them and their keys in all 14 locales are gone, and
  so are the F11 fullscreen shortcut and the IPC-socket option that only existed to
  serve them.

### Removed

- The last non-Switch branches in the player core: the PS Vita, PS4, Android,
  desktop, GXM and D3D11 arms of `mpv_core.cpp` / `mpv_core.hpp`, with the PS4-only
  HDR re-tag filter (`view/hdr_options.hpp`), its subtitle-font installer
  (`utils/default_font.hpp`), their tests and the empty `misc::initCrashDump()`
  stub the D3D11 arm called. What remains is what this tree actually builds:
  deko3d, the OpenGL fallback (GLFW or SDL2) and the `MPV_SW_RENDER` CPU path. No
  behaviour change on the console.
- The dead platform code left outside the player core: the PS Vita texture path in
  the artwork loader (its power-of-two downscale, DXT compression and the offline
  `withLocal` entry point), the Vita curl/mbedtls memory pool in the HTTP client,
  and the "D3D11"/"GXM" driver names in About. Artwork is uploaded at the size the
  server returned — which is what the console already did, since that cap existed
  for the Vita's GPU memory.
- The bundled PS4 subtitle font (718 KB), unreferenced since that installer was
  removed.

## [0.1.0] — 2026-11-21

First public release.

### Networking

- **Two addresses per Jellyfin/Emby connection.** An optional home address
  (`AppServer::local_url`) is probed before the remote one; Settings → Network
  picks the policy (`lan_first` / `lan_only` / `race`). The top-right badge shows
  LAN or WAN and its dialog (**ZR**) can re-probe or force the other address.
- **Home-address probe budget.** Settings → Network now sets how long the home
  address gets before the tunnel is used (1.5 s – 5 s, default 2 s), so a slow
  home Wi-Fi is not read as "away" and does not fall through to the metered
  address. The sign-in form probes with the same value.
- **WAN library allow-list.** Settings → Libraries gained an **X** column that
  decides, per library, whether it exists on the metered address; the Jellyfin/Emby
  backend then queries only the visible libraries. Playback is guarded too: an item
  from a hidden library is refused before it reaches mpv.
- **Downloads obey the allow-list.** Queueing an item whose library is hidden on the
  metered address is refused before anything is written, and the check runs again
  when a transfer starts — the address and the allow-list can change while an item
  waits, and the queue even outlives a restart. A download spends the whole file
  rather than the few minutes a playback costs, so it asks the player's rule
  (`utils/wan_guard.hpp`), not a second one of its own: a library allowed on WAN
  downloads as before, and a season refused episode by episode says so once.
- **Music obeys the allow-list too.** The audio player had no guard at all: it
  handed mpv a resolved track URL whatever the address was, so an album from a
  hidden library streamed on the metered one. It now asks the same rule before
  `setUrl`, and a refusal drops the queue instead of leaving it loaded — that queue
  would advance at the end of the track and be refused again, one track at a time,
  with silence in between.
- **Favorites and playlists obey the allow-list.** Neither endpoint takes a library
  scope, so their pages are filtered as they arrive: a movie or a show names its
  library in `ParentId`, an episode is resolved through its show once per page, and
  a page that is entirely hidden is skipped instead of ending the list early.
- **Edit a connection in place** (Y on a connection tile): addresses and
  credentials without delete-and-re-add, so the allow-list and the sidebar layout
  survive. An unchanged username with a blank password only rewrites the addresses
  and keeps the stored token.

### Fixed

- Installing the HOME tile now verifies what it wrote instead of trusting the write.
  The room for this bug was structural: the tile's artwork lives in the control NCA
  the console ends up with, so "InstallSD returned true" said nothing about the
  icon. `forwarder::install()` reads this build's icon out of the running NRO's ASET
  section, installs, reads the installed title's control data back and compares the
  two; `main/hints/icon_stale` reports a console that kept other artwork instead of
  launching a tile that will look wrong again. The forwarder NACP also stopped being
  stamped `0.1.0` for every build — the HOME menu keys its icon cache on the title's
  version, so each build now carries the git build count.
- Downloading a cloud source works again. The transfer used to assemble
  `/Videos/{id}/stream`, which the front-ends serving those sources refuse (403,
  missing user context); it now asks PlaybackInfo for the link the server actually
  hands out, the way direct play does. A download whose link leaves the server host
  no longer carries the server's auth headers — a CDN has no business receiving them.
- A cloud source playing behind a 302 front-end is now always played as-is. On the
  Switch the 1080p resolution limit derived a bitrate cap for any 4K item, which
  routed it into a server-side transcode — the server pulling the whole cloud file
  and re-encoding it, i.e. exactly what that setup cannot do, and by default. A
  remote source now ignores the stored cap and the resolution limit (only the
  failed-direct-play rescue may still ask for one), and its quality menu is gone:
  every tier of it means "transcode", and there is no encoder on the other side.
- Music no longer outlives the connection it came from. The queue held the old
  server's rating keys and a stream URL resolved against its address, and switching
  offered "next" for a library the user had just left; it is now dropped wherever
  the active connection changes — switching, signing in, editing the connection in
  use, deleting it, reconnecting from the offline shell. A queue of local files is
  left alone.
- A library's sort and filter no longer leak between servers, or into the next
  library. The stored preference was keyed by the section id alone and Plex numbers
  sections 1, 2, 3…, so two servers shared one entry; the key now carries the
  connection, existing entries are adopted once, switching no longer wipes
  everyone's preferences, and a library with no stored preference starts from the
  defaults instead of the selection the last library left behind.
- A cached poster can no longer answer for another server. Artwork is keyed by the
  connection that fetched it, since a relative path is that server's own id; offline
  the cache still accepts a copy from another connection rather than showing a blank
  poster, because nothing can be re-fetched there.
- A queued download no longer resumes against whichever connection is active. Each
  item is stamped with the server and profile that queued it, and a transfer only
  starts under that connection: asking another server for a part key it does not
  have is a 404 at best and, where part ids collide, a different file. The row reads
  "Waiting for its server" until the connection comes back, and switching to it
  resumes the queue.
- Adding a second account on a server that already exists no longer drops the home
  address typed on that form: the address lives on the server entry, so it is
  adopted there. The sign-in form now says the addresses belong to the server, which
  is why two accounts on one server always show the same pair.
- Installing the HOME tile (Settings → Home menu icon) now reports a failure
  instead of showing "done": the install result was ignored and the title ID read
  afterwards was undefined, so a failed install launched the previously installed
  tile and left its old icon on screen.
- Streams from a cloud-drive front-end (TgtoDrive and similar) behind a 302 play
  through the server's `PlaybackInfo` URL instead of an assembled one.
- The app and mpv send the same User-Agent; the CDN signature binds the two.

## [0.1.0] - 2026-11-16

First SMCA release. SMCA (Switch Media Center Aggregator) is a Nintendo Switch
client forked from [GMCA](https://github.com/thcolin/gamepad-media-center-aggregator)
by thcolin (Apache-2.0), rebuilt as a Switch-only project.

### Servers

- Plex, Jellyfin, Emby and Stremio behind one backend interface, switchable from
  the connection screen; per-backend accent colour and tabs.

### Playback

- libmpv with hardware decoding (deko3d), direct play with automatic transcode,
  resume positions, audio/subtitle track selection, speed and aspect controls.
- Remote (`.strm`) sources on a cloud drive behind a 302 front-end (TgtoDrive and
  similar) now play through the `DirectStreamUrl` the server returns in
  `PlaybackInfo`, instead of a URL the client assembles itself.
- One User-Agent for the whole app — the HTTP client and the player send the same
  string, which is what such front-ends sign the playback URL against. Settable in
  Settings → Network or with `user-agent=` in `mpv.conf`.

### Library and files

- Offline downloads with a local catalog, SD card / WebDAV / HTTP / USB (UMS)
  browsing, subtitles (embedded and sidecar), 14 UI languages.

### Removed

- Every non-Switch platform of the upstream project (PS Vita, PS4, Windows, macOS,
  Linux, Android), their packaging and their CI jobs: the build now refuses to
  configure anything but `PLATFORM_SWITCH`.

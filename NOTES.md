# radioglobe project notes

An interactive internet-radio globe for 9front, inspired by
radio.garden.  Orthographic globe rendered with libdraw; spin
with the mouse, click a station dot to stream it through
play(1) to /dev/audio.

## Status

- Core program builds and runs: globe projection, rotation,
  zoom, station dots, hover labels, status bar, audio start/stop.
- Audio now runs an rc pipeline (not bare play):
      play -o /fd/1 URL </dev/null | audio/pcmconv -o s16c2rRATE >/dev/audio
  forked with RFFDG|RFNOTEG|RFNOWAIT.  play still does fetch +
  format detect + decode; pcmconv resamples to the device rate.
- FIXED "sped up audio": decoders/your aacdec can emit 48000Hz;
  writing that straight to a 44100Hz /dev/audio plays ~9% fast.
  pcmconv -o s16c2r<devrate> forces the rate so it matches.
  devrate defaults to 44100; override with `radioglobe -r 48000`
  if /dev/audio actually runs at 48k.
- AAC kept: mkstations now emits AAC and AAC+ (you installed an
  aacdec).  HLS (.m3u8) still filtered - play/aacdec can't demux it.
- Coastlines: runtime-loaded coast.dat from Natural Earth via
  mkcoast.  Looks much better than the old hand-typed data.
- Source lives in /usr/dave/work/radioglobe (a stray /usr/dave/src
  from earlier should be removed with `rm -r`).

### Next time / TODO shortlist
- Confirm the pcmconv fix actually cured the speedup on real
  streams (esp. 48k AAC).  If some still sound off, check whether
  /dev/audio is at 48000 and try `radioglobe -r 48000`.
- Consider routing through mixfs instead of pcmconv: it auto-
  resamples to the device output AND allows multiple streams.
- De-dup stations stacked on identical coordinates (many
  SomaFM/WALM/0N entries share one lat/lon -> overlapping dots).
- ICY now-playing metadata (zuke has icy.c to borrow).

## Audio device rate (how to find / set it)

The sample rate is owned by the audio driver
(/sys/src/9/port/devaudio.c) and exposed through the audio(3)
device, NOT by play/pcmconv.  See audio(3): /dev/volume is the
control file; each "source" is a line of "source value" (or
"source left right").  The relevant source is `speed` -- the
device sampling frequency in Hz.

Find the current device rate:

    grep speed /dev/volume
    # e.g.  speed 44100      (or 48000)

If /dev/volume has no `speed` line, the device only runs at its
fixed default (44.1 kHz per audio(3)); some USB DACs expose more.

Set it (writes go to the same file, same format):

    echo speed 48000 >/dev/volume

Other useful sources you may see there:
    audio left right   output volume (D/A), 0..100
    delay value        output buffering, in samples (latency)
    (tone controls, etc. -- driver dependent)

How this ties into radioglobe:
- The decoders (and your aacdec) emit PCM at some rate; pcmconv
  in our pipeline forces that to s16c2r<devrate> so it matches
  whatever the DEVICE is set to.
- So the rule is: read `speed` from /dev/volume, then run
  radioglobe -r <that number>.  Default is 44100, which is right
  for the stock device.  If you `echo speed 48000 >/dev/volume`,
  also pass `-r 48000` (or vice-versa) so the two agree.
- Mismatch symptom: pcmconv target > device speed  => slow/low;
  pcmconv target < device speed  => fast/high (the "sped up"
  bug).  Equal => correct pitch.

To inspect what a station actually decodes to (debugging):

    hget URL | audio/mp3dec | audio/pcmconv -i s16c2r44100 \
        -o s16c2r44100 >/dev/null   # adjust -i if you know it
    # or just listen with explicit rate:
    hget URL | audio/aacdec | audio/pcmconv -o s16c2r48000 >/dev/audio

(That last line is the manual equivalent of what radioglobe does;
it is also what /usr/dave/audionotes and brennpunkt already used.)

mixfs alternative: `audio/mixfs` binds over /dev/audio and
"resamples incoming audio to the format of the audio device
output if it does not match the default (s16c2r44100)".  If we
route through mixfs we would not need -r at all -- mixfs reads
the device rate itself and resamples to it, and also allows
multiple simultaneous streams.  (See TODO.)

## Current work: better data

Two problems being fixed:

1. Coastlines were hand-typed (~600 points) and look bad.
2. Station list was hand-picked and is mediocre / partly stale.

Plan: replace both with real datasets converted by small
libjson-based tools, loaded at runtime from data files.

### Coastlines

Source: Natural Earth, via the nvkelso GeoJSON mirror.
Public domain.  Start with 110m (coarse, clean), can move to
50m for more detail.

Fetch (run yourself; the agent has no network):

    hget https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_110m_coastline.geojson > /tmp/coast.geojson

Higher detail (optional, larger):

    hget https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_50m_coastline.geojson > /tmp/coast.geojson

GeoJSON coordinate order is [lon, lat] (x,y) -- the REVERSE of
our Geo{lat,lon}.  The coastline theme is a FeatureCollection
of LineString / MultiLineString features.

Converter: mkcoast.c  (uses libjson)
    mkcoast < /tmp/coast.geojson > coast.dat

### coast.dat format (text)

One polyline per block.  A header line "> N" gives the point
count, followed by N lines of "lat lon".  Blank line / EOF
ends.  Comments start with #.  Example:

    > 3
    51.2 -0.3
    51.5 -0.1
    51.9  0.2

coastinit() loads coast.dat at runtime (searched in . then
/lib/radio/coast.dat).

### Stations

Source: radio-browser.info -- free open community radio DB,
public JSON API, no key.  Closest open analog to whatever
radio.garden curates.

Fetch the top working geolocated stations (run yourself):

    hget 'https://de1.api.radio-browser.info/json/stations/search?limit=1000&has_geo_info=true&hidebroken=true&order=clickcount&reverse=true' > /tmp/stations.json

API notes:
- url_resolved is better than url (redirects/playlists already
  followed) -- prefer it.
- codec field: keep MP3, OGG/VORBIS, and AAC/AAC+ (aacdec is
  installed).  HLS (.m3u8) is still filtered out by default - it
  is a segmented playlist, not a raw stream, so neither play nor
  aacdec can demux it.  `-a` disables all codec/HLS filtering.
- GOTCHA: the longitude field is "geo_long", NOT "geo_lon".
  Using the wrong name silently drops every station (lon falls
  back to the out-of-range sentinel and the row is filtered).
  Latitude is "geo_lat".
- hls=1 and any ".m3u8" url => HLS; skipped by default (the
  9front decoders don't do HLS).
- name can contain leading tabs/spaces, embedded quotes, control
  chars -- mkstations sanitizes (collapses whitespace, turns " into ').

Converter: mkstations.c  (uses libjson, bio)
    mkstations < /tmp/stations.json > stations

mkstations flags:
    -a            keep ALL codecs and HLS (no filtering)
    -b N          require bitrate >= N kbps

Verified against a real radio-browser dump 2026-06: the data uses
geo_long; with that fix mkstations emits the MP3/OGG stations fine.

Writes our station-file format:
    lat lon "name" url
one per line.

## Files

    main.c        event loop, UI, station mgmt, audio
    globe.c       orthographic projection + rendering
    coast.c       loads coast.dat at runtime
    dat.h         shared types
    mkcoast.c     GeoJSON coastline -> coast.dat
    mkstations.c  radio-browser JSON -> stations
    util.c/util.h readall(), shared by the two converters
    stations      station list (generated, sample committed)
    coast.dat     coastline data (generated)
    README        user-facing docs
    NOTES.md      this file

## Build

    mk            builds radioglobe, mkcoast, mkstations
    mk clean

## Run

    radioglobe -s stations -c coast.dat   # explicit data files
    radioglobe                            # uses /lib/radio/*
    radioglobe -r 48000                   # if /dev/audio is 48k

Flags: -s stationfile  -c coastfile  -r devrate(Hz, default 44100)

Install:
    mk install
    mkdir -p /lib/radio
    cp stations coast.dat /lib/radio/

## Redraw performance (coastlines)

coast2.dat is a much higher-resolution Natural Earth extract than
the original coast.dat (hundreds of thousands of points vs. ~600).
The naive draw loop -- recompute sin/cos for every point of every
polyline, every redraw -- got noticeably slow at that scale,
especially while dragging (which redraws on every mouse-motion
event) and even on redraws that don't touch the globe at all (hover
label / status bar updates used to repaint the whole coastline set
too). Fixed in three layers, each in globe.c/coast.c, all
transparent to the coast.dat format and to mkcoast:

1. Precomputed unit vectors. coastinit() converts every point's
   lat/lon into a Cartesian unit vector once at load time
   (Coastline.vec[], via geo2vec()). Per-frame projection
   (vecproject() in globe.c) is then a handful of dot products
   against a once-per-frame view basis (viewbasis()), instead of
   sin/cos/asin/atan2 per point per frame.
2. Cached static layer. globedraw() renders ocean+grid+coastlines+
   border into an offscreen Image, keyed on (rect, clat, clon,
   zoom), and only regenerates it when one of those actually
   changes. Redraws triggered by hover/selection/status-bar updates
   now just blit the cached image -- the coastline walk doesn't
   happen at all for those.
3. Bounding-cap culling. Each Coastline gets a precomputed bounding
   spherical cap (center direction + cosine of half-angle,
   computecap() in coast.c). drawcoasts() uses one dot product per
   polyline to reject it outright if it cannot possibly have any
   point facing the viewer (e.g. it's entirely on the far side of
   the globe), without touching any of its points. Conservative by
   construction: it can fail to cull a polyline that turns out to
   have nothing visible point-by-point, but it will never wrongly
   skip one that does -- correctness rests on the existing per-point
   visibility test, this is purely an early-out.

Net effect: per-frame cost during drag/zoom is now proportional to
"points in polylines that could plausibly be on the visible
hemisphere," not "every point in the whole dataset," and redraws
that don't change the view do no coastline work at all.

### Deferred: tiling long polylines

The bounding-cap trick (#3 above) helps most when polylines are
reasonably short/localized (e.g. one per island or per coastline
segment). If coast2.dat instead encodes very long polylines (say,
one continuous shape spanning a big chunk of a continent or more),
the cap ends up wide and the early-out rarely fires. Not yet
checked whether coast2.dat's polylines are short or long-and-few;
if the latter, the next worthwhile step is chopping long polylines
into shorter chunks (in mkcoast, or as a one-time post-process on
the .dat file) purely so each chunk gets its own tight cap. Not
done since current performance seems acceptable; revisit if large
data still feels slow after a tiling test.

**Update (tested against real data):** confirmed needed. Running with
coast2.dat (~6x the points of coast.dat) plus stations2 still
feels chunky during drag/zoom animation -- the three optimizations
above clearly help (it's not anywhere near as bad as the naive
per-point-per-frame version would be) but aren't enough at this
data scale. Likely cause matches the suspicion above: if coast2.dat's
polylines are long (whole continents/coastline-runs rather than
per-island segments), their bounding caps are wide and rarely cull
anything, so drawcoasts() still walks most points most frames.
Committing the current three-layer optimization as-is (it's a real
improvement and a clean base), but it is not sufficient for
coast2.dat-level detail. Next concrete step: measure whether
coast2.dat's polylines are in fact long-and-few (a quick point-count
histogram per polyline would confirm/deny this in a minute), then
implement the tiling/chunking post-process described above if so.
Also worth profiling stations2 separately -- drawstations() has no
equivalent culling at all yet (every station is projected every
frame regardless of visibility), which may also be contributing at
that station-count scale.

### Two easy fixes landed (cache reuse + station vectors)

Before tackling the tiling work above, fixed two cheap, contained
issues found while reviewing the code for the momentum-spin change:

1. **Cache image churn (globe.c:globedraw()).** The offscreen
   `cache` image (see "Cached static layer" above) was being freed
   and reallocated from scratch on *every* call where clat/clon/zoom
   changed -- i.e. every single mouse-motion event during a drag,
   regardless of dataset size. That's a round trip to the draw
   device to destroy and rebuild an image of the same size, every
   drag frame, for no reason (the rectangle essentially only changes
   on window resize). Fixed: only free/realloc when the rectangle
   itself changes; every other redraw now draws fresh content into
   the same already-allocated image. No behavior change, pure
   removed overhead, and it scales with drag-frame-rate rather than
   dataset size, so it likely affected the coast2.dat+stations2
   "chunky" feel mentioned above independent of the polyline-length
   issue.

2. **Station culling (globe.c:drawstations(), dat.h, main.c).**
   Stations now get the same treatment coastlines already had:
   Station gained a precomputed Vec3 `vec` field (dat.h), filled in
   once per station at load time via geo2vec() (main.c:
   loadstations()). drawstations() now computes the per-frame view
   basis once (viewbasis()) and projects each station with
   vecproject() against its precomputed vector -- a few dot products
   -- instead of calling geo2screen()/project() (sin/cos/asin/atan2)
   on every station on every redraw. Same win as the coastline
   vector precompute, just applied to stations; matters once a
   station file the size of stations2 is loaded.

Both are pure performance changes with no behavior change (same
dots, same visibility test, same hit-testing in main.c, which still
uses the original geo2screen() path -- not yet touched, see below).

Note: main.c:findstation() (used for both click hit-testing and
hover-label lookups, called on basically every mouse-motion event
even when not dragging) still loops over all stations calling the
old geo2screen()/project() path. It wasn't touched in this round to
keep the change small, but it's a natural follow-up using the same
Station.vec infrastructure if hover lag is ever noticeable with
stations2.

Next: the coastline tiling/chunking work is still the big remaining
item for coast2.dat-level detail (see above) -- these two fixes
reduce constant overhead but don't change the fact that long
polylines defeat the bounding-cap early-out.

### Batched poly() draws (bigger fix, also landed)

Realized the two fixes above (cache reuse, station vectors) don't
touch what is likely the *actual* dominant cost at coast2.dat scale:
drawcoasts() drew one line() call per line *segment* -- i.e. one
draw-device protocol round trip per visible point-pair. The
vector-precompute work earlier removed the per-point trig cost, but
never touched this: a coastline polyline with thousands of on-screen
points still cost thousands of separate protocol messages every
redraw, no matter how cheap the projection math got. With
hundreds of thousands of points in coast2.dat, this is plausibly a
much bigger cost than the trig ever was, and tiling/chunking
(splitting *which* polylines we walk) was never going to fix it,
since it doesn't reduce the number of points actually drawn once a
polyline (or chunk of one) is visible -- it only helps skip
polylines that are entirely off-screen.

Fix: draw(2) has poly(dst, p, np, end0, end1, radius, src, sp), which
draws a whole connected polyline -- conceptually a series of line()
calls -- in a *single* protocol message. drawcoasts() now accumulates
each run of consecutive visible points into a buffer and issues one
poly() call per run, instead of one line() call per segment. Same
exact set of segments drawn (a segment only exists where both
endpoints are visible, same as before), just batched.

Caveat found while implementing: poly()'s wire format packs the
point count as (np-1) into a 16-bit field (see
/sys/src/libdraw/poly.c), so a single call tops out at 65536 points.
Given the live concern that some coast2.dat polylines may be very
long (whole continents), runs are chunked at Maxpolypts=8192 points
per call, carrying the boundary point over into the next chunk so
the connecting segment is still drawn. Comfortably under the 65536
hard limit, and still a huge reduction in call count for anything
that isn't already short.

This is probably the single biggest lever available without
inspecting coast2.dat's actual structure, since it attacks the
"every visible point costs a protocol round trip" cost directly,
independent of whether polylines happen to be long or short. The
tiling/chunking idea above is still worth doing too (it reduces
*work* for polylines that turn out to be entirely off-screen, which
poly-batching doesn't help with at all), but this should be tried
first and re-evaluated before investing in tiling.

Not yet done: the same per-segment line() pattern exists in
drawgrid() (lat/lon grid lines), but at ~1500 total points across
the whole grid it is nowhere near the cost coastlines were, so left
alone for now.

### Fixed: 'q' felt delayed while momentum spin is active

Observed during testing with coast2.dat/stations2: pressing 'q' (or
any key) to quit while the inertial spin (see "Momentum / inertial
spin" above) was still decaying seemed to wait for the spin to
finish before taking effect.

Cause: event(2)'s multiplexer (/sys/src/libdraw/event.c) gives each
input source a fixed priority by its key's bit position -- mouse(1)
is slave 0, keyboard(2) is slave 1, our timer(4, see Etick in
main.c) is slave 2 -- and always returns the lowest-index source
with data ready, so keyboard does outrank the timer in principle.
The catch: the raw event pipe is only drained (extract()) when the
main loop calls back into event(), and our Etick case ran redraw()
fully and synchronously before looping back. At coast2.dat/
stations2 scale, even after the poly()-batching fix above, a single
redraw is not free; while it's in flight, an arriving keypress just
sat buffered in the pipe, unseen, until that redraw call returned.
During an active fast spin, ticks kept arriving every ~25ms and each
one retriggered another redraw, so a keystroke could keep "just
missing its turn" against a steady stream of new ticks -- giving the
impression input was locked out until the spin fully decayed, even
though no single redraw took anywhere near that long.

Fix (main.c, case Etick): check `ecankbd()` (event(2)) at the top of
the tick handler and `break` (skip this tick's redraw entirely) if a
keystroke is already waiting, so the loop goes straight back to
event() instead of doing another redraw first. Small, self-contained,
no effect on the spin's physics (a skipped tick just doesn't apply
that tick's velocity/friction step; it picks back up next tick if
the key wasn't a quit).

## Momentum / inertial spin on drag release (IMPLEMENTED)

Dragging the globe used to move it 1:1 with the mouse and stop
dead the instant you released the button. Now it eases to a stop
instead, like flicking a real globe. (a.k.a. "momentum scrolling" /
"inertial rotation" / "flick to spin" -- the effect used in Google
Earth, phone map apps, etc.)

Implemented in main.c exactly per the plan below:
  - `etimer(Etick, Tickms)` (Etick=4, Tickms=25ms, ~40Hz) is
    started once after einit(); event(&ev) returns Etick like any
    other event key, handled by a `case Etick:` alongside
    Emouse/Ekeyboard in the main switch.
  - vlat/vlon (deg/ms) are updated continuously while dragging
    from consecutive (clat,clon,m.msec) samples.
  - Grabbing the globe (new button-1 down) zeroes vlat/vlon so
    re-grabbing a spinning globe stops it immediately.
  - On Etick, if not dragging and (vlat,vlon) != (0,0), clat/clon
    advance by velocity*Tickms, clamp/wrap as usual, then vlat/vlon
    decay by `friction` (0.92) each tick and snap to zero below
    `vmin` (0.0005 deg/ms) so the timer goes idle (no more redraws)
    once the spin has visibly stopped.
  - globe.c/coast.c untouched, as predicted -- this rode entirely
    on the existing redraw-cache machinery.

Tuning knobs (main.c globals): Tickms, friction, vmin. Not yet
hand-tuned against a real run; if it feels too "light" (stops
too fast) raise friction toward 0.95-0.97; too "heavy"/floaty,
lower it. Original design notes kept below for reference.

### Original design (for reference)

Currently dragging the globe moves it 1:1 with the mouse and it
stops dead the instant you release the button. Feels a bit static;
a globe with "weight" to it would ease to a stop instead, like
flicking a real globe. (a.k.a. "momentum scrolling" / "inertial
rotation" / "flick to spin" -- the effect used in Google Earth,
phone map apps, etc.)

This rides for free on the redraw performance work above: each
spin-down animation frame is just a clat/clon change like any drag
frame, so it costs no more than dragging already does. No changes
needed in globe.c/coast.c; this is a self-contained main.c feature.

### Plan

1. **Timer.** Call `etimer(0, ~20-30 /* ms, i.e. 30-50Hz */)` once
   after `einit()`; it returns a timer event key (call it `Etick`)
   that comes back from the *same* `event(&ev)` call already used
   in the main loop, alongside Emouse/Ekeyboard. Add one more
   `case Etick:` to the existing `switch(e)`. No threads, no
   separate process, no select() hackery needed -- this is exactly
   what etimer(2) is for.

2. **Track instantaneous velocity while dragging.** The existing
   drag code computes clat/clon from the *total* offset since
   button-down (dragstart/dragclat/dragclon), which is fine for
   1:1 tracking but doesn't give an instantaneous speed. Add two
   small bits of per-drag state:
       double lastclat, lastclon;
       ulong  lastmsec;       /* from Mouse.msec, free timestamp */
       double vlat, vlon;     /* degrees per ms, running estimate */
   On every mouse-motion event while dragging, after computing the
   new clat/clon, if `m.msec > lastmsec`:
       dt = m.msec - lastmsec;
       vlat = (clat - lastclat) / dt;
       vlon = (clon - lastclon) / dt;
       lastclat = clat; lastclon = clon; lastmsec = m.msec;
   This keeps a continuously-updated "current speed" sample; the
   value at the moment the button comes up is what we hand off to
   the animation. (A one-pole smoothing filter, e.g.
   `vlat = 0.5*vlat + 0.5*newsample`, would reduce jitter from
   irregular mouse event spacing if the raw estimate feels twitchy;
   not necessary for a first cut.)

3. **Kick off the spin on release.** Where the code currently does
   `if(dragging){ dragging = 0; }` on button-up, that's it -- vlat/
   vlon already hold the launch velocity from step 2. Nothing else
   to do there except maybe zero them if the drag was very short
   (avoids a flick on an accidental tiny twitch/click).

4. **Animate on each Etick.** Convert vlat/vlon (deg/ms) to deg/tick
   using the timer's actual period, apply friction, stop when slow:
       const double friction = 0.92;     /* tune: higher = "heavier" */
       const double vmin = 0.0005;       /* deg/ms, stop threshold */
       case Etick:
           if(dragging || (vlat==0 && vlon==0))
               break;                    /* nothing to animate */
           clon += vlon * tickms;
           clat += vlat * tickms;
           if(clat > 90) clat = 90;
           if(clat < -90) clat = -90;
           while(clon > 180) clon -= 360;
           while(clon < -180) clon += 360;
           vlat *= friction;
           vlon *= friction;
           if(fabs(vlat) < vmin && fabs(vlon) < vmin)
               vlat = vlon = 0;          /* fully stopped, go idle */
           redraw();
           break;
   The `dragging` check matters: don't let a stale momentum value
   fight an active drag (also zero vlat/vlon when a new drag starts,
   so re-grabbing a spinning globe stops it immediately, matching
   real-world expectation).

5. **Tuning knobs** (pick by feel, not by theory):
   - tick interval (~20-30ms is plenty smooth; coarser saves
     redraws, finer feels silkier),
   - friction per tick (closer to 1.0 = spins longer/"heavier";
     0.9-0.95 is a reasonable starting range),
   - stop threshold (too high stops abruptly, too low spins
     forever at an imperceptible crawl and never goes idle).

### Why this is low-risk to add later

- Doesn't touch globe.c/coast.c at all -- pure main.c event-loop
  and drag-state change.
- The timer is idle (cheap no-op check, no redraw) whenever
  vlat==vlon==0, i.e. essentially always except during an actual
  spin-down, so it doesn't introduce a constant redraw-every-30ms
  background cost.
- Builds entirely on existing pieces: Mouse.msec (already
  provided), the existing clat/clon clamping logic (already
  written, just needs to be reachable from the timer case too),
  and the render cache (already invalidates correctly on any
  clat/clon change, momentum-driven or not).

## Review fixes round (landed)

A code-review pass fixed a batch of small bugs and cleanups in
one go (deliberately NOT addressing stream-death detection --
if the radio goes silent, the listener notices; not worth the
plumbing):

- **Reload leak (main.c:loadstations()):** freed the stations
  array but never the strduped name/url strings; every menu
  "reload" leaked the whole string set.  Now frees them first.
- **Antimeridian flick spike (main.c drag handler):** vlon was
  computed from already-normalized clon, so dragging across
  +-180 made the delta ~360 deg and a release there launched
  the globe at absurd speed.  The delta is now wrapped into
  [-180,180] before dividing by dt.
- **Stale `playing` after reload:** menu reload now stops the
  stream; station indices are meaningless across a reload and
  the status bar could name the wrong station.
- **globehit() removed (globe.c, dat.h):** dead code (nothing
  called it) with a latent divide-by-zero at rho==0 (click dead
  center).  Delete rather than fix; resurrect from git if a
  "click ocean to recenter" feature ever wants it.
- **globegeom() helper (globe.c, dat.h):** the cx/cy/rad disc
  geometry was open-coded in ~6 places.  Now one function.
  This also fixed a real desync: the drag handler measured
  screen->r while the renderer measures globerect() (screen
  minus status bar), so drag didn't track the cursor exactly
  1:1.  Both now use globegeom(globerect(), ...).
- **stationhit() (globe.c):** findstation() (hover + click, runs
  per mouse-motion event) now uses the precomputed Station.vec /
  viewbasis/vecproject path like drawstations(), instead of
  trig-per-station geo2screen().  This closes the "natural
  follow-up" flagged in the earlier perf notes.  Hit radius now
  tracks dot size (12+dotradius(zoom)) instead of fixed 15px.
- **Scroll chord edge bug (main.c):** the scroll-wheel cases
  broke out of the Emouse case before oldbuttons was updated,
  so a chorded scroll+button event could double-trigger button
  edge detection.  oldbuttons is now updated on those paths.
- **coastinit() (-c failure):** an explicitly named -c file that
  fails to load is now sysfatal instead of silently falling back
  to ./coast.dat / /lib/radio/coast.dat.
- **mkstations -b error path:** EARGF(fprint(...)) aborted (the
  ARGBEGIN macro calls abort() after the expression) instead of
  exiting cleanly; now a proper usage() like main.c has.
- **Shared readall():** identical function was duplicated in
  mkcoast.c and mkstations.c; moved to util.c/util.h, linked
  into both converters (mkfile updated).
- **mkfile nits:** CLEANFILES=$TARG made clean delete the same
  files twice (dropped); installdata's `test -f x && cp ...`
  made the recipe FAIL when a data file was merely absent --
  now `if(test -f x) cp ...` (optional means optional).
- **Alloc checks:** strdup/quotestrdup results in main.c are now
  checked (sysfatal), consistent with the rest of the file.

Gotcha worth remembering: `mk clean all` in ONE invocation can
fail -- mk stats object files before clean deletes them, decides
they're up to date, then the link step can't find them.  Run
`mk clean` and `mk` as separate invocations.

## Drag smoothness round (landed)

Investigating "flickers a lot with detailed maps and lots of
stations".  First finding: the frame IS already fully composed
offscreen -- redraw() renders globe+stations+statusbar into a
`back` image and blits it to screen with one draw() + one
flushimage().  There is no flip primitive in the draw device;
a single atomic blit is the best available, and we do it.  If
literal flicker (half-drawn frames) is still observed, check
that /$objtype/bin/radioglobe isn't a stale pre-back-buffer
binary: `mk install`.

Two new fixes for the stutter that remains at coast2/stations2
scale:

1. **Frame-paced drag redraws (main.c).**  The drag handler
   redrew on every mouse-motion event.  When one redraw takes
   longer than the gap between mouse events, the event queue
   backlogs and every stale intermediate position gets fully
   rendered in sequence -- the globe visibly lags and judders
   behind the cursor.  Now drag motion only updates the orbit
   (cheap; every sample still feeds the velocity tracker, so
   flick momentum is unchanged) and sets a `dragdirty` flag;
   the existing 40Hz Etick renders the latest state once per
   tick while dragging.  Release renders any pending final
   position immediately.  Redraw rate is now bounded at 40Hz
   no matter how fast the mouse streams events.

2. **Same-pixel point suppression (globe.c:drawcoasts()).**
   Zoomed out, consecutive coastline points overwhelmingly
   project to the same pixel; each kept point costs a memline
   in the draw device.  Points identical to the previous
   runbuf entry are now skipped -- the drawn segments are
   identical minus zero-length ones.  Cuts raster + protocol
   cost roughly in proportion to (dataset density / zoom); does
   NOT cut per-point projection cost (see LOD below).

### Follow-up: dot flicker at stations2 scale (landed)

Dots still flickered with stations2 (fine with the small
stations file).  Two causes, both scaling with station density:

1. **Selected dot + label drawn mid-loop, then overdrawn
   (globe.c:drawstations()).**  The orange dot and its label
   were emitted the moment the loop hit i==sel; every
   later-indexed station then drew on top of them.  Dense file
   = real overlaps, and *which* dots overlapped changed every
   frame during rotation, so the selection and label visibly
   flickered.  Now the loop skips sel and draws the selected
   dot + label after all other dots, always on top.

2. **Hover selection churn = unpaced redraw storm (main.c).**
   findstation() runs per mouse-motion event; in a dense field
   the nearest station changes on nearly every pixel of travel,
   and each change triggered an immediate full redraw -- same
   backlog disease the drag path had, each queued redraw
   showing a different selection.  Hover changes now just set
   the dirty flag.

While there, unified the pacing: the per-drag dragdirty flag
became a general `dirty` flag; drag motion, momentum ticks, and
hover changes all mark it, and Etick renders the latest state
at most once per tick.  This also deleted the special-case
render-on-release and render-while-dragging blocks from the
previous round -- one funnel instead of three paths.  Discrete
actions (zoom, keyboard, menu, click) still redraw immediately.

### Follow-up 2: limb popping, dot dedup, sticky hover (landed)

Third round on "dots flicker at stations2 scale", attacking the
remaining mechanisms (globe.c, one prototype in dat.h/main.c):

1. **Limb taper (dotsize()).**  Dots popped in and out at full
   size as rotation carried them across the cosc>0 visibility
   threshold.  vecproject() now reports cosc itself (magnitude =
   distance from the limb) instead of a boolean, and dot radius
   ramps from 1px at the horizon to full size over a narrow band
   (cosc 0..0.08) -- the pop becomes a swell.
2. **Per-frame dot dedup (dotseen()).**  Stations stack (many
   share one coordinate; zoomed out, whole clusters collapse
   onto single pixels), and every duplicate cost a fillellipse
   message repainting already-painted pixels.  A generation-
   stamped open-addressed table (free per-frame clear, short
   probe cap that fails open to just drawing a duplicate) drops
   dots already drawn at the same pixel this frame.
3. **Off-window cull.**  Zoomed in, most of the visible
   hemisphere projects outside the window; those dots were still
   sent to the draw device to be clipped there.  ptinrect against
   the window (inset by -dotr) skips them client-side.
4. **Sticky hover selection (stationhit()).**  In a dense field
   the nearest station changes on nearly every pixel of travel,
   so the label hopped between packed neighbors even at paced
   redraw rates.  stationhit() now takes the current selection
   and keeps it while the cursor remains within hit distance of
   it; selection changes only when the cursor actually leaves
   the selected station.  Also means a click always hits the
   station the label names.

### Follow-up 3: frame cost + pacing round (landed)

Remaining flicker at stations2/coast2 scale is dominated by low
frame rate (moving dots strobe when they jump many pixels per
frame) plus unsynchronized presentation (no vsync anywhere in
the stack; the only mitigation is being fast).  So this round
attacks frame cost and pacing, and adds measurement:

1. **Coastline chunking (coast.c:chopcoasts()).**  The long-
   deferred, confirmed-needed fix: polylines are split into
   <=256-point chunks at load (adjacent chunks share the
   boundary point; chunks alias the original pts arrays).
   Continent-length polylines had caps covering half the
   sphere, so the bounding-cap cull never fired on exactly the
   datasets that needed it; tight per-chunk caps make it work
   again.
2. **Zoom LOD (dat.h, coast.c:meanstep(), globe.c).**  Each
   (post-chop) polyline records its mean angular point spacing
   at load.  drawcoasts() converts that to on-screen pixels
   (step*rad) and strides over points when consecutive points
   are sub-pixel, clamped to 16x, always landing on the final
   point so chunk boundaries stay connected.  Zoomed out, work
   scales with pixels on screen instead of dataset size; zoomed
   in, stride returns to 1 (full detail).
3. **Tick coalescing (main.c).**  Ticks that pile up behind a
   slow redraw each triggered another redraw back-to-back
   (bursty, always slightly stale).  The Etick handler now
   applies each tick's physics but skips the redraw when
   ecanread(Etick) shows another tick already queued -- one
   redraw for the whole backlog, with fully advanced state.
4. **Frame-time readout (main.c).**  redraw() times itself and
   the idle status line shows the previous frame's cost in ms.
   Diagnosis without guessing: if the number is high, flicker
   is strobing/low fps (attack frame cost); if it's low and
   flicker persists during motion only, it's presentation-level
   tearing (nothing app-side left); if flicker shows on a fully
   idle globe, it isn't radioglobe at all (stale binary or
   display path).

### Follow-up 4: selection-only updates (landed)

User observation that nailed the remaining hover flicker: merely
mousing over any station repainted dots nowhere near the pointer.
A hover selection change was taking the full redraw path -- whole
cache blit, every dot, full-window blit to screen -- to change
what amounts to one dot's color, a label, and the status bar.  On
a display path with unsynchronized presentation, that full-window
rewrite is visible as a global dot shimmer.

Now split into base + overlay (globe.c, main.c, dat.h):

- drawstations() draws ALL dots plain (including the selected
  one) into the composed base frame in `back`.
- drawsel() (new) draws the highlight dot + label over the top
  and returns the screen rectangle it covered.
- redraw() keeps the overlay OUT of back: it composes base into
  back, blits, then draws the overlay on the screen only.  back
  therefore always holds the selection-free base.
- redrawsel() (new light path, used by the hover handler): when
  only the selection changed, restore the old overlay rect's
  pixels from back, draw the new overlay, redraw the status bar.
  Every other pixel on screen is not written at all -- distant
  dots physically cannot flicker on hover now.

Selection changes are cheap enough to run per mouse event again,
so they bypass the tick pacing (which still governs all
view-changing redraws).

### Follow-up 5: light-path polish (landed)

Why mouseover flicker happened at all, for the record: nothing
in the display stack has vsync (app blit, framebuffer, host
presentation all free-run), so a full-window rewrite -- which is
what a hover selection change used to trigger -- can always be
sampled mid-write.  Small high-contrast dots show that most.
The base+overlay split (follow-up 4) fixed the structure; this
round trims what remains:

- **Status bar strip trimmed on the light path (main.c).**  The
  full window-width bar erase+text was the biggest write left on
  a hover change.  dst==screen now erases only as far as the new
  text (and the previous text) reaches; full recomposes into
  back still paint the whole strip so no stale tails survive.
- **Hover skips the light path while a view redraw is pending
  (main.c).**  If a momentum tick advanced the orbit but its
  redraw hasn't run, back/screen lag the orbit; drawing an
  overlay positioned by the new orbit onto the old frame briefly
  misplaces it.  When dirty is set, the queued full redraw shows
  the new selection instead.
- **Cluster-lite dots (globe.c).**  The per-frame dedup now
  keys on dotr-sized cells instead of exact pixels: stations
  packed closer than a dot radius (which already overlapped
  almost entirely) draw as one dot.  Cleaner look in dense
  areas and fewer fillellipse messages exactly where there were
  most.

Diagnostic for "is my binary current": wave the mouse over dots
without dragging and watch the ms number in the status bar.  The
light path doesn't recompute it, so if it updates on mere hover,
the running binary predates follow-up 4 -- mk install.

Still open:

- **Real station clustering** (cluster-lite above merges
  same-cell dots at draw time; a fuller version would merge
  near-neighbors, show cluster counts, and cut hit-test cost).

## TODO / ideas

- mixfs for playback (auto-resample + multiple simultaneous
  streams) instead of the pcmconv pipeline.
- ICY now-playing metadata (zuke has icy.c to borrow).
- Keyboard station search.
- Filled landmasses (needs polygon clipping vs horizon circle).
- Cache fetched data, maybe a refresh command in the menu.
- Cluster/de-dup nearby station dots when zoomed out.
- Momentum/inertial spin on drag release: DONE (see above);
  still want to hand-tune friction/vmin/Tickms against a real
  run for feel.

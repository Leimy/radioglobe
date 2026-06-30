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

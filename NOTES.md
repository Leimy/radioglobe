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
- Rendering as it stands now: a texture-mapped, lit globe.
  With a real equirectangular photo (NASA Blue Marble) baked
  by `mkearth -i`, drawearth() samples it bilinearly per pixel
  through interpolated row/atan LUTs, so it stays smooth to the
  512x zoom limit; the vector coastlines and grid are still
  drawn on top.  Motion frames render at half resolution and a
  full-quality settle frame follows when the motion stops, so
  drag and spin are smooth despite the per-pixel cost.
- Animation is frame-rate independent: momentum lives in
  libview's Orbit, and radioglobe sets orb.frictionref = Tickms
  and passes real elapsed nsec() time, so a flick covers the
  same ground in the same wall-clock time at 8fps as at 40fps.
  This replaced the earlier tick-count physics, which was the
  actual cause of "the animation doesn't keep up / gets out of
  sync."  See "The real 'animation gets out of sync' bug" below.
- Dependency: libview (/usr/dave/work/libview) -- Vec3 helpers,
  viewbasis(), and the Orbit inertial-drag state machine, shared
  with claudegraph.  radioglobe links libview.a$O; the mkfile
  rebuilds it from that work tree, so plain `mk` suffices.  The
  converters do not link it.  See "libview" below.
- Solid-earth mode (IMPLEMENTED): mkearth.c bakes an
  equirectangular earth in one of two ways -- a land/ocean mask
  from coast.dat (`mkearth < coast.dat > earth.mask`), or a real
  RGB photo texture from an equirectangular world image like
  NASA Blue Marble (`mkearth -i earth.plan9img > earth.mask`,
  fed a Plan 9 image produced by `jpg -9 -t`).  If radioglobe
  finds either kind of earth.mask (-e, or ./earth.mask, or
  /lib/radio/earth.mask) it draws a shaded, lit, solid globe
  (globe.c: earthinit()/drawearth()) instead of the flat ocean
  disc, with the vector coastline outline and grid still drawn
  on top from coast.dat as always.  Neither found -> unchanged
  flat-disc rendering.  See "Solid-earth rendering" and "Real
  photo texture support" below for how this differs from the
  earlier tinyrenderer investigation (which is still just
  parked/not used).
- Source lives in /usr/dave/work/radioglobe.  (The stray
  /usr/dave/src from early on is gone; nothing to clean up.)

### Next time / TODO shortlist
- Confirm the pcmconv fix actually cured the speedup on real
  streams (esp. 48k AAC).  If some still sound off, check whether
  /dev/audio is at 48000 and try `radioglobe -r 48000`.
- Consider routing through mixfs instead of pcmconv: it auto-
  resamples to the device output AND allows multiple streams.
- De-dup stations stacked on identical coordinates (many
  SomaFM/WALM/0N entries share one lat/lon -> overlapping dots).
- ICY now-playing metadata (zuke has icy.c to borrow).

## libview (shared view math)

radioglobe no longer owns its view math.  /usr/dave/work/libview
holds it, and claudegraph (/usr/dave/work/claude9/claudegraph.c)
links the same archive; the point of the extraction was that both
programs had already grown diverging copies of the same code,
including the antimeridian wrap fix, which now exists in exactly
one place.

What radioglobe gets from it:

  view.h    the API contract; requires u.h/libc.h first.  Don't
            change signatures casually -- claudegraph compiles
            against it too.
  Vec3      x/y/z plus v3dot/v3len/... (dat.h's Station.vec and
            Coastline.vec/center are Vec3).
  viewbasis(yaw, pitch, &ex,&ey,&ez)
            the screen basis; radioglobe maps lon->yaw,
            lat->pitch and calls it once per frame, then projects
            with three dot products (globe.c:vecproject()).
            drawearth() uses the same basis for its per-pixel
            inverse projection.
  Orbit     yaw/pitch/zoom plus the inertial drag state machine:
            orbitdown (grab stops the spin), orbitmove (wrap-safe
            velocity across the +-180 seam), orbitup, orbittick,
            orbitzoom, orbitnorm.  main.c's `orb` is the single
            source of truth for the view; clat/clon/zoom no
            longer exist as separate globals (globe.c's arguments
            are still named clat/clon and are fed orb.pitch and
            orb.yaw).

radioglobe-specific settings applied after orbitinit(&orb, 0, 30,
1): zoommin 0.5, zoommax 512 (raised from 128 to separate
same-city station clusters), frictionref = Tickms (real-time
decay, see below); pitch clamp and friction/vmin keep libview's
defaults.  Drag sensitivity is passed per event as 180.0/rad,
recomputed from globegeom().

Library-side additions made for radioglobe are additive only:
`Orbit.frictionref` defaults to 0, which reproduces the original
per-call friction behavior bit-for-bit so claudegraph is
unaffected.  libview has its own regression test
(`cd /usr/dave/work/libview && mk && ./viewtest`), including
testfrictionref, which asserts exactly that compatibility.

## Repos and branches

Three separate trees, and only two of them are git repos:

  radioglobe  /usr/dave/work/radioglobe -- on branch `rendering`
              (branches: front, rendering).  The texture-mapped
              earth, the pacing/coarse-frame work and the
              elapsed-time momentum all landed here.
  libview     /usr/dave/work/libview -- on branch `front`, its
              own repo.  Carries the additive `frictionref`
              field, orbitstep() factoring, and the
              testfrictionref regression test.
  tr          /usr/dave/work/tr -- NOT a git repo (no .git).  The
              tinyrenderer port plus globebench.c.  radioglobe
              does not link or include anything from it; see the
              investigation section below for what it was used
              for, and note that globe.c's header comment and
              this file both cite
              /usr/dave/work/tr/p9/globebench.c, so those
              references dangle for anyone who only has the
              radioglobe repo.

Order matters when pushing: radioglobe's mkfile points at the
libview *work tree* by absolute path, so a fresh clone of
radioglobe alone will not build.  Either push libview first and
say so in its README's "Linking it into a consumer" terms
(work-tree mode now, installed-copy mode -- $home/lib,
$home/include -- once libview lives on its own), or vendor it.
That decision is not made yet.

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

    main.c        event loop, UI, station mgmt, audio, frame
                  pacing, momentum tick (orbittick + real dt)
    globe.c       orthographic projection + rendering, solid
                  earth (land/ocean mask or photo texture),
                  per-pixel shading, station dots + overlay
    coast.c       loads coast.dat at runtime, chunks polylines
    dat.h         shared types
    mkcoast.c     GeoJSON coastline -> coast.dat
    mkstations.c  radio-browser JSON -> stations
    mkearth.c     coast.dat -> earth.mask (land/ocean raster),
                  or -i image -> earth.mask (RGB photo texture)
    util.c/util.h readall(), shared by the two converters
    stations      station list (generated, sample committed)
    coast.dat     coastline data (generated)
    coast2.dat    higher-detail coastlines (generated, ~6x points)
    stations2     larger station list (generated)
    earth.mask    solid-earth data: land/ocean mask or photo
                  texture (generated, optional)
    README        user-facing docs
    NOTES.md      this file
    CODE-REVIEW.md  static review of the whole tree (2026-07-16);
                  findings still open are listed there, not here

Outside this directory:

    /usr/dave/work/libview   Vec3 math, viewbasis(), Orbit
                  inertial drag; linked as libview.a$O, shared
                  with claudegraph (/usr/dave/work/claude9)

## Build

    mk            builds libview (if stale), then radioglobe,
                  mkcoast, mkstations, mkearth
    mk clean

The mkfile carries `-I/usr/dave/work/libview` in CFLAGS, links
`/usr/dave/work/libview/libview.a$O` into radioglobe only, and has
a virtual rule that runs `mk` in the libview tree first, so a
libview source change is picked up here without an `mk install`
in between.  Do NOT run `mk clean all` in one invocation -- mk
stats the object files before clean deletes them, decides they
are up to date, and the link step then can't find them.

## Run

    radioglobe -s stations -c coast.dat   # explicit data files
    radioglobe -e earth.mask              # solid-shaded globe
    radioglobe                            # uses /lib/radio/*
    radioglobe -r 48000                   # if /dev/audio is 48k

Flags: -s stationfile  -c coastfile  -e earthmask  -r devrate(Hz,
default 44100)

Generating the mask (optional; needs coast.dat first):
    mkearth < coast.dat > earth.mask

Install:
    mk install
    mkdir -p /lib/radio
    cp stations coast.dat earth.mask /lib/radio/

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

**Where it lives now:** the state machine described below was
implemented in main.c first (vlat/vlon and friction inline), and
was later extracted verbatim into libview's `Orbit`
(/usr/dave/work/libview/orbit.c: orbitdown/orbitmove/orbitup/
orbittick, with vyaw/vpitch in place of vlon/vlat) so claudegraph
and radioglobe stop carrying diverging copies of it.  main.c now
owns only the timer, the elapsed-time measurement, and the redraw
pacing; the physics is library code.  Read the bullets below as
the design, not as a current file map.

Implemented originally in main.c exactly per the plan below:
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

Tuning knobs: `Tickms` is still a main.c enum (25ms, ~40Hz, and
also what main.c hands to `orb.frictionref`); `friction` (0.92)
and `vmin` (0.0005) are now Orbit fields with those defaults set
by libview's orbitinit(), so tuning them means assigning
`orb.friction` / `orb.vmin` in main() after orbitinit, not editing
libview.  Not yet hand-tuned against a real run; if it feels too
"light" (stops too fast) raise friction toward 0.95-0.97; too
"heavy"/floaty, lower it.  Note that with frictionref set, the
friction value is now per-Tickms-of-real-time rather than
per-tick-event, which is what makes the feel independent of frame
cost.  Original design notes kept below for reference.

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

### Follow-up 6: dense-cluster selection round (landed)

User report: individual stations in dense clusters (Europe) are
nearly impossible to hover-select at almost any zoom level.
Four causes/fixes (globe.c, main.c):

1. **Int overflow in stationhit() (real bug).**  Squared pixel
   distances were computed in int; zoomed in, rad reaches tens
   of thousands of pixels, a far-off-window station projects up
   to ~2*rad from the cursor, and (2*rad)^2 overflows 32 bits
   (from roughly zoom 45 on a ~1000px window).  Wrapped
   "distances" could beat the station actually under the cursor,
   making selection erratic exactly when zooming in to split a
   cluster.  Fixed with a box-reject against maxd before
   squaring (also cheaper); the sticky-check square is guarded
   the same way.
2. **Sticky zone shrunk to the drawn dot.**  Sticky selection
   kept the current station while the cursor stayed within the
   full grab radius (maxd ~17px).  In a dense cluster every
   neighbor sits inside that zone, so hover could never move to
   them without leaving the whole neighborhood.  The sticky
   boundary is now the drawn dot itself (dotsize()+2px); slide
   off the dot and nearest-wins resumes.  Clicks still hit the
   station the label names.
3. **Dots shrink again at deep zoom (dotradius()).**  Dots were
   5px for all zoom > 3, and cluster-lite dedup merges anything
   packed closer than a dot radius, so clusters stayed fused as
   one blob well past the zoom where 3px dots would resolve
   them.  At zoom > 24 dots drop back to 3px: separation is
   what's scarce there, not visibility.
4. **zoommax raised 128 -> 512 (main.c).**  At 128x on a ~1000px
   window a degree is ~1100px, so same-city stations a few
   hundred meters apart still landed within one dot.  512x gives
   4x the separation.  Safe now that the hit test box-rejects
   before squaring (fix 1); draw-side coordinates stay well
   within int range.  Stations at *identical* coordinates still
   can never be separated by zoom -- that's the standing de-dup
   TODO.

Still open:

- **Real station clustering** (cluster-lite above merges
  same-cell dots at draw time; a fuller version would merge
  near-neighbors, show cluster counts, and cut hit-test cost).

## Solid-earth rendering (IMPLEMENTED, mask-based)

Landed a solid, lit, shaded globe, replacing the flat ocean disc
when data is available -- but via a much simpler route than the
separate tinyrenderer investigation below explored (which was
NOT used/built). Worth reading this section before that one so
the two don't get conflated.

**What it is.** A new offline converter, mkearth.c, rasterizes
coast.dat into an equirectangular (lat/lon) bitmap the same size
class as a world map image, flood-fills ocean from a handful of
known open-ocean seed points (4-connected, wrapping in longitude;
anything the flood can't reach counts as land), and writes it as
`earth.mask`:

    mkearth [-w width] [-t preview.tga] < coast.dat > earth.mask

`-w` sets mask width (height = width/2, default 2048x1024). `-t`
dumps an unlit land/ocean preview .tga for eyeballing the result
before trusting it. mkearth also prints the land percentage of the
generated mask (Earth is ~30% land); a much lower number is the
tell for a leak -- an unclosed ring in coast.dat that let the flood
escape into a landmass and mark it ocean.

**How radioglobe uses it (globe.c, main.c).** `-e file` (else
./earth.mask, else /lib/radio/earth.mask; none found means the
untouched flat-disc path) loads the mask at startup
(`earthinit()`). When loaded, `globedraw()` calls `drawearth()`
instead of drawing the flat ocean disc: for every disc pixel it
inverts the orthographic projection analytically --

    pz = sqrt(1 - px^2 - py^2)
    world = px*ex + py*ey + pz*ez     (ex/ey/ez from viewbasis(), same as coastlines/stations)

-- to get the 3D point on the unit sphere that projects there, looks
up its lat/lon in the mask for land/ocean color, and shades it with
a fixed viewer-attached light (`dl = dot(world, light)`, ambient
floor + diffuse). No mesh, no z-buffer, no per-vertex anything: a
single sphere under orthographic projection has a closed-form
inverse per pixel, which is exactly what makes this route so much
cheaper than a triangle rasterizer for this one shape. Two lookup
tables (`rowlut`: sphere z -> mask row, avoiding per-pixel asin();
`atanlut`: a quantized atan() for a hand-rolled `lutatan2()`, avoiding
libc atan2() per pixel) keep the per-pixel cost to arithmetic plus
two table reads. The composed image is built in a client-side RGB24
buffer and shipped with one `loadimage()` + one `draw()`, not one
draw-device call per pixel.

Everything else is unchanged: the vector coastline outline, the
lat/lon grid, and station dots are still drawn from coast.dat / the
station file on top of the shaded sphere exactly as over the old
flat disc (globedraw() calls drawgrid()/drawcoasts() either way), so
the outline stays exact even though the fill beneath it is a
rasterized approximation. The existing view-cache (`cache`, keyed
on rect/clat/clon/zoom) covers `drawearth()` too -- it only re-runs
when the view actually changes, same as the old ocean-disc fill did.

**Relationship to the investigation below.** The tinyrenderer
exploration (still parked, not built into radioglobe) was aimed at
a general triangle-mesh-based solid globe with texture mapping,
useful if a fully lit/normal-mapped/textured Earth render were
wanted, or if the shape weren't a plain sphere. This implementation
solves the much narrower, much cheaper problem radioglobe actually
has -- "one sphere, orthographic view, need a land/ocean texture" --
directly, without needing a mesh, a rasterizer port, or a photo
texture at all: the "texture" is coast.dat's own vector data, baked
once offline, which is exactly the "recommended starting point" the
investigation below landed on. Consider the mesh/tinyrenderer route
only if radioglobe ever wants something a single analytic sphere
inverse can't give it (e.g. non-spherical geometry, real specular
highlights needing surface normals beyond the sphere's own, or
perspective camera dolly-zoom instead of orthographic scale-zoom).

**Not done / possible follow-ups:** no night-side city-lights
texture, no cloud layer, no seasonal terminator, no bump-mapped
mountains -- the mask is a flat 1-bit land/ocean classification and
the shading is a single fixed lamp direction, deliberately kept as
simple as coast.dat's own outline rendering. mkearth's flood-seed
list is hand-picked (Pacific/S.Atlantic/Indian/Arctic/Drake); a
higher -w together with thinner straits in future coastline data
could in principle pinch one shut again, same caveat mkearth.c's
own comment already flags.

### Real photo texture support (mkearth -i, EARTHTEX) -- IMPLEMENTED

Follow-up landed after using the mask for a while: it's flat-colored
(one fixed green for all land, one fixed blue for all ocean), so
even lit it looks nothing like a satellite photo. Since the render
path (`drawearth()`'s per-pixel inverse projection + lighting) was
always going to need *some* w x h color source at (row, col), the
natural extension was to let that source be a real photographic
equirectangular texture -- e.g. NASA's public-domain "Blue Marble
Next Generation" (visibleearth.nasa.gov) -- instead of only the
computed land/ocean mask.

**mkearth -i imagefile (mkearth.c):** a second, independent mode,
mutually exclusive with the coast.dat path (it reads no stdin and
ignores -w/-t). Converts an already-decoded Plan 9 image(6) file
into an `EARTHTEX` block and exits:

    jpg -9 -t earth.jpg > earth.plan9img   # -9: uncompressed image(6), -t: force RGB
    mkearth -i earth.plan9img > earth.mask

Why decode via jpg/png/tif(1) rather than link a JPEG/PNG/TIFF
library into mkearth: those converters already exist in 9front and
already do exactly this decode-to-image(6) step (see their `-9 -t`
options); mkearth only needs libmemdraw, which was already going to
be reachable for other reasons (memimage's channel-converting draw,
below). One dependency instead of three image codec libraries.

Implementation (mkearth.c:mktexture()): `readmemimage(fd)` (from
libmemdraw, `#pragma lib "libmemdraw.a"` in memdraw.h -- no explicit
mkfile change needed, confirmed by a clean build) loads the image(6)
file into memory *without an open display connection*, which is
exactly what a headless command-line converter needs (this is the
same library the kernel's own draw(3) driver and page(1)/jpg(1) are
built on). Whatever the source's actual channel format turns out to
be, `memimagedraw()` into a freshly allocated RGB24 Memimage handles
the conversion (grayscale, paletted, whatever -t already coerced it
to) via the same channel-conversion machinery draw(2) uses on
screen -- so mkearth doesn't need to special-case input formats
itself. `unloadmemimage()` then hands back the pixel bytes already
in RGB24's own in-memory order (B,G,R per image(6)'s documented
convention for that channel string -- confirmed against globe.c's
existing `ebuf` writes, which already use that exact order with
`/* land: B */`-style comments). No resampling: output w/h are the
source image's own, so whatever resolution you feed in is what
`drawearth()` samples from later -- keep it to a few thousand pixels
wide; the full-resolution multi-tile Blue Marble releases (tens of
thousands of pixels per tile) are unnecessary for an on-screen disc
and slow to load for no visible benefit.

Written format:

    EARTHTEX
    w h
    <w*h*3 raw bytes, row-major from the source image's own row 0
     (must be the north pole, as with any standard equirectangular
     world map): B,G,R per pixel>

**globe.c:** `loadmask()` now recognizes either magic line
(`EARTHMASK` or `EARTHTEX`) and allocates into one of two mutually
exclusive buffers (`emask`, 1 byte/pixel, or `etex`, 3 bytes/pixel);
a new `earthloaded` flag (replacing the old direct `emask != nil`
checks in `globedraw()`) is set on success regardless of which
format loaded. `drawearth()`'s inner pixel loop is unchanged up
through computing `row`/`col` (the rowlut/atanlut machinery doesn't
care what's stored at each cell) and only the final color lookup
branches: `etex != nil` samples the real B,G,R bytes directly and
multiplies by the same `inten` lighting term the flat-color path
already used, so texture mode gets identical shading behavior, just
a richer color source. Everything else -- the view-cache, the
coastline/grid overlay drawn on top, the -e/./earth.mask/
/lib/radio/earth.mask search order -- is untouched; both formats
share the exact same `-e` flag and file slot, so switching between
"a quick land/ocean mask" and "a real satellite photo" is just
regenerating the same earth.mask with a different mkearth
invocation.

**Bilinear texel filtering (IMPLEMENTED, texture path only).**
Confirmed in practice: at radioglobe's max zoom (512x) a single
texel of a several-thousand-pixel-wide Blue Marble image covers
many screen pixels, and the original nearest-neighbor lookup made
that obvious as hard-edged blocks -- worse than the flat land/ocean
mask ever looked, since a mask has no fine detail to lose in the
first place. A bigger mask only pushes the same problem out further
(and costs more memory/load time) rather than fixing it; the actual
fix is interpolating *between* texels instead of snapping to one.

`drawearth()` now computes the continuous (fractional) row/col
position -- `idxf`/`colf` -- instead of only the rounded integer
`row`/`col` the emask path still uses, and when `etex != nil` blends
the 4 neighboring texels by fractional row/col (standard bilinear:
weight each corner by `(1-frac)`/`frac` in both axes). The row
fraction is obtained by interpolating between `rowlut[idx]` and
`rowlut[idx+1]` -- i.e. within the existing LUT -- rather than
calling `asin()` a second time, so the added cost is one more LUT
read plus arithmetic, not trig. Longitude wraps (`col1` mod
`emaskw`, since longitude is a full circle); latitude does not
(`row1` clamps at the last row -- there's no wraparound over a
pole). The emask (land/ocean flat-color) path deliberately keeps
nearest-neighbor: its hard block edges are already covered by the
exact vector coastline drawn on top, so blending there would only
add cost (extra reads/lerps every pixel, every view-changing redraw)
for a blur nobody would see under the crisp outline.

Not pursued further: mipmapping / a proper minification filter for
the opposite case (zoomed *out*, many texels per screen pixel,
where bilinear alone can alias/shimmer on fine detail like small
islands) -- not yet reported as visible in practice, and the
existing coastline LOD stride already reduces the *vector* overlay's
cost at low zoom the same way a mipmap would for the raster fill;
worth adding only if the raster fill itself is seen to shimmer while
zoomed out.

### Deep-zoom blockiness round 2: it was our LUTs, not the texture

Tested on the real Blue Marble texture at 512x: bilinear helped
("softer") but blocks remained, and the obvious suspect -- texture
resolution -- turned out not to be the main culprit.  Do the math
before buying more pixels: on a ~1000px window at 512x, rad is
~256,000 screen px.  lutatan2() was reading its LUT *without
interpolation*, quantizing angles in ~1/Natan (1/1024) rad steps;
its own comment correctly noted that was "well under one mask
column," which is exactly why it was harmless under nearest-neighbor
sampling.  But 1/1024 rad of longitude at 512x projects to ~250
SCREEN pixels -- so the fractional column feeding the new bilinear
blend was itself arriving in texel-sized stair-steps.  The filter
smoothed within each step; the steps stayed.  Same story, milder,
for rowlut, which stored integer (whole-row-quantized) values.

Fix (globe.c): both LUT reads are now linearly interpolated between
adjacent entries, and rowlut stores exact fractional double rows
instead of rounded ushorts.  Quantization error drops from
first-order (1/N per step) to second-order (~1e-7 rad) -- far below
a texel at any reachable zoom -- for one extra multiply-add per
lookup.  The emask nearest-neighbor path is unchanged in behavior
((int) of the same values as before).

Lesson recorded for next time: when a sampling artifact survives a
filtering fix, check the *coordinate pipeline feeding the filter*
before reaching for higher-resolution data.  Every stage upstream of
the blend (LUTs here) must deliver precision finer than a texel at
max zoom, or the filter just interpolates between quantized inputs.

What genuinely remains after this fix is magnification blur, not
blockiness: at 512x a 5400-wide texture texel legitimately covers
~300 screen px at the equator (2*pi*256000/5400), rendered by
bilinear as smooth gradients.  Real *detail* at that zoom would need
on the order of a million-pixel-wide texture -- not loadable whole
(21600x10800x3 is already ~700MB).  The realistic path, if ever
wanted, is tiled multi-resolution loading (fetch/decode only the
visible patch's tiles from the tiled full-res Blue Marble releases,
which NASA distributes exactly for this reason) -- noted as a
possible future direction, deliberately not started.

### Interaction stutter round (solid-earth mode)

Confirmed on real use with the Blue Marble texture: rendering
quality good, but drag/spin stutters and input feels erratic.
Diagnosis is arithmetic, not mystery: drawearth() recomputes every
window pixel on every view-changing frame -- ~1M pixels of sqrt +
interpolated-LUT lookups + bilinear blend, plus a ~3MB loadimage()
per frame -- and once one frame outlasts the 25ms tick, the pacing
machinery (which was designed for the far cheaper vector path)
degrades: ticks coalesce, frames bunch, and mouse events get
processed in bursts between slow frames.  The "erratic" feel is the
same backlog disease documented in the drag-smoothness rounds above,
reintroduced by a heavier per-frame cost.

Two fixes landed (globe.c, one flag through dat.h/main.c):

1. **Scanline span clipping (drawearth()).**  Per row, compute the
   disc's x-extent analytically (|px| <= sqrt(1 - py^2)) and walk
   only that span; the black margins are memset in bulk.  Zoomed
   out, most of the window is margin, so this alone removes most of
   the per-pixel work exactly where the whole hemisphere is visible
   and the texture sampling is at its densest.  Also removes the
   per-pixel outside-disc test everywhere except the span's edge
   pixels.

2. **Half-resolution motion frames (globecoarse()).**  While the
   view is actually in motion (drag or momentum spin -- i.e. the
   dirty-flag path in main.c's Etick), drawearth samples every
   other pixel in x and y and replicates each sample into a 2x2
   block: 4x fewer samples and 4x less loadimage traffic per frame.
   When a tick finds no motion left, main.c renders one final
   full-quality settle frame (needfine flag), so the image you
   actually look at while stationary is always full resolution --
   the softening exists only during motion, where it reads as
   motion blur rather than lost detail.  The globedraw view-cache
   is keyed on the quality flag too (ccoarse), so the settle frame
   re-renders instead of blitting the coarse cache.  Discrete
   actions (zoom, arrows, menu, click) never set coarse and stay
   full quality; the flag is a no-op when no earth is loaded, so
   the plain vector globe is untouched by any of this.

Diagnosis aid: the status bar's ms readout shows the previous
frame's cost.  Motion frames should now be roughly a quarter of
what they were; if drag still stutters, read that number while
dragging and report it -- the next lever, if one is ever needed, is
parallelizing drawearth's row loop across cores (see the
tinyrenderer investigation's fork-per-frame caveat: a persistent
worker pool, not rfork/wait per frame, would be the right shape at
40Hz).

### The real "animation gets out of sync" bug: tick-count physics

Follow-up report after the two fixes above: input itself was NOT
bursty or laggy -- rather "the animation doesn't keep up and gets
out of sync."  That is a different bug from frame cost, and the two
cheap-frame fixes above could only ever hide it.

Cause: the momentum physics was advanced by a FIXED Tickms (25ms)
per tick *event*, and friction was applied once per tick *event*.
So the spin's trajectory was measured in "ticks the application
managed to service," not in wall-clock time.  Whenever a frame
outlasted the tick interval -- which the per-pixel earth render can
easily do -- ticks were serviced less often than every 25ms, so the
spin advanced LESS than real time and (since friction is also
per-tick) decayed over a LONGER wall-clock period than the flick
implied.  Slow frames therefore didn't merely look choppy: they
changed the animation's actual trajectory, which is precisely the
"doesn't keep up / out of sync" symptom.  Two aggravating details in
the same handler: the `ecankbd()` early-out `break`ed *before*
orbittick(), silently discarding that interval's motion outright
(previously noted here as harmless -- it was, for correctness, but
it is exactly this class of drift), and tick coalescing via
`ecanread(Etick)` intentionally serviced one tick per backlog.

Fix, in two parts:

1. **libview (view.h, orbit.c): `Orbit.frictionref`.**  New,
   additive field: the caller's nominal tick length.  When set,
   orbittick() subdivides whatever ms it is handed into steps of at
   most frictionref, so one call with 4x the nominal interval
   behaves *exactly* as four nominal calls would have (a trailing
   partial step gets pow(friction, dt/ref)).  It defaults to 0,
   which keeps the original one-friction-per-call behavior, so
   claudegraph -- the other consumer, whose tick rate it can keep
   up with -- is bit-for-bit unaffected; view.h's "don't change
   signatures casually" contract is honored (no signature changed).
   The legacy body was factored into a static orbitstep() so both
   paths share one copy of the integration/clamp/wrap logic.
2. **main.c: pass real elapsed time.**  The Etick handler now
   measures nsec() since the previous tick and hands orbittick that
   interval, with `orb.frictionref = Tickms` set at init.  Physics
   moved *above* the ecankbd() early-out (it is a few multiplies;
   only redraw is worth skipping), so a pending keystroke no longer
   costs the spin an interval.  Guards: a non-positive delta falls
   back to one nominal tick -- the globebench work in this file
   recorded a real observed nsec() anomaly, one reading ~23s
   negative, so this is not theoretical -- and the delta is capped
   at 250ms so a genuine long stall (window hidden, machine busy)
   makes the globe resume rather than teleport.

Net effect: frame rate now affects only how *smoothly* the spin is
sampled, not where it goes or how long it lasts.  A flick decays
over the same wall-clock time at 8fps as at 40fps.

Regression test added (libview/viewtest.c: testfrictionref):
asserts 4 steps of ref == 1 step of 4*ref with frictionref set,
that ms == frictionref is identical to the legacy single step, that
the frictionref == 0 default still moves 4x as far and decays once
for a 4x call (the claudegraph-compatibility guarantee), and that a
zero interval is a no-op.  Run it with `cd /usr/dave/work/libview &&
mk && ./viewtest` (one PASS/FAIL line per check; exits nil iff all
pass).

## Investigated: solid-shaded globe via software rasterization

Question that prompted this: radioglobe draws coastlines as
*outlines* (globe.c's own comment says why -- "filled continents
require complex polygon clipping against the globe horizon...
the outlines look fine"). Is that a real ceiling, or just the
easier thing to build? Specifically: could a solid, lit, shaded
sphere be rendered instead, using the tinyrenderer-style software
triangle rasterizer ported to 9front at /usr/dave/work/tr/p9
(geom.c/tga.c/model.c/ourgl.c -- a z-buffered, Phong-capable
triangle rasterizer, unrelated to and not currently linked by
radioglobe at all), and would it be fast enough to redraw on
every drag/zoom event the way the current vector approach is?

This was **investigated, not implemented** -- no radioglobe file
was touched. The tool used to check lives in the tinyrenderer
port tree: /usr/dave/work/tr/p9/globebench.c (`mk globebench`).
It generates a UV-sphere mesh in memory (no .obj file needed --
Model's fields are public) and shades it with a deliberately
cheap Gouraud shader (ambient+diffuse only, solid ocean color, no
texture, no normal map, no specular pow()) so the numbers measure
the *floor* cost of "filled shaded sphere," not a fully-dressed
Earth render. It times real frames (camera sweeping a full orbit,
as if dragging) with nsec(), not shell `time`.

### What it found, including two bugs the process itself caught

1. **A benchmark usage bug**: per arg(2), option scanning stops at
   the first argument that doesn't start with `-`. Running
   `globebench 700 -p 4` silently stopped flag parsing at `700`,
   so `-p` was *never applied* in the first round of testing --
   three different `-p` values all secretly ran sequentially.
   Fixed by rejecting stray positional args outright (`argc != 0`
   -> usage()) instead of silently ignoring them.
2. **A timer bug**: one nsec() reading came back ~23 seconds
   *negative* out of 60 frames (nsec() reads /dev/bintime; cause
   unconfirmed, possibly a one-off clock read glitch), which
   silently wrecked the average until the stats code was hardened
   to discard implausible (negative or >10s) deltas rather than
   trust every reading.
3. **A real, fixable inefficiency, found by the numbers not by
   inspection**: the vertex shader was calling
   `mat4invert_transpose(ModelView)` -- a full 4x4 cofactor-
   expansion matrix inversion -- once per *vertex* (12288 times a
   frame for a 4096-triangle sphere) instead of once per frame,
   even though ModelView doesn't change within a frame. Hoisting
   it into the per-frame shader init (semantically a no-op; same
   output) gave a uniform ~41% speedup at every worker count
   tested. The exact same inefficiency exists in the real
   renderer's PhongShader::vertex() at /usr/dave/work/tr/p9/main.c
   (inherited from the original tinyrenderer C++).  **Since
   fixed there too:** that shader now hoists the inversion into
   phonginit() (PhongShader.mvit), with a comment crediting this
   benchmark; nothing left to do on that one.

### Numbers (700x700 disc, 4096-triangle sphere, no texture)

| workers | avg ms/frame | fps  |
|---|---|---|
| 1  | 33.58 | 29.8 |
| 4  | 25.19 | 39.7 |
| 10 | 40.12 | 24.9 (worse than sequential) |

`-p 4` beats sequential but only by ~1.33x, well short of 4x;
`-p 10` is worse than sequential outright. Two causes, both
already-known limitations of the tinyrenderer port rather than
surprises: (a) only fragment/scanline work is split across
workers -- every worker still redundantly re-transforms the
*entire* vertex set every frame, so that redundant cost scales
*up* with worker count and fights the benefit of splitting
fragment work; (b) the benchmark (like the real renderer)
`rfork`s and `wait()`s fresh every single frame, which is fine for
a batch renderer that runs once and exits but is the wrong shape
for a 40Hz-or-more live redraw loop -- the fork/join overhead
itself is a real, nonzero tax 60 times a second. `-p 10` losing
to `-p 4` also suggests this machine has on the order of 4 usable
cores; oversubscribing past that costs more than it returns.

### Verdict

**Not a hard ceiling.** ~30-40fps for a bare shaded sphere (no
texture) is respectable -- draggable and readable, if not
60fps-buttery. It is *nowhere near* as fast as what radioglobe
does today (pure vector point-projection + a handful of
poly()/fillellipse() calls, no per-pixel software shading loop at
all -- almost certainly one or two orders of magnitude faster,
effectively instant). So the honest framing is: solid shading is a
real, buildable option that trades radioglobe's current
unconditionally-smooth interaction for something in the
"responsive but not instant" 25-40fps range, not a technical dead
end. That's a product tradeoff for whoever owns this decision, not
something the numbers settle by themselves. And this was all
without a texture and without the heavier PhongShader-style
normal-mapping/specular a real Earth render would likely want,
which would cost more on top -- see below.

### The texture problem (raised directly: "I don't have great
map/texture data for the earth")

Real concern, and the obvious answer ("go find a Blue Marble JPEG
somewhere") has real friction (network fetch, license-checking,
converting to TGA, and it'd be static/fixed detail regardless of
zoom). But there's a much better option already sitting in this
project: **coast.dat/coast2.dat already IS the land/ocean map** --
it's just stored as vector polylines instead of a raster.
Rasterizing those polylines into an equirectangular (lat/lon)
bitmap -- flat 2D fill, ocean vs. land, once, offline, as a
preprocessing step -- produces exactly the diffuse texture a
solid-shaded sphere needs, using data this project already has and
already trusts, at whatever resolution is wanted, with no network
fetch and no new licensing question.

This sidesteps the exact problem globe.c's own comment flags
("filled continents require complex polygon clipping against the
globe horizon") rather than solving it: clipping a filled polygon
against a *3D view horizon on a rotating sphere, every frame* is
the hard version of this problem. Filling the *same* polygons into
a flat, static, non-rotating raster image *once* is the easy,
textbook version (even-odd or nonzero scanline polygon fill,
long-solved, no per-frame or per-view-angle cost at all) -- and
once it's a flat bitmap, the tinyrenderer-style renderer just
samples it as an ordinary diffuse texture the same way main.c
samples diffuse.tga today. Not built; recorded here because it's
the right next step if solid shading is ever pursued, and because
it directly answers "no good texture data" with "you already have
the data, it's just shaped as vectors."

### "I also zoom in and out... would zoom work?"

Yes, and it's not new work -- it's a knob the tinyrenderer port
already exposes. Today's radioglobe zoom (globegeom(): rad =
basereg * zoom) is a pure 2D scale of the projected disc; it
doesn't move the camera in 3D or introduce perspective
foreshortening. The renderer's `initviewport(x, y, w, h)` (see
/usr/dave/work/tr/p9/ourgl.c) does exactly that same kind of
screen-space scale already, independent of the 3D camera -- so
replicating radioglobe's current zoom feel is a matter of passing
`w = h = size*zoom` into initviewport() with eye/lookat held
fixed, not a new capability that needs designing. (The other
option, actually dollying the camera closer for a perspective
zoom with real parallax, is also possible via lookat()'s eye
distance, but would be a visual style change from radioglobe's
current orthographic look, not a requirement.)

One honest caveat, not a blocker: zooming in doesn't automatically
make a frame cheaper the way it intuitively might. Fragment/pixel
cost does shrink or grow with the visible area as expected, but
*vertex-stage setup cost is currently paid for the entire mesh
every frame regardless of what's actually on screen* (same
limitation as the parallelism scaling issue above -- nothing culls
or LODs the vertex pass). At deep zoom (radioglobe already goes up
to 512x) most of a sphere mesh would be off-screen but still fully
processed. This is exactly the class of problem globe.c/coast.c
already solved for the vector path -- bounding-cap culling
(capvisible()) and LOD striding (meanstep()-driven stride in
drawcoasts()) -- so there's clear precedent in this codebase for
how to fix it, if a mesh-based renderer is ever built: the same
two ideas (skip what can't be visible, reduce detail below
sub-pixel spacing) apply to triangles as much as to polyline
points. Not needed to answer "would zoom work" (it would), only
relevant to "would zoom make it faster" (not automatically, yet).

### Status: parked (superseded for radioglobe's actual need)

No radioglobe code was changed by this investigation, and still
hasn't been -- this section remains an exploration of the general
tinyrenderer/mesh route, kept for reference. The "texture-from-
coast.dat" idea it landed on as the recommended starting point *was*
picked up, but by a much smaller, purpose-built path instead of the
mesh renderer: see "Solid-earth rendering (IMPLEMENTED, mask-based)"
above, which added mkearth.c + globe.c:drawearth() using a per-pixel
analytic sphere inverse (no mesh, no z-buffer, no fork-per-frame
renderer process). That fully covers radioglobe's actual want ("a
shaded globe instead of the flat disc, textured from data we
already trust"). The mesh/tinyrenderer route stays parked and would
only be worth revisiting for something an analytic single-sphere
inverse genuinely can't do -- see that section's last paragraph.

## TODO / ideas

- mixfs for playback (auto-resample + multiple simultaneous
  streams) instead of the pcmconv pipeline.
- ICY now-playing metadata (zuke has icy.c to borrow).
- Keyboard station search.
- Filled landmasses: DONE via a mask, not polygon clipping -- see
  "Solid-earth rendering (IMPLEMENTED, mask-based)" above
  (mkearth.c + globe.c:drawearth(), -e flag / earth.mask). The
  mesh/tinyrenderer route explored separately in "Investigated:
  solid-shaded globe via software rasterization" remains parked
  and unused.
  - Possible follow-ups on the mask itself: night-side city
    lights, clouds, a seasonal terminator, bump-mapped terrain --
    none implemented, see the notes above.
- Cache fetched data, maybe a refresh command in the menu.
- Cluster/de-dup nearby station dots when zoomed out.
- Momentum/inertial spin on drag release: DONE (see above);
  still want to hand-tune friction/vmin/Tickms against a real
  run for feel.

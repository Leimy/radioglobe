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

## TODO / ideas

- mixfs for playback (auto-resample + multiple simultaneous
  streams) instead of the pcmconv pipeline.
- ICY now-playing metadata (zuke has icy.c to borrow).
- Keyboard station search.
- Filled landmasses (needs polygon clipping vs horizon circle).
- Cache fetched data, maybe a refresh command in the menu.
- Cluster/de-dup nearby station dots when zoomed out.

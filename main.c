#include <u.h>
#include <libc.h>
#include <draw.h>
#include <event.h>
#include <keyboard.h>
#include "view.h"
#include "dat.h"

/*
 * radioglobe - an interactive orthographic globe with radio station dots.
 *
 * Drag to rotate, scroll to zoom, click a station to stream it.
 * Stations: lat lon name url (one per line, '#' comments).
 *
 * Etick is a timer event key for momentum/inertial spin.
 * Emouse==1 and Ekeyboard==2 are taken by event(2); Etick uses 4.
 */
enum {
	Mplay,
	Mstop,
	Mreload,
	Mexit,
};

enum {
	Etick = 4,
	Tickms = 25,		/* timer interval (~40Hz) */
};

char *menuitems[] = {
	"play",
	"stop",
	"reload",
	"exit",
	nil,
};

Menu menu = { menuitems };

Orbit orb;

Station *stations;
int nstation;
int selstation = -1;
int playing = -1;
int streampid = -1;
Image *statbg;
static Image *back;	/* offscreen frame buffer */
static int dirty;	/* view changed; render on next Etick */
static int needfine;	/* need full-quality settle frame */
static long frametime;	/* previous frame cost, ms */

/* Sample rate of /dev/audio in Hz.  Decoders emit s16c2r44100;
 * at any other rate, a pcmconv stage resamples to devrate.
 */
int devrate = 44100;

static void
stopstream(void)
{
	if(streampid > 0){
		postnote(PNGROUP, streampid, "kill");
		streampid = -1;
	}
	playing = -1;
}

static void
startstream(int idx)
{
	int pid;
	char *qurl, *cmd;

	stopstream();
	if(idx < 0 || idx >= nstation)
		return;

	/*
	 * play(1) fetches URL via hget/webfs, detects format, runs decoder.
	 * RFCFDG: fresh fd table. RFNOTEG: kills the whole pipeline.
	 * RFNOWAIT: no zombie to reap.
	 */
	qurl = quotestrdup(stations[idx].url);
	if(qurl == nil)
		sysfatal("quotestrdup: %r");
	if(devrate == 44100)
		cmd = smprint("play %s </dev/null", qurl);
	else
		cmd = smprint("play -o /fd/1 %s </dev/null | "
			"audio/pcmconv -o s16c2r%d >/dev/audio", qurl, devrate);
	free(qurl);
	if(cmd == nil)
		sysfatal("smprint: %r");

	pid = rfork(RFPROC|RFCFDG|RFNOTEG|RFNOWAIT);
	if(pid == 0){
		int nfd;

		nfd = open("/dev/null", OREAD);		/* 0 */
		if(nfd != 0 && nfd >= 0){ dup(nfd, 0); close(nfd); }
		nfd = open("/dev/null", OWRITE);	/* 1 */
		if(nfd != 1 && nfd >= 0){ dup(nfd, 1); close(nfd); }
		nfd = open("/dev/cons", OWRITE);	/* 2 */
		if(nfd < 0) nfd = open("/dev/null", OWRITE);
		if(nfd != 2 && nfd >= 0){ dup(nfd, 2); close(nfd); }

		execl("/bin/rc", "rc", "-c", cmd, nil);
		exits("exec");
	}
	free(cmd);
	if(pid > 0){
		streampid = pid;
		playing = idx;
	}
}

static Rectangle
globerect(void)
{
	Rectangle gr;
	int h;

	h = font->height + 8;
	gr = screen->r;
	gr.max.y -= h;
	return gr;
}

static int
findstation(Point xy)
{
	return stationhit(globerect(), orb.pitch, orb.yaw, orb.zoom, xy, stations, nstation, selstation);
}

static char *
skipws(char *s)
{
	return s + strspn(s, " \t");
}

/*
 * Parse a decimal number occupying the whole of s and lying in
 * [lo, hi].  atof() would turn garbage into 0 and accept a
 * latitude of 400, which trigonometric periodicity then folds
 * onto a plausible but wrong spot on the sphere.
 */
static int
parsecoord(char *s, double lo, double hi, double *v)
{
	char *e;

	*v = strtod(s, &e);
	if(e == s || *e != 0)
		return -1;
	if(isNaN(*v))
		return -1;
	if(isInf(*v, 0))
		return -1;
	if(*v < lo || *v > hi)
		return -1;
	return 0;
}

static int
loadstations(char *path)
{
	int fd, n, cap, len, lineno, nbad;
	char *buf, *s, *e, *slat, *slon, *name, *url;
	char *last, *t, *u, *v;
	double lat, lon;
	long sz;
	Dir *d;

	fd = open(path, OREAD);
	if(fd < 0)
		return -1;
	d = dirfstat(fd);
	if(d == nil){
		close(fd);
		return -1;
	}
	sz = d->length;
	free(d);
	if(sz == 0)
		sz = 65536;
	buf = malloc(sz + 1);
	if(buf == nil){
		close(fd);
		return -1;
	}
	n = readn(fd, buf, sz);
	close(fd);
	if(n <= 0){
		free(buf);
		return -1;
	}
	buf[n] = 0;

	/* free the old list, including the strduped strings */
	for(n = 0; n < nstation; n++){
		free(stations[n].name);
		free(stations[n].url);
	}
	free(stations);
	stations = nil;
	nstation = 0;
	cap = 0;
	lineno = 0;
	nbad = 0;

	for(s = buf; s && *s; s = e){
		lineno++;
		e = strchr(s, '\n');
		if(e)
			*e++ = 0;
		s = skipws(s);
		if(*s == '#' || *s == 0)
			continue;

		/* parse: lat lon "name" url */
		slat = s;
		s += strcspn(s, " \t");
		if(*s) *s++ = 0;
		s = skipws(s);

		slon = s;
		s += strcspn(s, " \t");
		if(*s) *s++ = 0;
		s = skipws(s);

		/* parse: quoted name or unquoted name up to the last token (URL) */
		if(*s == '"'){
			s++;
			name = s;
			while(*s && *s != '"') s++;
			if(*s == '"') *s++ = 0;
			s = skipws(s);
			url = s;
		} else {
			/* unquoted: name is everything before the last token (URL) */
			name = s;
			last = nil;
			for(t = s; *t; t++){
				if(*t == ' ' || *t == '\t'){
					u = t + strspn(t, " \t");
					if(*u){
						v = u + strcspn(u, " \t");
						v += strspn(v, " \t");
						if(!*v){
							last = t;
							break;
						}
					}
				}
			}
			if(last == nil){
				nbad++;
				continue;
			}
			*last = 0;
			url = skipws(last + 1);
		}

		/* trim trailing whitespace from url */
		len = strlen(url);
		while(len > 0 && (url[len-1] == ' ' || url[len-1] == '\t'
			|| url[len-1] == '\r'))
			url[--len] = 0;
		if(!url[0]){
			nbad++;
			continue;
		}

		if(parsecoord(slat, -90.0, 90.0, &lat) < 0 ||
		   parsecoord(slon, -180.0, 180.0, &lon) < 0){
			fprint(2, "%s:%d: bad coordinates, line skipped\n", path, lineno);
			nbad++;
			continue;
		}

		if(nstation >= cap){
			cap = cap ? cap * 2 : 64;
			stations = realloc(stations, cap * sizeof(Station));
			if(stations == nil)
				sysfatal("realloc: %r");
		}
		stations[nstation].geo.lat = lat;
		stations[nstation].geo.lon = lon;
		geo2vec(stations[nstation].geo, &stations[nstation].vec);
		stations[nstation].name = strdup(name);
		stations[nstation].url = strdup(url);
		if(stations[nstation].name == nil || stations[nstation].url == nil)
			sysfatal("strdup: %r");
		nstation++;
	}
	free(buf);
	if(nbad > 0)
		fprint(2, "%s: %d malformed line%s skipped\n", path, nbad, nbad == 1 ? "" : "s");
	return nstation;
}

static void
drawstatusbar(Image *dst)
{
	Rectangle r;
	int h, w;
	static int lastw;
	char buf[256];
	Point p;

	h = font->height + 8;
	r.min.x = screen->r.min.x;
	r.max.x = screen->r.max.x;
	r.max.y = screen->r.max.y;
	r.min.y = r.max.y - h;

	if(playing >= 0 && playing < nstation){
		if(selstation >= 0 && selstation != playing)
			snprint(buf, sizeof buf, "> %s  |  %s",
				stations[playing].name,
				stations[selstation].name);
		else
			snprint(buf, sizeof buf, "> %s",
				stations[playing].name);
	} else if(selstation >= 0 && selstation < nstation)
		snprint(buf, sizeof buf, "  %s  [%.1f, %.1f]",
			stations[selstation].name,
			stations[selstation].geo.lat,
			stations[selstation].geo.lon);
	else
		snprint(buf, sizeof buf, "  [%.1f, %.1f]  zoom %.1fx  %ldms",
			orb.pitch, orb.yaw, orb.zoom, frametime);

	/*
	 * Light path (dst == screen): erase only as far as the new
	 * and the previous text reached, not the whole strip.  Full
	 * recomposes (dst == back) paint the whole strip, so back
	 * never accumulates stale tails.
	 */
	w = stringwidth(font, buf) + 12;
	if(dst == screen){
		if(w > lastw)
			lastw = w;
		if(r.min.x + lastw < r.max.x)
			r.max.x = r.min.x + lastw;
	}else
		lastw = w;

	draw(dst, r, statbg ? statbg : display->black, nil, ZP);
	p = Pt(r.min.x + 6, r.min.y + 4);
	string(dst, p, display->white, ZP, font, buf);
}

static Rectangle selrect;	/* screen pixels covered by the selection overlay */

/*
 * Full frame: compose globe + stations + status bar offscreen in
 * back, blit once (drawing straight to screen flickers when the
 * draw command buffer auto-flushes mid-frame), then paint the
 * selection overlay on the screen only.  Invariant: back never
 * contains overlay pixels, so redrawsel() can erase an old overlay
 * by restoring from it.
 */
static void
redraw(void)
{
	Rectangle gr;
	vlong t0;

	t0 = nsec();

	if(back == nil || !eqrect(back->r, screen->r)){
		freeimage(back);
		back = allocimage(display, screen->r, screen->chan, 0, DNofill);
		if(back == nil)
			sysfatal("allocimage: %r");
	}

	gr = globerect();
	globedraw(back, gr, orb.pitch, orb.yaw, orb.zoom);
	drawstations(back, gr, orb.pitch, orb.yaw, orb.zoom, stations, nstation);
	drawstatusbar(back);
	draw(screen, screen->r, back, nil, screen->r.min);
	selrect = drawsel(screen, gr, orb.pitch, orb.yaw, orb.zoom,
		stations, nstation, selstation);
	flushimage(display, 1);
	frametime = (nsec() - t0) / 1000000;
}

/*
 * Selection-only update: the view is unchanged, so back is still
 * exact.  Restore the old overlay's pixels from back, draw the new
 * overlay and the status bar; no other screen pixel is written.
 * Only valid when no view redraw is pending (see the hover code).
 */
static void
redrawsel(void)
{
	if(back == nil){
		redraw();
		return;
	}
	if(Dx(selrect) > 0 && Dy(selrect) > 0)
		draw(screen, selrect, back, nil, selrect.min);
	selrect = drawsel(screen, globerect(), orb.pitch, orb.yaw, orb.zoom,
		stations, nstation, selstation);
	drawstatusbar(screen);
	flushimage(display, 1);
}

/*
 * Instant preview for a zoom-only change: affine-scale the cached
 * static layer (globezoomwarp) instead of re-rendering it, then
 * composite the cheap layers on top.  Always marks the view dirty
 * so the next Etick does a real render; that real render is what
 * re-anchors the cache, so successive previews never compound
 * their resampling error.  If no preview is possible (no cache,
 * view rotated, zoom drifted too far) draw nothing and leave it to
 * the tick: an input handler must never pay for a per-pixel
 * render.  Rejected alternatives: NOTES.md.
 */
static void
zoomredraw(void)
{
	Rectangle gr;

	dirty = 1;
	if(back == nil || !eqrect(back->r, screen->r))
		return;

	gr = globerect();
	if(!globezoomwarp(back, gr, orb.pitch, orb.yaw, orb.zoom))
		return;

	drawstations(back, gr, orb.pitch, orb.yaw, orb.zoom, stations, nstation);
	drawstatusbar(back);
	draw(screen, screen->r, back, nil, screen->r.min);
	selrect = drawsel(screen, gr, orb.pitch, orb.yaw, orb.zoom,
		stations, nstation, selstation);
	flushimage(display, 1);
}

void
eresized(int new)
{
	if(new && getwindow(display, Refnone) < 0)
		sysfatal("getwindow: %r");
	redraw();
}

static void
usage(void)
{
	fprint(2, "usage: %s [-s stationfile] [-c coastfile] [-e earthmask] [-r devrate]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	Event ev;
	Mouse m;
	int e, oldbuttons;
	int cx, cy, rad;
	char *stationfile, *cfile, *efile;
	vlong now, lasttick;
	double dtms;

	stationfile = "/lib/radio/stations";
	cfile = nil;
	efile = nil;

	ARGBEGIN{
	case 's':
		stationfile = EARGF(usage());
		break;
	case 'c':
		cfile = EARGF(usage());
		break;
	case 'e':
		efile = EARGF(usage());
		break;
	case 'r':
		devrate = atoi(EARGF(usage()));
		if(devrate <= 0)
			devrate = 44100;
		break;
	default:
		usage();
	}ARGEND

	if(initdraw(nil, nil, "radioglobe") < 0)
		sysfatal("initdraw: %r");
	einit(Emouse | Ekeyboard);
	if(etimer(Etick, Tickms) != Etick)
		fprint(2, "warning: etimer failed, no momentum spin\n");

	if(cfile != nil)
		coastfile(cfile);
	coastinit();
	if(efile != nil)
		earthfile(efile);
	earthinit();
	globeinit();

	statbg = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x141414ff);
	if(statbg == nil)
		sysfatal("allocimage: %r");

	if(loadstations(stationfile) < 0)
		fprint(2, "warning: could not load %s: %r\n", stationfile);

	orbitinit(&orb, 0.0, 30.0, 1.0);
	/* frictionref: spin is wall-clock, not frame-count.
	 * zoommax 512: separates same-city clusters; stationhit()
	 * box-rejects before squaring so radii don't overflow int.
	 */
	orb.frictionref = Tickms;
	orb.zoommin = 0.5;
	orb.zoommax = 512.0;
	oldbuttons = 0;
	lasttick = nsec();

	redraw();

	for(;;){
		e = event(&ev);

		switch(e){
		case Emouse:
			m = ev.mouse;

			/* scroll zoom; keep oldbuttons current for edge detection */
			if(m.buttons & 8){
				orbitzoom(&orb, 1.15);
				zoomredraw();
				oldbuttons = m.buttons;
				break;
			}
			if(m.buttons & 16){
				orbitzoom(&orb, 1.0/1.15);
				zoomredraw();
				oldbuttons = m.buttons;
				break;
			}

			/* left button: drag to rotate */
			if(m.buttons & 1){
				if(!orb.dragging && !(oldbuttons & 1)){
					/* button-down edge: grab stops any spin */
					orbitdown(&orb, m.xy.x, m.xy.y, m.msec);
				} else {
					/* motion: update orbit, mark dirty; Etick renders once per tick */
					globegeom(globerect(), orb.zoom, &cx, &cy, &rad);
					orbitmove(&orb, m.xy.x, m.xy.y, m.msec,
						180.0 / rad);
					dirty = 1;
				}
			} else {
				/* button 1 up: end drag, keep velocity; edge detection works despite scroll overwriting oldbuttons */
				if(orb.dragging)
					orbitup(&orb);
			}

			/* middle button: select station */
			if((m.buttons & 2) && !(oldbuttons & 2)){
				int s = findstation(m.xy);
				if(s >= 0){
					selstation = s;
					startstream(s);
				}
				redraw();
			}

			/* right button: menu */
			if(m.buttons & 4){
				int sel = emenuhit(3, &m, &menu);
				switch(sel){
				case Mplay:
					if(selstation >= 0)
						startstream(selstation);
					break;
				case Mstop:
					stopstream();
					break;
				case Mreload:
					/* stop stream (indices meaningless across reload); old list preserved on failure */
					stopstream();
					if(loadstations(stationfile) < 0)
						fprint(2, "reload: could not load %s: %r\n", stationfile);
					selstation = -1;
					break;
				case Mexit:
					stopstream();
					exits(nil);
				}
				redraw();
			}

			/* hover: light path unless a view redraw is pending */
			if(m.buttons == 0 && !orb.dragging){
				int s = findstation(m.xy);
				if(s != selstation){
					selstation = s;
					if(!dirty)
						redrawsel();
				}
			}

			oldbuttons = m.buttons;
			break;

		case Ekeyboard:
			switch(ev.kbdc){
			case 'q':
			case Kdel:
				stopstream();
				exits(nil);
			case ' ':
			case '\n':
				if(selstation >= 0)
					startstream(selstation);
				redraw();
				break;
			case 's':
			case '.':
				stopstream();
				redraw();
				break;
			case Kleft:
				orb.yaw -= 10.0 / orb.zoom;
				orbitnorm(&orb);
				redraw();
				break;
			case Kright:
				orb.yaw += 10.0 / orb.zoom;
				orbitnorm(&orb);
				redraw();
				break;
			case Kup:
				orb.pitch += 10.0 / orb.zoom;
				orbitnorm(&orb);
				redraw();
				break;
			case Kdown:
				orb.pitch -= 10.0 / orb.zoom;
				orbitnorm(&orb);
				redraw();
				break;
			case '+':
			case '=':
				orbitzoom(&orb, 1.3);
				zoomredraw();
				break;
			case '-':
				orbitzoom(&orb, 1.0/1.3);
				zoomredraw();
				break;
			case '0':
				/* reset view; also stop any spin, or it would carry on from the reset position */
				orb.zoom = 1.0;
				orb.pitch = 30.0;
				orb.yaw = 0.0;
				orb.vyaw = 0.0;
				orb.vpitch = 0.0;
				orbitnorm(&orb);
				redraw();
				break;
			}
			break;

		case Etick:
			/* advance spin by real elapsed time; guards against nsec() anomalies and long stalls */
			now = nsec();
			dtms = (now - lasttick) / 1000000.0;
			lasttick = now;
			if(dtms <= 0.0)
				dtms = Tickms;
			if(dtms > 250.0)
				dtms = 250.0;

			/* physics always runs; only the redraw is skippable */
			if(orbittick(&orb, dtms))
				dirty = 1;

			/* skip redraw if a keystroke is waiting or another tick is queued */
			if(ecankbd() || ecanread(Etick))
				break;

			/* view-changing redraws: render once per tick max. Coarse during motion, settle frame on stop. */
			if(dirty){
				dirty = 0;
				globecoarse(1);
				redraw();
				globecoarse(0);
				needfine = 1;
			}
			else if(needfine && !orb.dragging){
				needfine = 0;
				redraw();
			}
			break;
		}
	}
}

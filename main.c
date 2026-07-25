#include <u.h>
#include <libc.h>
#include <draw.h>
#include <event.h>
#include <keyboard.h>
#include "view.h"
#include "dat.h"

/*
 * radioglobe - an internet radio globe for 9front
 *
 * presents an interactive orthographic globe projection
 * with radio station dots. click a station to stream it.
 * drag to rotate the globe. scroll to zoom.
 *
 * stations are read from a simple text file:
 *   lat lon name url
 * one per line, '#' comments.
 */

enum {
	Mplay,
	Mstop,
	Mreload,
	Mexit,
};

/*
 * Etick: timer event key for momentum/inertial spin.  Emouse==1 and
 * Ekeyboard==2 are already taken by event(2); 4 is the next power
 * of two and isn't used elsewhere, so we can request it explicitly
 * from etimer() and still switch on it as a compile-time constant.
 */
enum {
	Etick = 4,
	Tickms = 25,		/* ~40Hz */
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
static Image *back;	/* offscreen frame buffer; whole frame is composed here */
static int dirty;	/* view/selection changed; render on next tick (see Etick) */
static int needfine;	/* last render was coarse; render a full-quality settle frame when motion stops */
static long frametime;	/* previous redraw's cost in ms; shown in the status bar */

/*
 * Output sample rate for the audio device, in Hz.  The standard
 * decoders emit 44100; many AAC streams are natively 48000.  We
 * pass everything through audio/pcmconv to the device rate so a
 * decoder/device rate mismatch can't make playback sound sped up
 * or slowed down.  Override with -r if /dev/audio runs at 48000.
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
	char *qurl;
	char cmd[4096];

	stopstream();
	if(idx < 0 || idx >= nstation)
		return;

	/*
	 * Build an rc pipeline:
	 *
	 *   play -o /fd/1 URL </dev/null | audio/pcmconv -o s16c2rRATE >/dev/audio
	 *
	 * play(1) still does URL fetch (hget/webfs), format detection,
	 * and decoding (mp3dec/oggdec/aacdec/...), but with -o /fd/1 it
	 * writes raw PCM to stdout instead of opening /dev/audio itself.
	 * pcmconv then resamples that PCM to the device rate before it
	 * reaches /dev/audio.  This is what fixes "sped up" playback:
	 * a decoder emitting 48000Hz into a 44100Hz device plays ~9%
	 * fast; pcmconv converts the rate so it always matches.
	 *
	 * RFCFDG: child has independent, clean file descriptor table
	 * RFNOTEG: new note group so postnote(PNGROUP) kills the whole
	 *   pipeline (play, hget, decoder, pcmconv).
	 * RFNOWAIT: don't block the parent.
	 */
	qurl = quotestrdup(stations[idx].url);
	if(qurl == nil)
		sysfatal("quotestrdup: %r");
	snprint(cmd, sizeof cmd,
		"play -o /fd/1 %s </dev/null | "
		"audio/pcmconv -o s16c2r%d >/dev/audio",
		qurl, devrate);
	free(qurl);

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

static int
loadstations(char *path)
{
	int fd, n, cap, len;
	char *buf, *s, *e, *slat, *slon, *name, *url;
	char *last, *t, *u, *v;
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

	for(s = buf; s && *s; s = e){
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

		/* name: if quoted, read to closing quote */
		if(*s == '"'){
			s++;
			name = s;
			while(*s && *s != '"') s++;
			if(*s == '"') *s++ = 0;
			s = skipws(s);
			url = s;
		} else {
			name = s;
			/*
			 * name is everything up to the last
			 * whitespace-separated token (the url)
			 */
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
			if(last == nil)
				continue;
			*last = 0;
			url = skipws(last + 1);
		}

		/* trim trailing whitespace from url */
		len = strlen(url);
		while(len > 0 && (url[len-1] == ' ' || url[len-1] == '\t'
			|| url[len-1] == '\r'))
			url[--len] = 0;
		if(!url[0])
			continue;

		if(nstation >= cap){
			cap = cap ? cap * 2 : 64;
			stations = realloc(stations, cap * sizeof(Station));
			if(stations == nil)
				sysfatal("realloc: %r");
		}
		stations[nstation].geo.lat = atof(slat);
		stations[nstation].geo.lon = atof(slon);
		geo2vec(stations[nstation].geo, &stations[nstation].vec);
		stations[nstation].name = strdup(name);
		stations[nstation].url = strdup(url);
		if(stations[nstation].name == nil || stations[nstation].url == nil)
			sysfatal("strdup: %r");
		nstation++;
	}
	free(buf);
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
	 * On the light path (dst == screen) erase only as far as
	 * the text reaches -- and as far as the previous text
	 * reached -- instead of rewriting the full window-width
	 * strip on every hover change; it's the biggest write that
	 * path makes.  Full recomposes (dst == back) still paint
	 * the whole strip, so back never accumulates stale tails.
	 */
	w = stringwidth(font, buf) + 12;
	if(dst == screen){
		if(w > lastw)
			lastw = w;
		if(r.min.x + lastw < r.max.x)
			r.max.x = r.min.x + lastw;
		lastw = w;
	}else
		lastw = w;

	draw(dst, r, statbg ? statbg : display->black, nil, ZP);
	p = Pt(r.min.x + 6, r.min.y + 4);
	string(dst, p, display->white, ZP, font, buf);
}

static Rectangle selrect;	/* screen pixels covered by the selection overlay */

static void
redraw(void)
{
	Rectangle gr;
	vlong t0;

	t0 = nsec();

	/*
	 * Compose the whole frame offscreen, then blit it to the
	 * window in one draw.  Drawing straight to screen flickers
	 * at large station counts: libdraw's command buffer fills
	 * up mid-frame and each auto-flush exposes a half-drawn
	 * frame.  back shares screen's coordinate system, so all
	 * screen->r-based geometry stays valid.
	 */
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
	/*
	 * The selection overlay goes to the screen only, after the
	 * blit, so back always holds the selection-free base frame:
	 * redrawsel() erases an old overlay by restoring its pixels
	 * from back, which must not itself contain overlay pixels.
	 */
	selrect = drawsel(screen, gr, orb.pitch, orb.yaw, orb.zoom,
		stations, nstation, selstation);
	flushimage(display, 1);
	frametime = (nsec() - t0) / 1000000;
}

/*
 * Selection-only update: the view didn't change, so the base
 * frame in back is still exact.  Erase the old overlay by
 * restoring its pixels from back, draw the new overlay and the
 * status bar, and leave every other pixel on screen untouched.
 * Hovering across stations costs a couple of dot-sized writes
 * instead of a full-window recompose that repainted every dot
 * on screen to change one of them.
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

	if(loadstations(stationfile) < 0)
		fprint(2, "warning: could not load %s: %r\n", stationfile);

	orbitinit(&orb, 0.0, 30.0, 1.0);
	/*
	 * Drive the spin off real elapsed time, not tick count: with
	 * frictionref set, orbittick subdivides whatever ms we hand
	 * it into Tickms-sized steps (see view.h), so a late tick
	 * applies exactly the motion its elapsed time deserves.
	 * Without this the trajectory depended on how many ticks we
	 * managed to service -- with the per-pixel earth render a
	 * frame can outlast the tick interval, so the spin advanced
	 * slower than real time and decayed over a longer wall-clock
	 * period than the flick implied: visibly "not keeping up."
	 */
	orb.frictionref = Tickms;
	orb.zoommin = 0.5;
	/*
	 * 512x resolves same-city station clusters that 128x left
	 * merged: at 128x on a ~1000px window a degree is ~1100px,
	 * so stations a few hundred meters apart still landed
	 * within one dot.  Safe now that stationhit() box-rejects
	 * before squaring pixel distances (at these radii the
	 * squares overflowed int).
	 */
	orb.zoommax = 512.0;
	oldbuttons = 0;
	lasttick = nsec();	/* baseline for the Etick elapsed-time measurement */

	redraw();

	for(;;){
		e = event(&ev);

		switch(e){
		case Emouse:
			m = ev.mouse;

			/*
			 * scroll wheel zoom; keep oldbuttons up to date
			 * even on this early exit, so a chorded
			 * scroll+button event can't retrigger the
			 * button edge detection below on the next event.
			 */
			if(m.buttons & 8){
				orbitzoom(&orb, 1.15);
				redraw();
				oldbuttons = m.buttons;
				break;
			}
			if(m.buttons & 16){
				orbitzoom(&orb, 1.0/1.15);
				redraw();
				oldbuttons = m.buttons;
				break;
			}

			/* left button: drag to rotate */
			if(m.buttons & 1){
				if(!orb.dragging && !(oldbuttons & 1)){
					/* button-down edge: grab stops any spin */
					orbitdown(&orb, m.xy.x, m.xy.y, m.msec);
				} else {
					/*
					 * motion while held: compute the same
					 * radius the renderer uses so drag
					 * tracks the cursor exactly 1:1.
					 *
					 * Don't redraw here: at large dataset
					 * scale a redraw outlasts the interval
					 * between mouse events, so redrawing
					 * per event backlogs the queue and
					 * renders a parade of stale positions.
					 * Just update the orbit (cheap; every
					 * sample still feeds the velocity
					 * tracker, so flick momentum is
					 * unaffected) and let the next Etick
					 * render the latest position once.
					 */
					globegeom(globerect(), orb.zoom, &cx, &cy, &rad);
					orbitmove(&orb, m.xy.x, m.xy.y, m.msec,
						180.0 / rad);
					dirty = 1;
				}
			} else {
				if(orb.dragging){
					/*
					 * No button 1: end any drag, keeping
					 * the launch velocity.  Deliberately
					 * not an oldbuttons edge test -- the
					 * scroll branches above break early
					 * and overwrite oldbuttons, so a
					 * release during a scroll chord would
					 * lose the edge and leave the drag
					 * stuck on.
					 */
					orbitup(&orb);
					/* any pending final position renders on the next tick */
				}
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
					/*
					 * indices into stations[] are meaningless
					 * across a reload; stopping the stream is
					 * less surprising than a status bar that
					 * names the wrong station.
					 */
					stopstream();
					loadstations(stationfile);
					selstation = -1;
					break;
				case Mexit:
					stopstream();
					exits(nil);
				}
				redraw();
			}

			/*
			 * hover: find nearest station.  A selection
			 * change doesn't touch the view, so it takes
			 * the light path: restore + overlay + status
			 * bar, a few tiny writes.  Cheap enough to do
			 * per event, and no other pixel on screen is
			 * written at all.
			 */
			if(m.buttons == 0 && !orb.dragging){
				int s = findstation(m.xy);
				if(s != selstation){
					selstation = s;
					/*
					 * If a view change is already pending
					 * (momentum tick advanced the orbit
					 * but its redraw hasn't run yet),
					 * back and the screen lag the orbit
					 * state; the queued full redraw will
					 * include the new selection anyway.
					 */
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
				redraw();
				break;
			case '-':
				orbitzoom(&orb, 1.0/1.3);
				redraw();
				break;
			case '0':
				orb.zoom = 1.0;
				orb.pitch = 30.0;
				orb.yaw = 0.0;
				orbitnorm(&orb);
				redraw();
				break;
			}
			break;

		case Etick:
			/*
			 * Advance the spin by the time that actually
			 * elapsed since the previous tick, not by a
			 * nominal Tickms: ticks run late whenever a
			 * frame outlasts the tick interval, and
			 * charging each one a flat 25ms made the
			 * animation lag real time and overstay its
			 * decay.  orb.frictionref (set above) makes
			 * orbittick subdivide this correctly.
			 */
			now = nsec();
			dtms = (now - lasttick) / 1000000.0;
			lasttick = now;
			/*
			 * Guards: a bogus clock reading shouldn't
			 * teleport the globe (NOTES.md records a real
			 * observed nsec() anomaly -- one reading ~23s
			 * negative -- during the globebench work), and
			 * neither should a genuine long stall (window
			 * hidden, machine busy).  Fall back to one
			 * nominal tick, and cap the catch-up.
			 */
			if(dtms <= 0.0)
				dtms = Tickms;
			if(dtms > 250.0)
				dtms = 250.0;
			/*
			 * Physics first, unconditionally: it is a
			 * handful of multiplies, and skipping it (as
			 * the old ecankbd() early-out below did, by
			 * breaking before this point) silently dropped
			 * that interval's motion.  Only the expensive
			 * part -- redraw -- is worth skipping.
			 */
			if(orbittick(&orb, dtms))
				dirty = 1;
			/*
			 * If a keystroke (e.g. quit) is already
			 * waiting, skip this tick's redraw and let
			 * the loop go straight back to event() to
			 * handle it. redraw() runs synchronously and
			 * isn't free at large dataset scale; without
			 * this check a fast spin can keep retriggering
			 * redraws back-to-back and a pending keypress
			 * can keep "just missing its turn" until the
			 * spin fully decays. event(2) already
			 * prioritizes keyboard over the timer when
			 * both are ready -- this just avoids doing
			 * more (possibly slow) work before giving it
			 * the chance to.
			 */
			if(ecankbd())
				break;
			/*
			 * Ticks pile up behind a slow redraw.  This
			 * tick's physics is applied above, but if
			 * another tick is already queued, let it do
			 * the drawing: one redraw for the whole
			 * backlog, with fully advanced state, instead
			 * of a back-to-back burst of stale frames.
			 */
			if(ecanread(Etick))
				break;
			/*
			 * All view-changing redraws -- drag motion and
			 * momentum spin -- funnel through the dirty
			 * flag: mouse events just mark the view stale,
			 * and we render the latest state here, once per
			 * tick.  Redraw rate is bounded at the tick
			 * rate, the event queue can't back up behind
			 * slow redraws, and every frame shows current
			 * state instead of a queued stale one.
			 * (Selection-only changes bypass this entirely
			 * via redrawsel(), which is cheap enough to run
			 * per event.)
			 *
			 * With a solid earth loaded, motion frames
			 * render at half resolution (globecoarse; the
			 * per-pixel earth pass is the dominant frame
			 * cost and can outlast the tick interval at
			 * full window size, which is what made drag/
			 * spin stutter and input go erratic behind the
			 * backlogged frames).  When a tick finds no
			 * motion left, one full-quality settle frame
			 * re-renders the final view; discrete actions
			 * (zoom, keyboard, menu) never set coarse and
			 * so stay full quality.
			 */
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

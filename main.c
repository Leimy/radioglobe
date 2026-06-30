#include <u.h>
#include <libc.h>
#include <draw.h>
#include <event.h>
#include <keyboard.h>
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

double clat = 30.0;
double clon = 0.0;
double zoom = 1.0;
double zoommin = 0.5;
//double zoommax = 8.0;
double zoommax = 128.0;

/*
 * Momentum/inertial spin: vlat/vlon are the current angular
 * velocity in degrees per millisecond, tracked continuously while
 * dragging (see Emouse handling) and consumed on each Etick once
 * the button is released.  friction decays the velocity each tick;
 * vmin is the speed below which we consider the spin stopped (and
 * stop ticking redraw).
 */
double vlat = 0.0;
double vlon = 0.0;
double lastclat, lastclon;
ulong lastmsec;
double friction = 0.92;
double vmin = 0.0005;

Station *stations;
int nstation;
int selstation = -1;
int playing = -1;
int streampid = -1;
Image *statbg;

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

/*
 * quote a string for safe inclusion inside rc single quotes.
 * rc quoting: wrap in '...' and double any embedded '.
 * writes into dst (size dlen); returns dst.
 */
static char *
rcquote(char *dst, int dlen, char *s)
{
	char *d, *e;

	d = dst;
	e = dst + dlen - 1;
	if(d < e) *d++ = '\'';
	for(; *s && d < e; s++){
		if(*s == '\''){
			if(d < e) *d++ = '\'';
			if(d < e) *d++ = '\'';
		} else
			*d++ = *s;
	}
	if(d < e) *d++ = '\'';
	*d = 0;
	return dst;
}

static void
startstream(int idx)
{
	int pid;
	char qurl[2048];
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
	 * RFFDG: child gets its own fd table so its redirects can't
	 *   clobber the GUI's fds.
	 * RFNOTEG: new note group so postnote(PNGROUP) kills the whole
	 *   pipeline (play, hget, decoder, pcmconv).
	 * RFNOWAIT: don't block the parent.
	 */
	rcquote(qurl, sizeof qurl, stations[idx].url);
	snprint(cmd, sizeof cmd,
		"play -o /fd/1 %s </dev/null | "
		"audio/pcmconv -o s16c2r%d >/dev/audio",
		qurl, devrate);

	pid = rfork(RFPROC|RFFDG|RFNOTEG|RFNOWAIT);
	if(pid == 0){
		int fd, nfd;

		/* close draw device fds and other clutter */
		for(fd = 3; fd < 30; fd++)
			close(fd);

		/* sane 0/1/2 for rc; errors to console */
		close(0);
		close(1);
		close(2);
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
	Rectangle gr;
	int i, vis, best, bestd, d;
	Point p;

	gr = globerect();
	best = -1;
	bestd = 15*15; /* max click distance squared, in pixels */

	for(i = 0; i < nstation; i++){
		geo2screen(gr, clat, clon, zoom, stations[i].geo, &p, &vis);
		if(!vis)
			continue;
		d = (p.x - xy.x)*(p.x - xy.x) + (p.y - xy.y)*(p.y - xy.y);
		if(d < bestd){
			bestd = d;
			best = i;
		}
	}
	return best;
}

static char *
skipws(char *s)
{
	while(*s == ' ' || *s == '\t')
		s++;
	return s;
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
		while(*s && *s != ' ' && *s != '\t') s++;
		if(*s) *s++ = 0;
		s = skipws(s);

		slon = s;
		while(*s && *s != ' ' && *s != '\t') s++;
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
					u = t;
					while(*u == ' ' || *u == '\t') u++;
					if(*u){
						v = u;
						while(*v && *v != ' ' && *v != '\t') v++;
						while(*v == ' ' || *v == '\t') v++;
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
		stations[nstation].name = strdup(name);
		stations[nstation].url = strdup(url);
		nstation++;
	}
	free(buf);
	return nstation;
}

static void
drawstatusbar(void)
{
	Rectangle r;
	int h;
	char buf[256];
	Point p;
	h = font->height + 8;
	r.min.x = screen->r.min.x;
	r.max.x = screen->r.max.x;
	r.max.y = screen->r.max.y;
	r.min.y = r.max.y - h;

	draw(screen, r, statbg ? statbg : display->black, nil, ZP);

	p = Pt(r.min.x + 6, r.min.y + 4);

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
		snprint(buf, sizeof buf, "  [%.1f, %.1f]  zoom %.1fx",
			clat, clon, zoom);

	string(screen, p, display->white, ZP, font, buf);
}

static void
redraw(void)
{
	Rectangle gr;

	gr = globerect();
	globedraw(screen, gr, clat, clon, zoom);
	drawstations(screen, gr, clat, clon, zoom, stations, nstation, selstation);
	drawstatusbar();
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
	fprint(2, "usage: %s [-s stationfile] [-c coastfile] [-r devrate]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	Event ev;
	Mouse m;
	int e, dragging, oldbuttons;
	Point dragstart;
	double dragclat, dragclon;
	int rad;
	char *stationfile, *cfile;

	stationfile = "/lib/radio/stations";
	cfile = nil;

	ARGBEGIN{
	case 's':
		stationfile = EARGF(usage());
		break;
	case 'c':
		cfile = EARGF(usage());
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
	globeinit();

	statbg = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x141414ff);

	if(loadstations(stationfile) < 0)
		fprint(2, "warning: could not load %s: %r\n", stationfile);

	dragging = 0;
	oldbuttons = 0;
	dragclat = clat;
	dragclon = clon;
	dragstart = ZP;

	redraw();

	for(;;){
		e = event(&ev);

		switch(e){
		case Emouse:
			m = ev.mouse;

			/* scroll wheel zoom */
			if(m.buttons & 8){
				zoom *= 1.15;
				if(zoom > zoommax) zoom = zoommax;
				redraw();
				break;
			}
			if(m.buttons & 16){
				zoom /= 1.15;
				if(zoom < zoommin) zoom = zoommin;
				redraw();
				break;
			}

			/* left button: drag to rotate */
			if(m.buttons & 1){
				if(!dragging && !(oldbuttons & 1)){
					dragging = 1;
					dragstart = m.xy;
					dragclat = clat;
					dragclon = clon;
					/* grabbing a spinning globe stops it dead */
					vlat = 0.0;
					vlon = 0.0;
					lastclat = clat;
					lastclon = clon;
					lastmsec = m.msec;
				}
				if(dragging){
					rad = ((Dx(screen->r) < Dy(screen->r)) ? Dx(screen->r) : Dy(screen->r)) / 2 - 4;
					rad = (int)(rad * zoom);
					if(rad < 1) rad = 1;

					clon = dragclon - (double)(m.xy.x - dragstart.x) * 180.0 / rad;
					clat = dragclat + (double)(m.xy.y - dragstart.y) * 180.0 / rad;
					if(clat > 90.0) clat = 90.0;
					if(clat < -90.0) clat = -90.0;
					while(clon > 180.0) clon -= 360.0;
					while(clon < -180.0) clon += 360.0;

					/*
					 * Track instantaneous velocity (deg/ms)
					 * so we have a launch speed if the
					 * button comes up on this frame.
					 */
					if(m.msec > lastmsec){
						double dt = m.msec - lastmsec;
						vlat = (clat - lastclat) / dt;
						vlon = (clon - lastclon) / dt;
						lastclat = clat;
						lastclon = clon;
						lastmsec = m.msec;
					}
					redraw();
				}
			} else {
				if(dragging){
					dragging = 0;
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
					loadstations(stationfile);
					selstation = -1;
					break;
				case Mexit:
					stopstream();
					exits(nil);
				}
				redraw();
			}

			/* hover: find nearest station */
			if(m.buttons == 0 && !dragging){
				int s = findstation(m.xy);
				if(s != selstation){
					selstation = s;
					redraw();
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
				clon -= 10.0 / zoom;
				while(clon < -180.0) clon += 360.0;
				redraw();
				break;
			case Kright:
				clon += 10.0 / zoom;
				while(clon > 180.0) clon -= 360.0;
				redraw();
				break;
			case Kup:
				clat += 10.0 / zoom;
				if(clat > 90.0) clat = 90.0;
				redraw();
				break;
			case Kdown:
				clat -= 10.0 / zoom;
				if(clat < -90.0) clat = -90.0;
				redraw();
				break;
			case '+':
			case '=':
				zoom *= 1.3;
				if(zoom > zoommax) zoom = zoommax;
				redraw();
				break;
			case '-':
				zoom /= 1.3;
				if(zoom < zoommin) zoom = zoommin;
				redraw();
				break;
			case '0':
				zoom = 1.0;
				clat = 30.0;
				clon = 0.0;
				redraw();
				break;
			}
			break;

		case Etick:
			if(dragging || (vlat == 0.0 && vlon == 0.0))
				break;
			clon += vlon * Tickms;
			clat += vlat * Tickms;
			if(clat > 90.0) clat = 90.0;
			if(clat < -90.0) clat = -90.0;
			while(clon > 180.0) clon -= 360.0;
			while(clon < -180.0) clon += 360.0;
			vlat *= friction;
			vlon *= friction;
			if(fabs(vlat) < vmin && fabs(vlon) < vmin)
				vlat = vlon = 0.0;
			redraw();
			break;
		}
	}
}

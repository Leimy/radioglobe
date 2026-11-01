#include <u.h>
#include <libc.h>
#include <draw.h>
#include <bio.h>
#include "view.h"
#include "dat.h"

/*
 * Orthographic globe projection.
 *
 * The viewer looks at the globe from infinity.
 * Center of view is at (clat, clon).
 * A point (lat, lon) projects to:
 *   x = cos(lat) * sin(lon - clon)
 *   y = cos(clat)*sin(lat) - sin(clat)*cos(lat)*cos(lon - clon)
 * Visible when:
 *   sin(clat)*sin(lat) + cos(clat)*cos(lat)*cos(lon - clon) > 0
 */

static Image *ocean, *coastcol, *dotcol, *dotsel, *gridcol, *textcol;

#define DTOR	(PI / 180.0)

/*
 * Cached static globe layer: keyed on (r, clat, clon, zoom, coarse).
 * Hover and status bar changes reuse the cache.
 */
static Image *cache;
static Rectangle cacherect;
static double cclat, cclon, czoom;
static int cachevalid;
static int ccoarse;	/* quality the cache was rendered at; see globecoarse */

/*
 * Solid-earth mode: per-pixel shaded, textured sphere.  The flat
 * fillellipse ocean disc is replaced by drawearth() when either
 * emask (land/ocean mask) or etex (RGB photo texture) is loaded.
 *
 * Per-pixel inverse orthographic projection: each disc pixel maps
 * back onto the sphere analytically, no mesh or z-buffer needed.
 * Lighting is viewer-attached in view space (basis is orthonormal).
 * emask/etex are mutually exclusive; selected by EARTHMASK/EARTHTEX
 * magic string.  emaskw/emaskh describe whichever is active.
 */
static uchar *emask;
static uchar *etex;
static int emaskw, emaskh;
static char *earthpath;
static int earthloaded;

enum {
	Nasin	= 2048,		/* p.z -> mask row LUT */
	Natan	= 1024,		/* atan ratio LUT */
};
/* rowlut: exact fractional rows ((0.5 - asin(z)/PI)*emaskh).
 * Both LUTs are interpolated: prevents stair-stepping bilinear
 * sampling at high zoom (1/Natan rad ≈ 250px at 512x).
 */
static double *rowlut;
static double *atanlut;
static double lightx, lighty, lightz;

static uchar *ebuf;		/* client-side RGB24 pixel buffer */
static Image *eimg;
static Rectangle erect;

static Image*
color(ulong rgba)
{
	Image *i;

	i = allocimage(display, Rect(0,0,1,1), screen->chan, 1, rgba);
	if(i == nil)
		sysfatal("allocimage: %r");
	return i;
}

void
globeinit(void)
{
	ocean = color(0x0a1a3aff);
	coastcol = color(0x3a6a4aff);
	gridcol = color(0x1a2a3aff);
	dotcol = color(0x44dd88ff);
	dotsel = color(0xff8844ff);
	textcol = color(DWhite);
}

void
earthfile(char *path)
{
	earthpath = path;
}

/* ecoarse: half-resolution earth during drag/spin for smoothness. */
static int ecoarse;

void
globecoarse(int c)
{
	ecoarse = earthloaded ? c : 0;
}

static int
loadmask(char *path)
{
	Biobuf *b;
	char *ln, *e, *f[2];
	int w, h, istex;
	long n;

	b = Bopen(path, OREAD);
	if(b == nil)
		return -1;
	ln = Brdline(b, '\n');
	if(ln == nil){
		Bterm(b);
		return -1;
	}
	if(Blinelen(b) >= 9 && strncmp(ln, "EARTHMASK", 9) == 0)
		istex = 0;
	else if(Blinelen(b) >= 8 && strncmp(ln, "EARTHTEX", 8) == 0)
		istex = 1;
	else{
		Bterm(b);
		return -1;
	}
	ln = Brdline(b, '\n');
	if(ln == nil){
		Bterm(b);
		return -1;
	}
	ln[Blinelen(b)-1] = 0;
	if(tokenize(ln, f, 2) != 2){
		Bterm(b);
		return -1;
	}
	/* strtol: catch non-numeric input; range check catches overflow */
	w = strtol(f[0], &e, 10);
	if(e == f[0] || w <= 0 || w < 4 || w > 16384){
		Bterm(b);
		return -1;
	}
	h = strtol(f[1], &e, 10);
	if(e == f[1] || h <= 0 || h < 2 || h > 8192){
		Bterm(b);
		return -1;
	}
	n = (long)w*h*(istex ? 3 : 1);
	if(istex){
		etex = malloc(n);
		if(etex == nil)
			sysfatal("malloc: %r");
		if(Bread(b, etex, n) != n){
			free(etex);
			etex = nil;
			Bterm(b);
			return -1;
		}
	}else{
		emask = malloc(n);
		if(emask == nil)
			sysfatal("malloc: %r");
		if(Bread(b, emask, n) != n){
			free(emask);
			emask = nil;
			Bterm(b);
			return -1;
		}
	}
	emaskw = w;
	emaskh = h;
	Bterm(b);
	return 0;
}

/* Load earth mask/texture and precompute LUTs + light direction. */
void
earthinit(void)
{
	int i;
	double z, t, len;

	if(earthpath != nil){
		/* named explicitly: failing loudly beats a silent flat globe */
		if(loadmask(earthpath) != 0)
			sysfatal("cannot load earth mask %s", earthpath);
	}
	else if(loadmask("earth.mask") != 0 && loadmask("/lib/radio/earth.mask") != 0)
		return;
	earthloaded = 1;

	rowlut = malloc((Nasin+1)*sizeof(double));
	atanlut = malloc((Natan+1)*sizeof(double));
	if(rowlut == nil || atanlut == nil)
		sysfatal("malloc: %r");
	for(i = 0; i <= Nasin; i++){
		z = 2.0*i/Nasin - 1.0;
		if(z < -1.0)
			z = -1.0;
		if(z > 1.0)
			z = 1.0;
		t = (0.5 - asin(z)/PI) * emaskh;
		if(t < 0.0)
			t = 0.0;
		if(t > emaskh-1)
			t = emaskh-1;
		rowlut[i] = t;
	}
	for(i = 0; i <= Natan; i++)
		atanlut[i] = atan((double)i/Natan);

	/* viewer-attached lamp: from the upper left, mostly frontal */
	lightx = -0.35;
	lighty = 0.45;
	lightz = 0.82;
	len = sqrt(lightx*lightx + lighty*lighty + lightz*lightz);
	lightx /= len;
	lighty /= len;
	lightz /= len;
}

/* atan2 via LUT: octant reduction, interpolated lookup on min/max ratio. */
static double
lutatan2(double y, double x)
{
	double ax, ay, a, t, f;
	int i;

	ax = fabs(x);
	ay = fabs(y);
	if(ax + ay < 1e-12)
		return 0.0;	/* looking dead at a pole */
	if(ax >= ay)
		t = ay/ax*Natan;
	else
		t = ax/ay*Natan;
	i = (int)t;
	if(i >= Natan)
		i = Natan-1;	/* ratio == 1.0 exactly: f becomes 1 */
	f = t - i;
	a = atanlut[i] + f*(atanlut[i+1] - atanlut[i]);
	if(ax < ay)
		a = PI/2.0 - a;
	if(x < 0)
		a = PI - a;
	if(y < 0)
		a = -a;
	return a;
}

/*
 * Shaded, textured globe: inverse orthographic projection per pixel.
 * Land/ocean color from mask, diffuse+ambient lighting from view-space lamp.
 * Pixels outside the disc are black, replacing both background and ocean disc.
 * Composed in client-side buffer, shipped with one loadimage() + draw().
 */
static void
drawearth(Image *dst, Rectangle r, double clat, double clon, double zoom)
{
	int x, y, w, h, cx, cy, rad, row, col, idx, stride;
	int row0, row1, col0, col1, x0, x1, xstep, ystep;
	double scale, px, py, pz, rr, wx, wy, wz, dl, inten;
	double idxf, rowf, colf, fracx, fracy;
	double bb, gg, rrr, w00, w01, w10, w11;
	double s2, halfw;
	uchar *bp, *rowp, *p00, *p01, *p10, *p11;
	Vec3 ex, ey, ez;

	w = Dx(r);
	h = Dy(r);
	if(eimg == nil || !eqrect(erect, r)){
		if(eimg != nil)
			freeimage(eimg);
		free(ebuf);
		eimg = allocimage(display, r, RGB24, 0, DNofill);
		if(eimg == nil)
			sysfatal("allocimage: %r");
		ebuf = malloc((long)w*3*h);
		if(ebuf == nil)
			sysfatal("malloc: %r");
		erect = r;
	}

	globegeom(r, zoom, &cx, &cy, &rad);
	viewbasis(clon, clat, &ex, &ey, &ez);
	scale = 1.0 / rad;
	stride = w*3;

	/*
	 * Interactive (coarse) mode: sample every other pixel in x
	 * and y and replicate into 2x2 blocks; main.c renders one
	 * full-resolution frame when the motion stops (see
	 * globecoarse above).
	 */
	xstep = ystep = ecoarse ? 2 : 1;

	for(y = r.min.y; y < r.max.y; y += ystep){
		rowp = ebuf + (long)(y - r.min.y)*stride;
		py = (cy - y) * scale;

		/* disc row span: memset black margins, walk only on-sphere pixels */
		s2 = 1.0 - py*py;
		if(s2 > 0.0){
			halfw = sqrt(s2) * rad;
			x0 = cx - (int)halfw;
			x1 = cx + (int)halfw;
			if(x0 < r.min.x)
				x0 = r.min.x;
			if(x1 > r.max.x-1)
				x1 = r.max.x-1;
		}else{
			x0 = 0;
			x1 = -1;	/* row entirely off the sphere */
		}
		if(x0 > x1){
			memset(rowp, 0, stride);
			if(ystep == 2 && y+1 < r.max.y)
				memset(rowp+stride, 0, stride);
			continue;
		}
		memset(rowp, 0, 3*(x0 - r.min.x));
		memset(rowp + 3*((x1+1) - r.min.x), 0, 3*(r.max.x - (x1+1)));

		bp = rowp + 3*(x0 - r.min.x);
		for(x = x0; x <= x1; x += xstep, bp += 3*xstep){
			px = (x - cx) * scale;
			rr = px*px + py*py;
			if(rr > 1.0){
				bp[0] = 0;	/* span edge: still off-sphere */
				bp[1] = 0;
				bp[2] = 0;
				if(xstep == 2 && x+1 <= x1){
					bp[3] = 0;
					bp[4] = 0;
					bp[5] = 0;
				}
				continue;
			}
			pz = sqrt(1.0 - rr);

			/* back onto the sphere: world = px*ex + py*ey + pz*ez */
			wx = px*ex.x + py*ey.x + pz*ez.x;
			wy = px*ex.y + py*ey.y + pz*ez.y;
			wz = px*ex.z + py*ey.z + pz*ez.z;

			/* idxf/colf: fractional for bilinear; row/col: truncated for nearest-neighbor */
			idxf = (wz + 1.0) * 0.5 * Nasin;
			if(idxf < 0)
				idxf = 0;
			if(idxf > Nasin)
				idxf = Nasin;
			idx = (int)idxf;
			if(idx >= Nasin)
				idx = Nasin-1;
			row = (int)rowlut[idx];

			colf = (lutatan2(wy, wx) + PI) * (1.0/(2.0*PI)) * emaskw;
			if(colf >= emaskw)
				colf -= emaskw;
			if(colf < 0)
				colf += emaskw;
			col = (int)colf;
			if(col >= emaskw)
				col = emaskw-1;

			/* lamp is in view space: no world transform needed */
			dl = px*lightx + py*lighty + pz*lightz;
			if(dl < 0)
				dl = 0;
			inten = 0.35 + 0.70*dl;
			if(inten > 1.0)
				inten = 1.0;

			if(etex != nil){
				/* bilinear texture sample */
				fracy = idxf - idx;
				rowf = rowlut[idx] + fracy*(rowlut[idx+1] - rowlut[idx]);
				row0 = (int)rowf;
				if(row0 >= emaskh-1)
					row0 = emaskh-2 >= 0 ? emaskh-2 : 0;
				row1 = row0+1;
				fracy = rowf - row0;

				col0 = (int)colf;
				fracx = colf - col0;
				col1 = col0+1;
				if(col1 >= emaskw)
					col1 = 0;

				p00 = etex + ((long)row0*emaskw + col0)*3;
				p01 = etex + ((long)row0*emaskw + col1)*3;
				p10 = etex + ((long)row1*emaskw + col0)*3;
				p11 = etex + ((long)row1*emaskw + col1)*3;
				w00 = (1-fracx)*(1-fracy);
				w01 = fracx*(1-fracy);
				w10 = (1-fracx)*fracy;
				w11 = fracx*fracy;
				bb  = p00[0]*w00 + p01[0]*w01 + p10[0]*w10 + p11[0]*w11;
				gg  = p00[1]*w00 + p01[1]*w01 + p10[1]*w10 + p11[1]*w11;
				rrr = p00[2]*w00 + p01[2]*w01 + p10[2]*w10 + p11[2]*w11;
				bp[0] = (uchar)(bb * inten);
				bp[1] = (uchar)(gg * inten);
				bp[2] = (uchar)(rrr * inten);
			}else if(emask[(long)row*emaskw + col]){
				bp[0] = (uchar)(0x5a * inten);	/* land: B */
				bp[1] = (uchar)(0x8a * inten);	/* G */
				bp[2] = (uchar)(0x4a * inten);	/* R */
			}else{
				bp[0] = (uchar)(0x50 * inten);	/* ocean: B */
				bp[1] = (uchar)(0x28 * inten);	/* G */
				bp[2] = (uchar)(0x10 * inten);	/* R */
			}
			if(xstep == 2 && x+1 <= x1){
				bp[3] = bp[0];	/* coarse: replicate right */
				bp[4] = bp[1];
				bp[5] = bp[2];
			}
		}
		if(ystep == 2 && y+1 < r.max.y)
			memcpy(rowp+stride, rowp, stride);	/* coarse: replicate the row below */
	}

	loadimage(eimg, eimg->r, ebuf, (long)stride*h);
	draw(dst, r, eimg, nil, r.min);
}

/*
 * Fast zoom preview: affine-scale cached layer when only zoom changes.
 * Returns 1 if drawn, 0 if caller must do a real globedraw().
 * maxwarp bounds extrapolation distance to prevent smear from repeated
 * resampling; destination is cleared to black first (affinewarp is SoverD).
 */
static double maxwarp = 2.0;	/* max preview scale factor away from the cache */

int
globezoomwarp(Image *dst, Rectangle r, double clat, double clon, double zoom)
{
	int cx, cy;
	double f, m[3][3];
	Warp w;

	if(!cachevalid || !eqrect(cacherect, r) || clat != cclat || clon != cclon)
		return 0;
	if(czoom <= 0)
		return 0;

	f = zoom / czoom;
	if(f > maxwarp || f < 1.0/maxwarp)
		return 0;

	if(f == 1.0){
		draw(dst, r, cache, nil, r.min);
		return 1;
	}

	cx = (r.min.x + r.max.x) / 2;
	cy = (r.min.y + r.max.y) / 2;

	memset(m, 0, sizeof m);
	m[0][0] = f;
	m[1][1] = f;
	m[2][2] = 1;
	w = mkwarp(m);

	draw(dst, r, display->black, nil, ZP);
	genaffinewarp(dst, Pt(cx, cy), r, cache, Pt(cx, cy), nil, ZP, &w, 1);
	return 1;
}

/*
 * Screen geometry for a globe drawn in rectangle r at a given zoom:
 * disc center (cx, cy) and radius rad in pixels.  The single
 * definition used by projection, drawing, hit testing and the drag
 * handler; they must never disagree about this.
 */
void
globegeom(Rectangle r, double zoom, int *cx, int *cy, int *rad)
{
	int rr;

	*cx = (r.min.x + r.max.x) / 2;
	*cy = (r.min.y + r.max.y) / 2;
	rr = ((Dx(r) < Dy(r)) ? Dx(r) : Dy(r)) / 2 - 4;
	rr = (int)(rr * zoom);
	if(rr < 1)
		rr = 1;
	*rad = rr;
}

/* Convert a Geo to a unit vector on the sphere. */
void
geo2vec(Geo g, Vec3 *v)
{
	double lat, lon, cl;

	lat = g.lat * DTOR;
	lon = g.lon * DTOR;
	cl = cos(lat);
	v->x = cl * cos(lon);
	v->y = cl * sin(lon);
	v->z = sin(lat);
}

/*
 * Project p against the view basis.  *cosc is the dot product
 * with ez: positive means facing the viewer (visible), and its
 * magnitude says how far from the limb (0 = exactly on the
 * horizon), which drawstations uses to taper dot size.
 */
static void
vecproject(Vec3 *ex, Vec3 *ey, Vec3 *ez, Vec3 p, double *px, double *py, double *cosc)
{
	*cosc = p.x*ez->x + p.y*ez->y + p.z*ez->z;
	*px = p.x*ex->x + p.y*ex->y + p.z*ex->z;
	*py = p.x*ey->x + p.y*ey->y + p.z*ey->z;
}

static void
project(double clat, double clon, Geo g, double *px, double *py, int *vis)
{
	double lat, lon, dlon;
	double slat, clat0, slat0, clat1, slon, clon1;
	double cosc;

	lat = g.lat * DTOR;
	lon = g.lon * DTOR;
	clat0 = clat * DTOR;
	dlon = lon - clon * DTOR;

	slat = sin(lat);
	clat1 = cos(lat);
	slat0 = sin(clat0);
	clat0 = cos(clat0);
	slon = sin(dlon);
	clon1 = cos(dlon);

	cosc = slat0*slat + clat0*clat1*clon1;
	*vis = (cosc > 0.0);

	*px = clat1 * slon;
	*py = clat0*slat - slat0*clat1*clon1;
}

void
geo2screen(Rectangle r, double clat, double clon, double zoom, Geo g, Point *p, int *visible)
{
	double x, y;
	int cx, cy, rad;

	globegeom(r, zoom, &cx, &cy, &rad);
	project(clat, clon, g, &x, &y, visible);
	p->x = cx + (int)(x * rad);
	p->y = cy - (int)(y * rad);
}

static void
drawgrid(Image *dst, Rectangle r, double clat, double clon, double zoom)
{
	Geo g;
	Point p, prev;
	int vis, prevvis;
	double lat, lon;

	/* latitude lines every 30 degrees */
	for(lat = -60; lat <= 60; lat += 30){
		prevvis = 0;
		g.lat = lat;
		for(lon = -180; lon <= 180; lon += 2){
			g.lon = lon;
			geo2screen(r, clat, clon, zoom, g, &p, &vis);
			if(vis && prevvis)
				line(dst, prev, p, Endsquare, Endsquare, 0, gridcol, ZP);
			prev = p;
			prevvis = vis;
		}
	}

	/* longitude lines every 30 degrees */
	for(lon = -180; lon < 180; lon += 30){
		prevvis = 0;
		g.lon = lon;
		for(lat = -90; lat <= 90; lat += 2){
			g.lat = lat;
			geo2screen(r, clat, clon, zoom, g, &p, &vis);
			if(vis && prevvis)
				line(dst, prev, p, Endsquare, Endsquare, 0, gridcol, ZP);
			prev = p;
			prevvis = vis;
		}
	}
}

/*
 * Bounding-cap culling: can any point of coastline c be visible?
 * c->center/c->capcos define a cone containing all polyline points.
 * A point is visible when dot(p, ez) > 0.  The maximum dot product
 * over the cap is <= 0 when dot(center, ez) <= -sin(theta).
 * sin(theta) from capcos via sqrt(1-cos^2) — no inverse trig needed.
 * Conservative only in our favor: never wrongly culls a visible line.
 */
static int
capvisible(Coastline *c, Vec3 *ez)
{
	double dot, s2;

	if(c->capcos == -2)	/* culling disabled for this polyline */
		return 1;

	dot = c->center.x*ez->x + c->center.y*ez->y + c->center.z*ez->z;
	s2 = 1.0 - c->capcos*c->capcos;
	if(s2 < 0)
		s2 = 0;
	return dot > -sqrt(s2);
}

enum {
	/* poly() wire-protocol: 16-bit point count (max 65536).
	 * 8192 is the chunk limit; boundary points are repeated
	 * into the next chunk so connecting segments are preserved.
	 */
	Maxpolypts = 8192,
};

static void
drawcoasts(Image *dst, Rectangle r, double clat, double clon, double zoom)
{
	static Point runbuf[Maxpolypts];
	int i, j, cx, cy, rad, nrun, px, py, stride;
	double x, y, cosc;
	Vec3 ex, ey, ez;
	Coastline *c;

	globegeom(r, zoom, &cx, &cy, &rad);

	/* computed once per call, not once per point */
	viewbasis(clon, clat, &ex, &ey, &ez);

	for(i = 0; i < ncoast; i++){
		c = &coasts[i];

		/* whole chunk on the far side: skip without touching its points */
		if(!capvisible(c, &ez))
			continue;

		/* poly() batching + LOD stride. Zero-length segments dropped. */
		stride = 1;
		if(c->step > 0 && c->step * rad < 1.0)
			stride = (int)(1.0 / (c->step * rad));
		if(stride > 16)
			stride = 16;

		nrun = 0;
		for(j = 0; j < c->npts; j += stride){
			/* never skip final point: preserves chunk boundaries */
			if(j + stride >= c->npts && j < c->npts - 1)
				stride = c->npts - 1 - j;
			vecproject(&ex, &ey, &ez, c->vec[j], &x, &y, &cosc);
			if(cosc <= 0.0){
				if(nrun > 1)
					poly(dst, runbuf, nrun, Endsquare, Endsquare, 0, coastcol, ZP);
				nrun = 0;
				continue;
			}
			px = cx + (int)(x * rad);
			py = cy - (int)(y * rad);
			/* drop zero-length segments: each kept point costs a memline */
			if(nrun > 0 && runbuf[nrun-1].x == px && runbuf[nrun-1].y == py)
				continue;
			runbuf[nrun].x = px;
			runbuf[nrun].y = py;
			nrun++;
			if(nrun == Maxpolypts){
				poly(dst, runbuf, nrun, Endsquare, Endsquare, 0, coastcol, ZP);
				runbuf[0] = runbuf[nrun-1];
				nrun = 1;
			}
		}
		if(nrun > 1)
			poly(dst, runbuf, nrun, Endsquare, Endsquare, 0, coastcol, ZP);
	}
}

void
globedraw(Image *dst, Rectangle r, double clat, double clon, double zoom)
{
	int cx, cy, rad;

	/* re-render only when the view actually changed */
	if(!cachevalid || !eqrect(cacherect, r) ||
	   clat != cclat || clon != cclon || zoom != czoom ||
	   ecoarse != ccoarse){
		/* reallocate only on resize */
		if(cache == nil || !eqrect(cacherect, r)){
			if(cache != nil)
				freeimage(cache);
			cache = allocimage(display, r, screen->chan, 0, DNofill);
			if(cache == nil)
				sysfatal("allocimage: %r");
		}

		globegeom(r, zoom, &cx, &cy, &rad);

		if(earthloaded)
			drawearth(cache, r, clat, clon, zoom);
		else {
			draw(cache, r, display->black, nil, ZP);
			fillellipse(cache, Pt(cx, cy), rad, rad, ocean, ZP);
		}

		drawgrid(cache, r, clat, clon, zoom);
		drawcoasts(cache, r, clat, clon, zoom);
		ellipse(cache, Pt(cx, cy), rad, rad, 1, coastcol, ZP);

		cacherect = r;
		cclat = clat;
		cclon = clon;
		czoom = zoom;
		ccoarse = ecoarse;
		cachevalid = 1;
	}

	draw(dst, r, cache, nil, r.min);
}

/* Dot radius: grows with zoom, shrinks at deep zoom (24x+). */
static int
dotradius(double zoom)
{
	if(zoom > 24.0)
		return 3;
	if(zoom > 3.0)
		return 5;
	if(zoom > 1.5)
		return 4;
	return 3;
}

/* Dot radius tapered near limb: 1px at horizon, full size at cosc >= 0.08. */
static int
dotsize(int dotr, double cosc)
{
	double band;

	band = 0.08;
	if(cosc >= band)
		return dotr;
	return 1 + (int)((dotr-1) * cosc / band);
}

enum {
	Dothash		= 8192,	/* per-frame dot dedup table; power of two */
	Maxprobe	= 8,
};

static ulong dotpos[Dothash];
static uint dotstamp[Dothash];
static uint dotgen;

/* Per-frame dot dedup via generation-stamped hash table. */
static int
dotseen(int x, int y)
{
	ulong key;
	uint h;
	int i;

	key = (ulong)(x & 0xffff)<<16 | (y & 0xffff);
	h = (key * 2654435761U) & (Dothash-1);
	for(i = 0; i < Maxprobe; i++){
		if(dotstamp[h] != dotgen){
			dotstamp[h] = dotgen;
			dotpos[h] = key;
			return 0;
		}
		if(dotpos[h] == key)
			return 1;
		h = (h+1) & (Dothash-1);
	}
	return 0;
}

/*
 * Draw station dots. Projects precomputed vectors against the
 * per-frame view basis (dot products), not trig per station.
 * Far-side and off-window culling applied.
 */
void
drawstations(Image *dst, Rectangle r, double clat, double clon, double zoom,
	Station *s, int ns)
{
	int i, dotr, dr, cx, cy, rad;
	double x, y, cosc;
	Point p;
	Rectangle clipr;
	Vec3 ex, ey, ez;

	globegeom(r, zoom, &cx, &cy, &rad);
	viewbasis(clon, clat, &ex, &ey, &ez);
	dotr = dotradius(zoom);
	clipr = insetrect(r, -dotr);	/* centers outside this can't touch r */
	dotgen++;			/* new dedup frame; see dotseen */

	for(i = 0; i < ns; i++){
		vecproject(&ex, &ey, &ez, s[i].vec, &x, &y, &cosc);
		if(cosc <= 0.0)
			continue;
		p.x = cx + (int)(x * rad);
		p.y = cy - (int)(y * rad);
		/* zoomed in, most of the hemisphere projects off-window */
		if(!ptinrect(p, clipr))
			continue;
		/* cluster-lite: dedup by dotr-sized cell, not exact pixel */
		if(dotseen(p.x/dotr, p.y/dotr))
			continue;
		dr = dotsize(dotr, cosc);
		fillellipse(dst, p, dr, dr, dotcol, ZP);
	}
}

/*
 * Draw selection overlay: highlight dot + name label.
 * Returns the rectangle it covered (ZR if nothing drawn).
 * Kept separate so a selection change only writes the overlay
 * area (restore old from back, draw new), not the full window.
 */
Rectangle
drawsel(Image *dst, Rectangle r, double clat, double clon, double zoom,
	Station *s, int ns, int sel)
{
	int dotr, dr, cx, cy, rad;
	double x, y, cosc;
	Point p, tp;
	Rectangle or;
	Vec3 ex, ey, ez;

	if(sel < 0 || sel >= ns)
		return ZR;
	globegeom(r, zoom, &cx, &cy, &rad);
	viewbasis(clon, clat, &ex, &ey, &ez);
	vecproject(&ex, &ey, &ez, s[sel].vec, &x, &y, &cosc);
	if(cosc <= 0.0)
		return ZR;
	dotr = dotradius(zoom);
	p.x = cx + (int)(x * rad);
	p.y = cy - (int)(y * rad);
	if(!ptinrect(p, insetrect(r, -dotr)))
		return ZR;

	dr = dotsize(dotr, cosc);
	fillellipse(dst, p, dr, dr, dotsel, ZP);
	tp.x = p.x + dr + 4;
	tp.y = p.y - font->height/2;
	string(dst, tp, textcol, ZP, font, s[sel].name);

	or = Rect(p.x-dr-1, p.y-dr-1, p.x+dr+2, p.y+dr+2);
	combinerect(&or, Rect(tp.x, tp.y,
		tp.x + stringwidth(font, s[sel].name), tp.y + font->height));
	if(!rectclip(&or, dst->r))
		return ZR;
	return or;
}

/*
 * Find nearest station to screen point xy, or -1.
 * Uses precomputed-vector projection (dot products), not trig per station.
 * Hit radius tracks dot size.
 */
int
stationhit(Rectangle r, double clat, double clon, double zoom, Point xy,
	Station *s, int ns, int cursel)
{
	int i, cx, cy, rad, px, py, best, bestd, maxd, dotr, dx, dy, dr, d;
	double x, y, cosc;
	Vec3 ex, ey, ez;

	globegeom(r, zoom, &cx, &cy, &rad);
	viewbasis(clon, clat, &ex, &ey, &ez);

	dotr = dotradius(zoom);
	maxd = 12 + dotr;	/* max click distance, pixels */

	/* sticky selection: keep current station while cursor is on its dot (±2px) */
	if(cursel >= 0 && cursel < ns){
		vecproject(&ex, &ey, &ez, s[cursel].vec, &x, &y, &cosc);
		if(cosc > 0.0){
			px = cx + (int)(x * rad);
			py = cy - (int)(y * rad);
			dx = px - xy.x;
			dy = py - xy.y;
			dr = dotsize(dotr, cosc) + 2;
			if(dx >= -dr && dx <= dr && dy >= -dr && dy <= dr &&
			   dx*dx + dy*dy <= dr*dr)
				return cursel;
		}
	}

	best = -1;
	bestd = maxd*maxd;

	for(i = 0; i < ns; i++){
		vecproject(&ex, &ey, &ez, s[i].vec, &x, &y, &cosc);
		if(cosc <= 0.0)
			continue;
		px = cx + (int)(x * rad);
		py = cy - (int)(y * rad);
		dx = px - xy.x;
		dy = py - xy.y;
		/* box-reject before squaring: prevents int overflow at deep zoom */
		if(dx > maxd || dx < -maxd || dy > maxd || dy < -maxd)
			continue;
		d = dx*dx + dy*dy;
		if(d < bestd){
			bestd = d;
			best = i;
		}
	}
	return best;
}

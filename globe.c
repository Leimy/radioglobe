#include <u.h>
#include <libc.h>
#include <draw.h>
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
static double dtor;

/*
 * Cached static globe layer (ocean + grid + coastlines + border).
 * This is the expensive part to render and it only depends on
 * (r, clat, clon, zoom).  Lots of redraws -- e.g. just updating the
 * hover/selection label, or the status bar -- don't change any of
 * those, so we reuse the cached image instead of re-walking the
 * whole coastline dataset every time.
 */
static Image *cache;
static Rectangle cacherect;
static double cclat, cclon, czoom;
static int cachevalid;

void
globeinit(void)
{
	dtor = PI / 180.0;
	ocean = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x0a1a3aff);
	coastcol = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x3a6a4aff);
	gridcol = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x1a2a3aff);
	dotcol = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0x44dd88ff);
	dotsel = allocimage(display, Rect(0,0,1,1), screen->chan, 1, 0xff8844ff);
	textcol = allocimage(display, Rect(0,0,1,1), screen->chan, 1, DWhite);
}

/*
 * Screen geometry for a globe drawn in rectangle r at a given zoom:
 * disc center (cx, cy) and radius rad in pixels.  This used to be
 * open-coded in half a dozen places (projection, drawing, hit
 * testing, and the drag handler in main.c), which is exactly how
 * the drag handler ended up disagreeing with the renderer about
 * which rectangle to measure.  One definition, used everywhere.
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

/*
 * Convert a Geo to a unit vector on the sphere.  Uses PI directly
 * (not the file-scope dtor) because this is called from coast.c
 * during coastinit(), which runs before globeinit() sets dtor.
 */
void
geo2vec(Geo g, Vec3 *v)
{
	double lat, lon, cl;

	lat = g.lat * PI / 180.0;
	lon = g.lon * PI / 180.0;
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

	lat = g.lat * dtor;
	lon = g.lon * dtor;
	clat0 = clat * dtor;
	dlon = lon - clon * dtor;

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
 * Can any point of coastline c possibly be visible, given that "ez"
 * is the unit vector pointing at the viewer (see viewbasis above)?
 *
 * c->center/c->capcos describe a bounding cap: a cone of half-angle
 * theta (cos(theta) == c->capcos) around c->center that contains
 * every point of the polyline.  A point p is visible exactly when
 * dot(p, ez) > 0 (it faces the viewer).  Over the whole cap, the
 * largest possible value of dot(p, ez) is:
 *
 *   - 1, if ez itself lies inside the cap (the cap straddles the
 *     point nearest the viewer), or
 *   - cos(angle(center,ez) - theta), otherwise -- i.e. the dot
 *     product at the cap's edge point closest to ez.
 *
 * That maximum is <= 0 exactly when angle(center,ez) >= theta + 90
 * degrees, which (taking cosines of both sides, and noting
 * cos(theta+90) == -sin(theta)) is the same as:
 *
 *   dot(center, ez) <= -sin(theta)
 *
 * sin(theta) is recovered from capcos == cos(theta) without ever
 * calling an inverse trig function, via sin(theta) = sqrt(1-cos^2).
 * Since capcos is an exact dot product of two unit vectors it should
 * already lie in [-1,1], but clamp the radicand anyway in case of
 * tiny floating-point overshoot.
 *
 * This bound can only be conservative in our favor: it may fail to
 * cull a polyline that turns out to have no visible points once you
 * look point-by-point (e.g. it bulges toward the viewer in the
 * middle but no actual data point lands in the visible gap), but it
 * will never wrongly cull one that does have a visible point.  Worst
 * case we do the per-point work we would have done anyway.
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
	/*
	 * poly()'s wire-protocol point count is a 16-bit field --
	 * see /sys/src/libdraw/poly.c: BPSHORT(a+5, np-1) -- so a
	 * single poly() call cannot encode more than 65536 points.
	 * Chunk well under that limit so unusually long coastline
	 * runs (plausible: NOTES.md flags that some Natural Earth
	 * polylines may span whole continents rather than per-island
	 * segments) still get batched into a handful of calls instead
	 * of silently overflowing the 16-bit field. The point at the
	 * chunk boundary is repeated as the first point of the next
	 * chunk so the connecting line segment is still drawn (poly's
	 * lines join adjacent points in its array).
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

		/*
		 * Quick reject: at any given moment roughly half of
		 * the planet's coastline is on the far side of the
		 * globe from the viewer.  Skip the whole polyline --
		 * without touching a single one of its points -- if
		 * it cannot possibly have anything visible.  This is
		 * the difference between "process every coastline
		 * point on every redraw" and "process only the ones
		 * that could matter," which is what actually saves
		 * work as coast.dat grows large.
		 */
		if(!capvisible(c, &ez))
			continue;

		/*
		 * Accumulate runs of consecutive visible points and
		 * draw each run with a single poly() call instead of
		 * one line() protocol message per segment. This draws
		 * exactly the same segments the old per-segment code
		 * did (a segment only when both its endpoints are
		 * visible) -- just batched. A polyline with thousands
		 * of on-screen points used to cost thousands of
		 * separate draw-device round trips every redraw (even
		 * after the vector-precompute optimization, which only
		 * removed the per-point trig, not the per-point draw
		 * call); now it costs one call per visible run, or a
		 * handful if a run is longer than Maxpolypts.
		 */
		/*
		 * LOD stride: c->step * rad is the average on-screen
		 * spacing of consecutive points in pixels.  When it
		 * falls below a pixel, walking every point buys no
		 * visible detail -- it only costs projection math and
		 * raster work -- so step over points to bring the
		 * drawn spacing back up to about a pixel.  Zoom in
		 * and the stride falls back to 1 (full detail).
		 */
		stride = 1;
		if(c->step > 0 && c->step * rad < 1.0)
			stride = (int)(1.0 / (c->step * rad));
		if(stride > 16)
			stride = 16;

		nrun = 0;
		for(j = 0; j < c->npts; j += stride){
			/*
			 * Never skip the final point: adjacent chunks
			 * share it, and dropping it opens gaps at the
			 * chunk boundaries.  Shortening the stride for
			 * the last hop is fine; only this polyline's
			 * final iteration uses it.
			 */
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
			/*
			 * Skip consecutive points that project to the
			 * same pixel.  Zoomed out, most of a detailed
			 * dataset collapses onto repeated pixels, and
			 * every point kept here costs a memline in the
			 * draw device.  The drawn segments are
			 * identical minus the zero-length ones.
			 */
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

/* fill visible land with a simple scanline approach would be expensive.
 * instead, draw filled coastline polygons. we draw all visible coast
 * segments as filled polygons where we have closed regions. for simplicity,
 * just draw the outlines -- filled continents require complex polygon
 * clipping against the globe horizon. the outlines look fine. */

void
globedraw(Image *dst, Rectangle r, double clat, double clon, double zoom)
{
	int cx, cy, rad;

	/*
	 * Re-render the static layer only when the view actually
	 * changed.  Plenty of redraws (hover/selection changes,
	 * status bar updates) leave clat/clon/zoom/r untouched, and
	 * with a large coast.dat re-walking every point on every one
	 * of those is the expensive part we don't need to repeat.
	 */
	if(!cachevalid || !eqrect(cacherect, r) ||
	   clat != cclat || clon != cclon || zoom != czoom){
		/*
		 * Only (re)allocate the offscreen cache image when its
		 * rectangle actually changes (i.e. a window resize).
		 * Previously this freed and reallocated the image on
		 * *every* call that reaches this branch, which includes
		 * every mouse-motion event during a drag -- pure churn,
		 * a round trip to the draw device to destroy and rebuild
		 * an image of the very same size, every single frame.
		 * Now we just draw fresh content into the same image.
		 */
		if(cache == nil || !eqrect(cacherect, r)){
			if(cache != nil)
				freeimage(cache);
			cache = allocimage(display, r, screen->chan, 0, DNofill);
			if(cache == nil)
				sysfatal("allocimage: %r");
		}

		globegeom(r, zoom, &cx, &cy, &rad);

		/* background */
		draw(cache, r, display->black, nil, ZP);

		/* globe disc */
		fillellipse(cache, Pt(cx, cy), rad, rad, ocean, ZP);

		/* grid */
		drawgrid(cache, r, clat, clon, zoom);

		/* coastlines */
		drawcoasts(cache, r, clat, clon, zoom);

		/* globe border */
		ellipse(cache, Pt(cx, cy), rad, rad, 1, coastcol, ZP);

		cacherect = r;
		cclat = clat;
		cclon = clon;
		czoom = zoom;
		cachevalid = 1;
	}

	draw(dst, r, cache, nil, r.min);
}

/*
 * Station dot radius in pixels: grows a little with zoom, then
 * shrinks again at deep zoom.  The per-frame dedup merges any
 * stations packed closer than a dot radius into one drawn dot,
 * so big dots keep dense clusters (e.g. European cities) fused
 * into a single blob well past the zoom level where smaller
 * dots would already resolve them into individually hoverable
 * targets.  At 24x and beyond, screen separation is what's
 * scarce, not dot visibility, so trade size for resolution.
 */
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

/*
 * Dot radius tapered near the limb: full size away from the
 * horizon, shrinking to 1px right at it.  Without the taper a
 * dot pops in and out at full size as rotation carries it
 * across the visibility threshold; growing it through a narrow
 * band (cosc in [0, band]) turns the pop into a swell.
 */
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

/*
 * Per-frame dot dedup: true if a dot was already drawn at (x,y)
 * this frame, recording it if not.  Stations stack -- many
 * entries share one coordinate, and zoomed out whole clusters
 * collapse onto single pixels -- and every duplicate costs a
 * fillellipse message to the draw device to repaint pixels that
 * are already painted.  Generation stamps make per-frame
 * clearing free; the short probe cap means a crowded table just
 * lets the odd duplicate through (a few wasted pixels) rather
 * than scanning.  drawstations bumps dotgen once per frame.
 */
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
 * Draw the station dots.  Like drawcoasts(), this projects each
 * station's precomputed unit vector (Station.vec, set once at load
 * time in main.c:loadstations() via geo2vec()) against a per-frame
 * view basis (viewbasis()/vecproject()) instead of calling
 * geo2screen()/project() -- i.e. sin/cos/asin/atan2 -- on every
 * station on every redraw.  With a large station file (stations2)
 * this matters exactly the same way it did for coast2.dat: the cost
 * was proportional to "every station, every frame" regardless of
 * how many are actually visible or selected.
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
		/*
		 * Cluster-lite: dedup by dotr-sized cell rather than
		 * exact pixel, so stations packed closer together
		 * than a dot radius draw as one dot instead of a
		 * smeared stack.  Such dots already overlapped almost
		 * entirely; this also cuts the message count in the
		 * dense areas where it matters.
		 */
		if(dotseen(p.x/dotr, p.y/dotr))
			continue;
		dr = dotsize(dotr, cosc);
		fillellipse(dst, p, dr, dr, dotcol, ZP);
	}
}

/*
 * Draw the selection overlay -- the selected station's dot in
 * the highlight color plus its name label -- and return the
 * rectangle it covered (ZR if nothing was drawn).  All stations,
 * the selected one included, are drawn plain by drawstations;
 * the overlay is painted over the top.
 *
 * Kept separate from drawstations so main.c can update just
 * this overlay when only the selection changes: hovering across
 * stations then costs two small screen writes (restore the old
 * overlay's pixels from the composed base, draw the new one)
 * instead of a full-window recompose, which repainted every dot
 * on screen -- and visibly disturbed all of them on displays
 * with unsynchronized presentation -- to change one.
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
 * Find the station nearest to screen point xy, or -1 if none is
 * close enough.  Used by main.c for both click hit-testing and the
 * hover label, i.e. it runs on essentially every mouse-motion
 * event, so it uses the same precomputed-vector projection as
 * drawstations() instead of the trig-per-station geo2screen() path.
 * The hit radius tracks the drawn dot size so targets don't get
 * relatively harder to click as dots grow with zoom.
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

	/*
	 * Sticky selection: while the cursor is still on the
	 * current selection's drawn dot (plus a couple px of
	 * slack), keep it, even if a neighbor is now nearer.  In
	 * a dense station file the nearest station changes on
	 * nearly every pixel of mouse travel, so without
	 * stickiness the hover label hops between packed
	 * neighbors; with it, the selection only moves once the
	 * cursor actually leaves the selected dot.  It also means
	 * a click always hits the station the label names.
	 *
	 * The sticky zone used to be the full grab radius (maxd,
	 * ~17px), which made dense clusters nearly unselectable:
	 * every neighbor sat inside the current selection's sticky
	 * zone, so hover could never move to them without leaving
	 * the whole neighborhood.  The dot itself is the right
	 * boundary -- slide off it and nearest-wins resumes.
	 */
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
		/*
		 * Box-reject before squaring.  Zoomed in, rad is
		 * tens of thousands of pixels and a visible-but-
		 * far-off-window station projects up to ~2*rad from
		 * the cursor; squaring that overflows a 32-bit int,
		 * and the wrapped "distance" could beat the station
		 * actually under the cursor.  Everything inside the
		 * box squares safely.
		 */
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

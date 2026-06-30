#include <u.h>
#include <libc.h>
#include <draw.h>
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
 * Compute the screen-space basis vectors for the current view
 * direction (clat, clon).  ex/ey are the screen-right/screen-up
 * unit vectors; ez points at the viewer.  Projecting a point is
 * then just three dot products -- no trig needed per point.
 */
static void
viewbasis(double clat, double clon, Vec3 *ex, Vec3 *ey, Vec3 *ez)
{
	double cla, sla, clo, slo;

	cla = cos(clat * dtor);
	sla = sin(clat * dtor);
	clo = cos(clon * dtor);
	slo = sin(clon * dtor);

	ex->x = -slo;       ex->y = clo;        ex->z = 0;
	ey->x = -sla*clo;   ey->y = -sla*slo;   ey->z = cla;
	ez->x = cla*clo;    ez->y = cla*slo;    ez->z = sla;
}

static void
vecproject(Vec3 *ex, Vec3 *ey, Vec3 *ez, Vec3 p, double *px, double *py, int *vis)
{
	double cosc;

	cosc = p.x*ez->x + p.y*ez->y + p.z*ez->z;
	*vis = cosc > 0.0;
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

	cx = (r.min.x + r.max.x) / 2;
	cy = (r.min.y + r.max.y) / 2;
	rad = ((Dx(r) < Dy(r)) ? Dx(r) : Dy(r)) / 2 - 4;
	rad = (int)(rad * zoom);

	project(clat, clon, g, &x, &y, visible);
	p->x = cx + (int)(x * rad);
	p->y = cy - (int)(y * rad);
}

int
globehit(Rectangle r, double clat, double clon, double zoom, Point xy, Geo *g)
{
	int cx, cy, rad;
	double x, y, rho;
	double sinc, cosc;
	double clat0, slat0;
	double lat, lon;

	cx = (r.min.x + r.max.x) / 2;
	cy = (r.min.y + r.max.y) / 2;
	rad = ((Dx(r) < Dy(r)) ? Dx(r) : Dy(r)) / 2 - 4;
	rad = (int)(rad * zoom);

	x = (double)(xy.x - cx) / rad;
	y = (double)(cy - xy.y) / rad;
	rho = sqrt(x*x + y*y);
	if(rho > 1.0)
		return 0;

	clat0 = clat * dtor;
	slat0 = sin(clat0);
	clat0 = cos(clat0);

	sinc = sin(asin(rho));  /* == rho, but clearer */
	cosc = cos(asin(rho));

	lat = asin(cosc * slat0 + y * sinc * clat0 / rho);
	lon = clon * dtor + atan2(x * sinc, rho * clat0 * cosc - y * slat0 * sinc);

	g->lat = lat / dtor;
	g->lon = lon / dtor;
	/* normalize longitude */
	while(g->lon > 180.0) g->lon -= 360.0;
	while(g->lon < -180.0) g->lon += 360.0;

	return 1;
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

static void
drawcoasts(Image *dst, Rectangle r, double clat, double clon, double zoom)
{
	int i, j, vis, prevvis, cx, cy, rad;
	double x, y;
	Point p, prev;
	Vec3 ex, ey, ez;
	Coastline *c;

	cx = (r.min.x + r.max.x) / 2;
	cy = (r.min.y + r.max.y) / 2;
	rad = ((Dx(r) < Dy(r)) ? Dx(r) : Dy(r)) / 2 - 4;
	rad = (int)(rad * zoom);

	/* computed once per call, not once per point */
	viewbasis(clat, clon, &ex, &ey, &ez);

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

		prevvis = 0;
		for(j = 0; j < c->npts; j++){
			vecproject(&ex, &ey, &ez, c->vec[j], &x, &y, &vis);
			p.x = cx + (int)(x * rad);
			p.y = cy - (int)(y * rad);
			if(vis && prevvis)
				line(dst, prev, p, Endsquare, Endsquare, 0, coastcol, ZP);
			prev = p;
			prevvis = vis;
		}
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

		cx = (r.min.x + r.max.x) / 2;
		cy = (r.min.y + r.max.y) / 2;
		rad = ((Dx(r) < Dy(r)) ? Dx(r) : Dy(r)) / 2 - 4;
		rad = (int)(rad * zoom);

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
	Station *s, int ns, int sel)
{
	int i, vis, dotr, cx, cy, rad;
	double x, y;
	Point p;
	Image *col;
	Vec3 ex, ey, ez;

	cx = (r.min.x + r.max.x) / 2;
	cy = (r.min.y + r.max.y) / 2;
	rad = ((Dx(r) < Dy(r)) ? Dx(r) : Dy(r)) / 2 - 4;
	rad = (int)(rad * zoom);

	viewbasis(clat, clon, &ex, &ey, &ez);

	dotr = 3;
	if(zoom > 1.5)
		dotr = 4;
	if(zoom > 3.0)
		dotr = 5;

	for(i = 0; i < ns; i++){
		vecproject(&ex, &ey, &ez, s[i].vec, &x, &y, &vis);
		if(!vis)
			continue;
		p.x = cx + (int)(x * rad);
		p.y = cy - (int)(y * rad);
		col = (i == sel) ? dotsel : dotcol;
		fillellipse(dst, p, dotr, dotr, col, ZP);

		/* draw label for selected station */
		if(i == sel){
			Point tp;
			tp.x = p.x + dotr + 4;
			tp.y = p.y - font->height/2;
			string(dst, tp, textcol, ZP, font, s[i].name);
		}
	}
}

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

static void
drawcoasts(Image *dst, Rectangle r, double clat, double clon, double zoom)
{
	int i, j, vis, prevvis;
	Point p, prev;
	Coastline *c;

	for(i = 0; i < ncoast; i++){
		c = &coasts[i];
		prevvis = 0;
		for(j = 0; j < c->npts; j++){
			geo2screen(r, clat, clon, zoom, c->pts[j], &p, &vis);
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

	cx = (r.min.x + r.max.x) / 2;
	cy = (r.min.y + r.max.y) / 2;
	rad = ((Dx(r) < Dy(r)) ? Dx(r) : Dy(r)) / 2 - 4;
	rad = (int)(rad * zoom);

	/* background */
	draw(dst, r, display->black, nil, ZP);

	/* globe disc */
	fillellipse(dst, Pt(cx, cy), rad, rad, ocean, ZP);

	/* grid */
	drawgrid(dst, r, clat, clon, zoom);

	/* coastlines */
	drawcoasts(dst, r, clat, clon, zoom);

	/* globe border */
	ellipse(dst, Pt(cx, cy), rad, rad, 1, coastcol, ZP);
}

void
drawstations(Image *dst, Rectangle r, double clat, double clon, double zoom,
	Station *s, int ns, int sel)
{
	int i, vis, dotr;
	Point p;
	Image *col;

	dotr = 3;
	if(zoom > 1.5)
		dotr = 4;
	if(zoom > 3.0)
		dotr = 5;

	for(i = 0; i < ns; i++){
		geo2screen(r, clat, clon, zoom, s[i].geo, &p, &vis);
		if(!vis)
			continue;
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

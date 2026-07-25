#include <u.h>
#include <libc.h>
#include <bio.h>
#include <draw.h>
#include <memdraw.h>

/*
 * mkearth - bake an equirectangular earth texture for radioglobe
 *
 * Two independent modes, both writing to standard output a file
 * radioglobe loads via -e (or ./earth.mask, or /lib/radio/earth.mask)
 * to draw a shaded solid globe instead of the flat ocean disc:
 *
 * 1. Default mode: reads coast.dat (mkcoast output: "> N" then N
 *    "lat lon" lines per polyline) on standard input, rasterizes
 *    every coastline segment into a w x w/2 pixel grid (x =
 *    longitude -180..180, y = latitude 90..-90, row 0 = north
 *    pole), flood-fills OCEAN from a handful of known open-ocean
 *    seed points (4-connected, wrapping in longitude), and writes:
 *
 *	EARTHMASK
 *	w h
 *	<w*h raw bytes, row-major from the north pole: 1=land 0=ocean>
 *
 *    The fill is topological, not geometric: anything the ocean
 *    flood cannot reach is land.  Boundary pixels (the rasterized
 *    coastlines themselves) count as land.  Consequences, all
 *    acceptable for a shading mask (the coastline overlay still
 *    draws exact outlines):
 *     - closed seas whose ring is in the data (e.g. the Caspian)
 *       come out as land;
 *     - Antarctica needs no special casing: the flood stops at its
 *       coastline and everything south of it stays land;
 *     - an UNCLOSED coastline ring in the data would let the flood
 *       leak into that landmass and mark it ocean.  The land
 *       percentage printed at the end is the tell: Earth is ~30%
 *       land by equirectangular pixel count; a dramatically lower
 *       number means a leak (use -t and look at the picture).
 *
 * 2. -i imagefile mode: converts an equirectangular photo/texture
 *    (e.g. NASA Blue Marble) instead of building a land/ocean mask
 *    from coast.dat.  imagefile must be an UNCOMPRESSED Plan 9
 *    image(6) file -- produce one from a JPEG/PNG/TIFF with, e.g.:
 *	jpg -9 -t earth.jpg > earth.plan9img
 *	mkearth -i earth.plan9img > earth.mask
 *    (-9: uncompressed image(6) output; -t: force true-color RGB.
 *    tif/png read the corresponding formats the same way.)  Writes:
 *
 *	EARTHTEX
 *	w h
 *	<w*h*3 raw bytes, row-major from the image's own row 0 (which
 *	 must be the north pole, as in any standard equirectangular
 *	 world map): B,G,R per pixel, matching Image channel RGB24>
 *
 *    w/h are taken directly from the source image -- no resampling
 *    is done, so whatever resolution you feed in is what radioglobe
 *    samples from.  In this mode coast.dat/stdin is not read at all.
 *
 * In both modes, radioglobe draws the vector coastline outline and
 * lat/lon grid from coast.dat on top of whichever earth is loaded,
 * so the visible outline stays exact either way; the mask/texture
 * only supplies the shaded fill underneath it.
 *
 * usage:
 *	mkearth [-w width] [-t preview.tga] < coast.dat > earth.mask
 *	mkearth -i imagefile > earth.mask
 */

enum {
	Unknown	= 0,	/* not reached by the flood: becomes land */
	Ocean	= 1,
	Bound	= 2,	/* rasterized coastline: counts as land */
};

static int W = 2048;
static int H;
static uchar *grid;

static void
plot(int x, int y)
{
	x %= W;
	if(x < 0)
		x += W;
	if(y < 0)
		y = 0;
	if(y >= H)
		y = H-1;
	grid[y*W + x] = Bound;
}

/*
 * Rasterize one segment as a straight line in equirectangular
 * pixel space (fine at coastline-segment scale).  DDA stepped at
 * >= one step per pixel in the dominant axis, so the drawn line
 * is 8-connected and the 4-connected ocean flood cannot leak
 * through it diagonally.
 */
static void
seg(double lat1, double lon1, double lat2, double lon2)
{
	double x1, y1, x2, y2, dx, dy;
	int i, n;

	/* antimeridian: take the short way; plot() wraps x */
	if(lon2 - lon1 > 180.0)
		lon2 -= 360.0;
	else if(lon1 - lon2 > 180.0)
		lon2 += 360.0;

	x1 = (lon1 + 180.0) / 360.0 * W;
	x2 = (lon2 + 180.0) / 360.0 * W;
	y1 = (90.0 - lat1) / 180.0 * H;
	y2 = (90.0 - lat2) / 180.0 * H;

	dx = x2 - x1;
	dy = y2 - y1;
	n = fabs(dx) > fabs(dy) ? (int)fabs(dx) : (int)fabs(dy);
	n += 2;
	for(i = 0; i <= n; i++)
		plot((int)(x1 + dx*i/n), (int)(y1 + dy*i/n));
}

/*
 * 4-connected BFS from open-ocean seeds, wrapping in longitude.
 * Several seeds, not one: at coarse widths a rasterized coastline
 * can pinch a strait shut (e.g. Bering) and split an ocean into
 * regions the other seeds still cover.
 */
static void
flood(void)
{
	static double seedlat[] = { 0, -30, -10, 85, -60 };	/* Pacific, S.Atlantic, Indian, Arctic, Drake */
	static double seedlon[] = { -150, -20, 80, 0, -100 };
	static int dx4[] = { 1, -1, 0, 0 };
	static int dy4[] = { 0, 0, 1, -1 };
	int *q;
	int qh, qt, x, y, i, p, nx, ny;

	q = malloc((long)W*H*sizeof(int));
	if(q == nil)
		sysfatal("malloc: %r");
	qh = qt = 0;
	for(i = 0; i < nelem(seedlat); i++){
		x = (int)((seedlon[i] + 180.0) / 360.0 * W) % W;
		y = (int)((90.0 - seedlat[i]) / 180.0 * H);
		if(y < 0) y = 0;
		if(y >= H) y = H-1;
		if(grid[y*W + x] != Unknown)
			continue;
		grid[y*W + x] = Ocean;
		q[qt++] = y*W + x;
	}
	while(qh < qt){
		p = q[qh++];
		x = p % W;
		y = p / W;
		for(i = 0; i < 4; i++){
			nx = x + dx4[i];
			ny = y + dy4[i];
			if(ny < 0 || ny >= H)
				continue;
			if(nx < 0)
				nx += W;
			if(nx >= W)
				nx -= W;
			if(grid[ny*W + nx] != Unknown)
				continue;
			grid[ny*W + nx] = Ocean;
			q[qt++] = ny*W + nx;
		}
	}
	free(q);
}

/* minimal uncompressed 24-bit TGA, top-left origin, for eyeballing */
static void
writepreview(char *path)
{
	int fd, x, y;
	uchar hdr[18], *row;

	fd = create(path, OWRITE, 0666);
	if(fd < 0){
		fprint(2, "mkearth: can't create %s: %r\n", path);
		return;
	}
	memset(hdr, 0, sizeof hdr);
	hdr[2] = 2;			/* uncompressed RGB */
	hdr[12] = W & 0xff;
	hdr[13] = (W>>8) & 0xff;
	hdr[14] = H & 0xff;
	hdr[15] = (H>>8) & 0xff;
	hdr[16] = 24;
	hdr[17] = 0x20;			/* top-left origin */
	write(fd, hdr, sizeof hdr);
	row = malloc(W*3);
	if(row == nil)
		sysfatal("malloc: %r");
	for(y = 0; y < H; y++){
		for(x = 0; x < W; x++){
			if(grid[y*W + x] == Ocean){
				row[x*3+0] = 0x3a;	/* B */
				row[x*3+1] = 0x1a;	/* G */
				row[x*3+2] = 0x0a;	/* R */
			}else{
				row[x*3+0] = 0x5a;
				row[x*3+1] = 0x8a;
				row[x*3+2] = 0x4a;
			}
		}
		write(fd, row, W*3);
	}
	free(row);
	close(fd);
}

/*
 * Convert an uncompressed Plan 9 image file (imagefile) into an
 * EARTHTEX texture on standard output, and exit -- coast.dat/stdin
 * is not touched in this mode.  No resampling: output dimensions are
 * exactly the source image's.  memimagedraw() into a fresh RGB24
 * Memimage does whatever channel conversion is needed (e.g. a
 * greyscale or paletted source), so this works regardless of the
 * source's original chan, not just already-RGB24 input.
 */
static void
mktexture(char *imagefile)
{
	int fd, w, h;
	long n;
	uchar *buf;
	Memimage *si, *ti;
	Biobuf bout;

	if(memimageinit() != 0)
		sysfatal("memimageinit: %r");

	fd = open(imagefile, OREAD);
	if(fd < 0)
		sysfatal("can't open %s: %r", imagefile);
	si = readmemimage(fd);
	close(fd);
	if(si == nil)
		sysfatal("readmemimage %s: %r (must be an UNCOMPRESSED "
			"Plan 9 image -- see jpg/tif/png -9)", imagefile);

	w = Dx(si->r);
	h = Dy(si->r);
	if(w < 2 || h < 2)
		sysfatal("%s: bad image size %dx%d", imagefile, w, h);

	ti = allocmemimage(si->r, RGB24);
	if(ti == nil)
		sysfatal("allocmemimage: %r");
	memimagedraw(ti, ti->r, si, si->r.min, nil, ZP, S);

	n = (long)w*h*3;
	buf = malloc(n);
	if(buf == nil)
		sysfatal("malloc: %r");
	if(unloadmemimage(ti, ti->r, buf, n) != n)
		sysfatal("unloadmemimage: %r");

	Binit(&bout, 1, OWRITE);
	Bprint(&bout, "EARTHTEX\n%d %d\n", w, h);
	Bwrite(&bout, buf, n);
	Bterm(&bout);

	fprint(2, "mkearth: %s -> %dx%d RGB texture\n", imagefile, w, h);
	exits(nil);
}

static void
usage(void)
{
	fprint(2, "usage: %s [-w width] [-t preview.tga] < coast.dat > earth.mask\n", argv0);
	fprint(2, "       %s -i imagefile > earth.mask\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	Biobuf bin, bout;
	char *ln, *p, *f[2], *preview, *imagefile;
	double lat, lon, lastlat, lastlon;
	int havelast, npoly, npts;
	long i, nland;

	preview = nil;
	imagefile = nil;
	ARGBEGIN{
	case 'w':
		W = atoi(EARGF(usage()));
		if(W < 64 || W > 16384 || W%2)
			usage();
		break;
	case 't':
		preview = EARGF(usage());
		break;
	case 'i':
		imagefile = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	/*
	 * -i is a wholly separate mode: convert an image and exit,
	 * without touching stdin/coast.dat or any of -w/-t (which
	 * only apply to the land/ocean mask path below).
	 */
	if(imagefile != nil)
		mktexture(imagefile);

	H = W/2;
	grid = mallocz((long)W*H, 1);
	if(grid == nil)
		sysfatal("malloc: %r");

	Binit(&bin, 0, OREAD);
	havelast = 0;
	lastlat = 0.0;
	lastlon = 0.0;
	npoly = 0;
	npts = 0;
	while((ln = Brdstr(&bin, '\n', 1)) != nil){
		p = ln + strspn(ln, " \t");
		if(*p == '#' || *p == 0){
			free(ln);
			continue;
		}
		if(*p == '>'){
			havelast = 0;
			npoly++;
			free(ln);
			continue;
		}
		if(tokenize(p, f, 2) == 2){
			lat = atof(f[0]);
			lon = atof(f[1]);
			if(havelast)
				seg(lastlat, lastlon, lat, lon);
			lastlat = lat;
			lastlon = lon;
			havelast = 1;
			npts++;
		}
		free(ln);
	}
	Bterm(&bin);

	if(npts == 0)
		sysfatal("no coastline points on input (expected coast.dat)");

	flood();

	if(preview != nil)
		writepreview(preview);

	nland = 0;
	for(i = 0; i < (long)W*H; i++){
		grid[i] = grid[i] == Ocean ? 0 : 1;
		nland += grid[i];
	}

	Binit(&bout, 1, OWRITE);
	Bprint(&bout, "EARTHMASK\n%d %d\n", W, H);
	Bwrite(&bout, grid, (long)W*H);
	Bterm(&bout);

	fprint(2, "mkearth: %d polylines, %d points -> %dx%d mask, %.1f%% land\n",
		npoly, npts, W, H, 100.0*nland/((double)W*H));
	exits(nil);
}

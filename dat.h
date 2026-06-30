typedef struct Station Station;
typedef struct Geo Geo;
typedef struct Vec3 Vec3;

struct Geo {
	double lat;	/* degrees, north positive */
	double lon;	/* degrees, east positive */
};

/* unit vector on the sphere, precomputed from a Geo so per-frame
 * projection is a few multiplies/adds instead of sin/cos/asin/atan2 */
struct Vec3 {
	double x, y, z;
};

struct Station {
	char *name;
	char *url;
	Geo geo;
	Vec3 vec;	/* precomputed unit vector, set via geo2vec() at load time */
};

/* globe.c */
void	globeinit(void);
void	globedraw(Image *dst, Rectangle r, double clat, double clon, double zoom);
int	globehit(Rectangle r, double clat, double clon, double zoom, Point xy, Geo *g);
void	geo2screen(Rectangle r, double clat, double clon, double zoom, Geo g, Point *p, int *visible);
void	geo2vec(Geo g, Vec3 *v);
void	drawstations(Image *dst, Rectangle r, double clat, double clon, double zoom, Station *s, int ns, int sel);

/* coast.c - coastline polygon data */
typedef struct Coastline Coastline;
struct Coastline {
	int npts;
	Geo *pts;
	Vec3 *vec;	/* precomputed unit vectors, one per pts[] entry */

	/*
	 * Bounding cap: the smallest cone, centered on "center" and
	 * with half-angle theta (cos(theta) == capcos), that
	 * contains every point of this polyline.  Lets drawcoasts()
	 * reject the whole polyline -- with a single dot product --
	 * when it cannot possibly have any point facing the viewer,
	 * instead of projecting every one of its points first.
	 *
	 * capcos == -2 is a sentinel meaning "don't bother culling
	 * this one" (used when the points are spread out so widely
	 * that a single center direction isn't meaningful); the
	 * per-point visibility test in drawcoasts() still renders
	 * it correctly either way.
	 */
	Vec3 center;
	double capcos;
};

extern Coastline *coasts;
extern int ncoast;
void	coastinit(void);
void	coastfile(char *path);

typedef struct Station Station;
typedef struct Geo Geo;

struct Geo {
	double lat;	/* degrees, north positive */
	double lon;	/* degrees, east positive */
};

struct Station {
	char *name;
	char *url;
	Geo geo;
};

/* globe.c */
void	globeinit(void);
void	globedraw(Image *dst, Rectangle r, double clat, double clon, double zoom);
int	globehit(Rectangle r, double clat, double clon, double zoom, Point xy, Geo *g);
void	geo2screen(Rectangle r, double clat, double clon, double zoom, Geo g, Point *p, int *visible);
void	drawstations(Image *dst, Rectangle r, double clat, double clon, double zoom, Station *s, int ns, int sel);

/* coast.c - coastline polygon data */
typedef struct Coastline Coastline;
struct Coastline {
	int npts;
	Geo *pts;
};

extern Coastline *coasts;
extern int ncoast;
void	coastinit(void);
void	coastfile(char *path);

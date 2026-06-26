#include <u.h>
#include <libc.h>
#include <bio.h>
#include <json.h>

/*
 * mkstations - convert radio-browser.info JSON into
 * radioglobe's station-file format.
 *
 *	mkstations [-a] [-b minbitrate] < stations.json > stations
 *
 * Fetch the input with (no network in the agent, run yourself):
 *   hget 'https://de1.api.radio-browser.info/json/stations/search?\
 *     limit=1000&has_geo_info=true&hidebroken=true&\
 *     order=clickcount&reverse=true' > /tmp/stations.json
 *
 * The response is a JSON array of objects.  We use:
 *	name		station name
 *	url_resolved	stream url (preferred); falls back to url
 *	geo_lat		latitude
 *	geo_long	longitude (note: "geo_long", not "geo_lon")
 *	codec		MP3 / OGG / AAC / ...
 *	bitrate		kbps (number)
 *
 * By default MP3, OGG/Vorbis, and AAC/AAC+ are emitted (an
 * aacdec must be installed for AAC).  HLS (.m3u8) playlists are
 * still skipped since the decoders cannot demux them.  -a keeps
 * every codec and HLS.  -b sets a minimum bitrate.
 *
 * Output line format:  lat lon "name" url
 */

Biobuf *bout;
int keepall;
int minbitrate;

static char *
readall(int fd)
{
	char *buf;
	long n, sz, tot;

	sz = 1<<20;
	buf = malloc(sz);
	if(buf == nil)
		sysfatal("malloc: %r");
	tot = 0;
	for(;;){
		if(tot >= sz){
			sz *= 2;
			buf = realloc(buf, sz);
			if(buf == nil)
				sysfatal("realloc: %r");
		}
		n = read(fd, buf+tot, sz-tot);
		if(n < 0)
			sysfatal("read: %r");
		if(n == 0)
			break;
		tot += n;
	}
	buf = realloc(buf, tot+1);
	if(buf == nil)
		sysfatal("realloc: %r");
	buf[tot] = 0;
	return buf;
}

/* json value -> number, with default if missing/not a number */
static double
numof(JSON *o, char *key, double def)
{
	JSON *v;

	v = jsonbyname(o, key);
	if(v == nil)
		return def;
	if(v->t == JSONNumber)
		return v->n;
	/* radio-browser sometimes returns geo as strings */
	if(v->t == JSONString && v->s != nil && v->s[0] != 0)
		return atof(v->s);
	return def;
}

/* json value -> string, or nil */
static char *
strof(JSON *o, char *key)
{
	JSON *v;

	v = jsonbyname(o, key);
	if(v == nil)
		return nil;
	return jsonstr(v);
}

/*
 * write a name into a quoted field, sanitizing: collapse
 * whitespace, drop control chars and embedded double quotes.
 */
static void
emitname(char *s)
{
	Rune r;
	int n, sp;

	Bputc(bout, '"');
	sp = 0;
	while(*s){
		n = chartorune(&r, s);
		s += n;
		if(r == '"')
			r = '\'';
		if(r < 0x20 || r == 0x7f || r == ' ' || r == '\t'){
			sp = 1;
			continue;
		}
		if(sp){
			Bputc(bout, ' ');
			sp = 0;
		}
		Bprint(bout, "%C", r);
	}
	Bputc(bout, '"');
}

static int
codecok(char *codec)
{
	if(keepall)
		return 1;
	if(codec == nil)
		return 0;
	if(cistrcmp(codec, "MP3") == 0)
		return 1;
	if(cistrcmp(codec, "OGG") == 0)
		return 1;
	if(cistrcmp(codec, "VORBIS") == 0)
		return 1;
	/*
	 * AAC and AAC+ are kept now that an aacdec is installed.
	 * radio-browser reports AAC+ (HE-AAC) as "AAC+".  These
	 * are still subject to the HLS filter below: a raw AAC
	 * icecast stream plays, an HLS (.m3u8) playlist does not.
	 */
	if(cistrcmp(codec, "AAC") == 0)
		return 1;
	if(cistrcmp(codec, "AAC+") == 0)
		return 1;
	return 0;
}

static int
emitstation(JSON *o)
{
	char *name, *url, *codec;
	double lat, lon, br, hls;

	if(o == nil || o->t != JSONObject)
		return 0;

	name = strof(o, "name");
	url = strof(o, "url_resolved");
	if(url == nil || url[0] == 0)
		url = strof(o, "url");
	codec = strof(o, "codec");
	lat = numof(o, "geo_lat", 1e9);
	lon = numof(o, "geo_long", 1e9);
	br = numof(o, "bitrate", 0);
	hls = numof(o, "hls", 0);

	if(name == nil || name[0] == 0)
		return 0;
	if(url == nil || url[0] == 0)
		return 0;
	if(lat > 90.0 || lat < -90.0 || lon > 180.0 || lon < -180.0)
		return 0;	/* missing/garbage geo */
	if(lat == 0.0 && lon == 0.0)
		return 0;	/* null island: almost always bad data */
	if(!codecok(codec))
		return 0;
	if(minbitrate > 0 && br > 0 && br < minbitrate)
		return 0;

	/*
	 * HLS (.m3u8) streams aren't handled by the standard 9front
	 * audio decoders; skip them unless keepall is set.
	 */
	if(!keepall){
		if(hls != 0.0)
			return 0;
		if(strstr(url, ".m3u8") != nil)
			return 0;
	}

	/* url must not contain whitespace (it is the last field) */
	if(strpbrk(url, " \t\r\n") != nil)
		return 0;

	Bprint(bout, "%.4f %.4f ", lat, lon);
	emitname(name);
	Bprint(bout, " %s\n", url);
	return 1;
}

void
main(int argc, char **argv)
{
	char *data;
	JSON *root;
	JSONEl *e;
	int n;

	keepall = 0;
	minbitrate = 0;

	ARGBEGIN{
	case 'a':
		keepall = 1;
		break;
	case 'b':
		minbitrate = atoi(EARGF(fprint(2,
			"usage: %s [-a] [-b minbitrate] < stations.json > stations\n",
			argv0)));
		break;
	default:
		fprint(2, "usage: %s [-a] [-b minbitrate] < stations.json > stations\n", argv0);
		exits("usage");
	}ARGEND
	USED(argc); USED(argv);

	data = readall(0);
	root = jsonparse(data);
	if(root == nil)
		sysfatal("jsonparse: %r");

	bout = Bfdopen(1, OWRITE);
	if(bout == nil)
		sysfatal("Bfdopen: %r");

	Bprint(bout, "# radioglobe station list\n");
	Bprint(bout, "# generated by mkstations from radio-browser.info\n");
	Bprint(bout, "# format: lat lon \"name\" url\n");

	n = 0;
	if(root->t == JSONArray){
		for(e = root->first; e != nil; e = e->next)
			n += emitstation(e->val);
	}else if(root->t == JSONObject){
		/* a single station object */
		n += emitstation(root);
	}else
		sysfatal("unexpected json: not an array of stations");

	Bterm(bout);
	jsonfree(root);
	free(data);

	fprint(2, "%s: %d stations\n", argv0, n);
	exits(nil);
}

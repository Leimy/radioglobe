#include <u.h>
#include <libc.h>
#include "util.h"

/*
 * Slurp an entire file descriptor into a NUL-terminated malloced
 * buffer.  Shared by mkcoast and mkstations, both of which parse
 * their whole (potentially large) JSON input in one go.
 */
char *
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

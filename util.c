#include <u.h>
#include <libc.h>
#include <bio.h>
#include "util.h"

/*
 * Slurp an entire file descriptor into a NUL-terminated malloced
 * buffer.  Shared by mkcoast and mkstations, both of which parse
 * their whole (potentially large) JSON input in one go.
 *
 * Brdstr(_, '\0', 0) does the work: if it hits end of file before
 * finding the delimiter byte, it treats that the same as finding
 * it, so it returns everything read so far as one malloced,
 * NUL-terminated buffer.  Since our JSON input never contains an
 * embedded NUL, asking for delimiter '\0' amounts to "read to EOF".
 */
char *
readall(int fd)
{
	Biobuf b;
	char *s;

	if(Binit(&b, fd, OREAD) < 0)
		sysfatal("Binit: %r");
	s = Brdstr(&b, '\0', 0);
	Bterm(&b);
	if(s == nil)
		s = strdup("");	/* empty input */
	return s;
}

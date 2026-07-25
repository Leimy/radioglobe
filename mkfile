</$objtype/mkfile

BIN=/$objtype/bin

TARG=\
	radioglobe\
	mkcoast\
	mkstations\
	mkearth\

LIBVIEW=/usr/dave/work/libview/libview.a$O

GLOBEOFILES=\
	main.$O\
	globe.$O\
	coast.$O\

HFILES=\
	dat.h\
	util.h\

default:V: all

all:V: $TARG

$LIBVIEW:V:
	@{cd /usr/dave/work/libview && mk}

radioglobe: $GLOBEOFILES $LIBVIEW
	$LD -o $target $GLOBEOFILES $LIBVIEW

mkcoast: mkcoast.$O util.$O
	$LD -o $target mkcoast.$O util.$O -ljson

mkstations: mkstations.$O util.$O
	$LD -o $target mkstations.$O util.$O -ljson

mkearth: mkearth.$O
	$LD -o $target mkearth.$O

%.$O: %.c $HFILES
	$CC $CFLAGS -I/usr/dave/work/libview $stem.c

install:V: $TARG
	for(i in $TARG) cp $i $BIN/$i

installdata:V:
	mkdir -p /lib/radio
	if(test -f stations) cp stations /lib/radio/stations
	if(test -f coast.dat) cp coast.dat /lib/radio/coast.dat
	if(test -f earth.mask) cp earth.mask /lib/radio/earth.mask

clean:V:
	rm -f *.[$OS] [$OS].out $TARG

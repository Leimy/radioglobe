</$objtype/mkfile

BIN=/$objtype/bin

TARG=\
	radioglobe\
	mkcoast\
	mkstations\

GLOBEOFILES=\
	main.$O\
	globe.$O\
	coast.$O\

HFILES=\
	dat.h\
	util.h\

default:V: all

all:V: $TARG

radioglobe: $GLOBEOFILES
	$LD -o $target $GLOBEOFILES

mkcoast: mkcoast.$O util.$O
	$LD -o $target mkcoast.$O util.$O -ljson

mkstations: mkstations.$O util.$O
	$LD -o $target mkstations.$O util.$O -ljson

%.$O: %.c $HFILES
	$CC $CFLAGS $stem.c

install:V: $TARG
	for(i in $TARG) cp $i $BIN/$i

installdata:V:
	mkdir -p /lib/radio
	if(test -f stations) cp stations /lib/radio/stations
	if(test -f coast.dat) cp coast.dat /lib/radio/coast.dat

clean:V:
	rm -f *.[$OS] [$OS].out $TARG

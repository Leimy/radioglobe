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

CLEANFILES=$TARG

default:V: all

all:V: $TARG

radioglobe: $GLOBEOFILES
	$LD -o $target $GLOBEOFILES

mkcoast: mkcoast.$O
	$LD -o $target mkcoast.$O -ljson

mkstations: mkstations.$O
	$LD -o $target mkstations.$O -ljson

%.$O: %.c $HFILES
	$CC $CFLAGS $stem.c

install:V: $TARG
	for(i in $TARG) cp $i $BIN/$i

installdata:V:
	mkdir -p /lib/radio
	test -f stations && cp stations /lib/radio/stations
	test -f coast.dat && cp coast.dat /lib/radio/coast.dat

clean:V:
	rm -f *.[$OS] [$OS].out $TARG $CLEANFILES

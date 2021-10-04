# program name
PROG = xfiles

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man
LOCALINC = /usr/local/include
LOCALLIB = /usr/local/lib
X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib
FREETYPEINC = /usr/include/freetype2
FREETYPEINCOBSD = ${X11INC}/freetype2

# includes and libs
INCS = -I${LOCALINC} -I${X11INC} -I${FREETYPEINC} -I${FREETYPEINCOBSD}
LIBS = -L${LOCALLIB} -L${X11LIB} -lm -lfontconfig -lXft -lX11 -lXpm -lpthread

# flags
CFLAGS = -g -O0 -Wall -Wextra ${INCS} ${CPPFLAGS}
LDFLAGS = ${LIBS}

bindir = ${DESTDIR}${PREFIX}
mandir = ${DESTDIR}${MANPREFIX}

SRCS = ${PROG}.c
OBJS = ${SRCS:.c=.o}

all: ${PROG}

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LDFLAGS}

${OBJS}: config.h

.c.o:
	${CC} ${CFLAGS} -c $<

clean:
	-rm ${OBJS} ${PROG}

install: all
	mkdir -p ${bindir}/bin
	install -m 755 ${PROG} ${bindir}/bin/${PROG}
	mkdir -p ${mandir}/man1
	install -m 644 ${PROG}.1 ${mandir}/man1/${PROG}.1

uninstall:
	rm -f ${bindir}/bin/${PROG}
	rm -f ${mandir}/man1/${PROG}.1

.PHONY: all clean install uninstall

PROG = xfiles
OBJS = ${PROG:=.o} widget.o util.o
SRCS = ${OBJS:.o=.c}
MANS = ${PROG:=.1}

PREFIX ?= /usr/local
MANPREFIX ?= ${PREFIX}/share/man
LOCALINC ?= /usr/local/include
LOCALLIB ?= /usr/local/lib
X11INC ?= /usr/X11R6/include
X11LIB ?= /usr/X11R6/lib

INCS = -I${LOCALINC} -I${X11INC} -I/usr/include/freetype2 -I${X11INC}/freetype2
LIBS = -L${LOCALLIB} -L${X11LIB} -lfontconfig -lXft -lX11 -lXext -lXpm -lpthread

all: ${PROG}

${PROG}: ${OBJS}
	${CC} ${LIBS} ${LDFLAGS} -o $@ ${OBJS}

.c.o:
	${CC} ${INCS} ${CFLAGS} ${CPPFLAGS} -c $<

xfiles.o: util.h widget.h
widget.o: util.h widget.h winicon.data fileicon.xpm

tags: ${SRCS}
	ctags ${SRCS}

clean:
	rm -f ${OBJS} ${PROG} ${PROG:=.core} tags

install: all
	install -d ${DESTDIR}${PREFIX}/bin
	install -d ${DESTDIR}${MANPREFIX}/man1
	install -m 755 thumbnail ${DESTDIR}${PREFIX}/bin/thumbnail
	install -m 755 ${PROG} ${DESTDIR}${PREFIX}/bin/${PROG}
	install -m 644 ${MANS} ${DESTDIR}${MANPREFIX}/man1/${MANS}

uninstall:
	rm ${DESTDIR}${PREFIX}/bin/thumbnail
	rm ${DESTDIR}${PREFIX}/bin/${PROG}
	rm ${DESTDIR}${MANPREFIX}/man1/${MANS}

.PHONY: all tags clean install uninstall

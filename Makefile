PROG = xfiles
OBJS = ${PROG:=.o} widget.o util.o config.o
SRCS = ${OBJS:.o=.c}
MANS = ${PROG:=.1}

PREFIX ?= /usr/local
MANPREFIX ?= ${PREFIX}/share/man
LOCALINC ?= /usr/local/include
LOCALLIB ?= /usr/local/lib
X11INC ?= /usr/X11R6/include
X11LIB ?= /usr/X11R6/lib

INCS = -I${LOCALINC} -I${X11INC} -I/usr/include/freetype2 -I${X11INC}/freetype2
LIBS = -L${LOCALLIB} -L${X11LIB} -lfontconfig -lXft -lX11 -lXext -lXrender -lXcursor -lXpm -lpthread

all: ${PROG}

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LIBS} ${LDFLAGS}

.c.o:
	${CC} -std=c99 -pedantic -D_POSIX_C_SOURCE=200809L \
	${INCS} ${CFLAGS} ${CPPFLAGS} -c $<

xfiles.o: util.h widget.h icons/file.xpm icons/folder.xpm
widget.o: util.h widget.h winicon.data icons/x.xpm

tags: ${SRCS}
	ctags ${SRCS}

clean:
	rm -f ${OBJS} ${PROG} ${PROG:=.core} tags

install: all
	install -d ${DESTDIR}${PREFIX}/bin
	install -d ${DESTDIR}${MANPREFIX}/man1
	install -m 755 ${PROG} ${DESTDIR}${PREFIX}/bin/${PROG}
	install -m 644 ${MANS} ${DESTDIR}${MANPREFIX}/man1/${MANS}

uninstall:
	rm ${DESTDIR}${PREFIX}/bin/${PROG}
	rm ${DESTDIR}${MANPREFIX}/man1/${MANS}

.PHONY: all tags clean install uninstall

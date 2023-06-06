PROG = xfiles
OBJS = ${PROG:=.o} widget.o util.o icons.o ctrlsel.o ctrlfnt.o
SRCS = ${OBJS:.o=.c}
MAN  = ${PROG:=.1}
SCRIPT1 = xfilesctl
SCRIPT2 = xfilesthumb
ICONS = \
	icons/file-app.xpm \
	icons/file-archive.xpm \
	icons/file-audio.xpm \
	icons/file-code.xpm \
	icons/file-config.xpm \
	icons/file-core.xpm \
	icons/file-gear.xpm \
	icons/file-image.xpm \
	icons/file-info.xpm \
	icons/file-object.xpm \
	icons/file-text.xpm \
	icons/file-video.xpm \
	icons/file.xpm \
	icons/folder-apps.xpm \
	icons/folder-book.xpm \
	icons/folder-code.xpm \
	icons/folder-db.xpm \
	icons/folder-download.xpm \
	icons/folder-game.xpm \
	icons/folder-gear.xpm \
	icons/folder-home.xpm \
	icons/folder-image.xpm \
	icons/folder-link.xpm \
	icons/folder-mail.xpm \
	icons/folder-meme.xpm \
	icons/folder-mount.xpm \
	icons/folder-music.xpm \
	icons/folder-trash.xpm \
	icons/folder-up.xpm \
	icons/folder-video.xpm \
	icons/folder.xpm

PREFIX ?= /usr/local
MANPREFIX ?= ${PREFIX}/share/man
LOCALINC ?= /usr/local/include
LOCALLIB ?= /usr/local/lib
X11INC ?= /usr/X11R6/include
X11LIB ?= /usr/X11R6/lib

# Add -DCTRLFNT_NO_SEARCH to disable fallback font search at runtime
DEFS = -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -D_BSD_SOURCE -D_DEFAULT_SOURCE
INCS = -I${LOCALINC} -I${X11INC} -I/usr/include/freetype2 -I${X11INC}/freetype2
LIBS = -L${LOCALLIB} -L${X11LIB} -lfontconfig -lXft -lX11 -lXext -lXcursor -lXrender -lXpm -lpthread

all: ${PROG}

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LIBS} ${LDFLAGS}

.c.o:
	${CC} -std=c99 -pedantic ${DEFS} ${INCS} ${CFLAGS} ${CPPFLAGS} -c $<

xfiles.o:  util.h widget.h icons/file.xpm icons/folder.xpm
widget.o:  util.h widget.h ctrlsel.h ctrlfnt.h winicon.data icons/x.xpm
ctrlsel.o: ctrlsel.h
ctrlfnt.o: ctrlfnt.h
icons.o: ${ICONS}

tags: ${SRCS}
	ctags ${SRCS}

lint: ${SRCS}
	-mandoc -T lint -W warning ${MAN} ctrlfnt.3 ctrlsel.3
	-clang-tidy ${SRCS} -- -std=c99 ${DEFS} ${INCS} ${CPPFLAGS}

clean:
	rm -f ${OBJS} ${PROG} ${PROG:=.core} tags

install: all
	install -d ${DESTDIR}${PREFIX}/bin
	install -d ${DESTDIR}${MANPREFIX}/man1
	install -m 755 ${PROG} ${DESTDIR}${PREFIX}/bin/${PROG}
	install -m 644 ${MAN} ${DESTDIR}${MANPREFIX}/man1/${MAN}

install-script:
	install -m 755 examples/${SCRIPT1} ${DESTDIR}${PREFIX}/bin/${SCRIPT1}
	install -m 755 examples/${SCRIPT2} ${DESTDIR}${PREFIX}/bin/${SCRIPT2}

uninstall:
	-rm ${DESTDIR}${PREFIX}/bin/${PROG}
	-rm ${DESTDIR}${MANPREFIX}/man1/${MAN}

uninstall-script:
	-rm ${DESTDIR}${PREFIX}/bin/${SCRIPT1}
	-rm ${DESTDIR}${PREFIX}/bin/${SCRIPT2}

.PHONY: all tags clean install uninstall lint

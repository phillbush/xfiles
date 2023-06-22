PROG = xfiles
OBJS = ${PROG:=.o} widget.o util.o icons.o ctrlsel.o ctrlfnt.o
SRCS = ${OBJS:.o=.c}
MAN  = ${PROG:=.1}
SCRIPTS = \
	examples/xfilesctl \
	examples/xfilesthumb
WINICONS = \
	icons/winicon16x16.abgr \
	icons/winicon32x32.abgr \
	icons/winicon48x48.abgr \
	icons/winicon64x64.abgr
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

.SUFFIXES: .c .o .abgr .xpm

# Add -DCTRLFNT_NO_SEARCH to disable fallback font search at runtime
DEFS = -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -D_BSD_SOURCE -D_DEFAULT_SOURCE
INCS = -I${LOCALINC} -I${X11INC} -I/usr/include/freetype2 -I${X11INC}/freetype2
LIBS = -L${LOCALLIB} -L${X11LIB} -lfontconfig -lXft -lX11 -lXext -lXcursor -lXrender -lXpm -lpthread

bindir = ${DESTDIR}${PREFIX}/bin
mandir = ${DESTDIR}${MANPREFIX}/man1

all: ${PROG}

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${LIBS} ${LDFLAGS}

.c.o:
	${CC} -std=c99 -pedantic ${DEFS} ${INCS} ${CFLAGS} ${CPPFLAGS} -c $<

# Convert XPM images into ARGB raw images.  Requires Imagemagick and
# hexdump.  You need not run this.  The source code is released with
# the converted files included.
.xpm.abgr:
	{ \
		printf 'unsigned long %s[] = {\n' "$<" | \
		sed 's,icons/,,;s,.xpm,,' ; \
		convert $< -color-matrix '0 0 1 0, 0 1 0 0, 1 0 0 0, 0 0 0 1' RGBA:- | \
		hexdump -v -e '1/4 "0x%08x,\n"' ; \
		printf '};\n' ; \
	} >$@

xfiles.o:  util.h widget.h icons/file.xpm icons/folder.xpm
widget.o:  util.h widget.h ctrlsel.h ctrlfnt.h icons/x.xpm
ctrlsel.o: ctrlsel.h
ctrlfnt.o: ctrlfnt.h
icons.o: ${ICONS} ${WINICONS}

tags: ${SRCS}
	ctags ${SRCS}

# Grep for commented-out code and TODO/XXX comments; lint the manual and
# the source code.  Requires mandoc and clang.  You need not run this.
# The source code is released after it has been linted.
lint: ${SRCS} ${ICONS} ${WINICONS}
	-@fgrep -e '	//' -e 'TODO' -e 'XXX' ${SRCS} || true
	-shellcheck ${SCRIPTS}
	-mandoc -T lint -W warning ${MAN} ctrlfnt.3 ctrlsel.3
	-clang-tidy ${SRCS} -- -std=c99 ${DEFS} ${INCS} ${CPPFLAGS}

clean:
	rm -f ${OBJS} ${PROG} ${PROG:=.core} tags

install: all
	mkdir -p ${bindir}
	mkdir -p ${mandir}
	install -m 755 ${PROG} ${bindir}/${PROG}
	install -m 644 ${MAN} ${mandir}/${MAN}

uninstall:
	-rm ${bindir}/${PROG}
	-rm ${mandir}/${MAN}

.PHONY: all tags clean install uninstall lint

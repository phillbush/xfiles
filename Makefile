.SUFFIXES: .c .o .dbg

PROG = xfiles

DEBUG_PROG = ${PROG:=_dbg}

OBJS = \
	${PROG:=.o} \
	widget.o util.o icons.o \
	control/dragndrop.o \
	control/selection.o \
	control/font.o

DEBUG_OBJS = ${OBJS:.o=.dbg}

SRCS = ${OBJS:.o=.c}

MANS = \
	xfiles.1 \
	control/ctrldnd.3 \
	control/ctrlfnt.3 \
	control/ctrlsel.3

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

PROG_CPPFLAGS = \
	-D_POSIX_C_SOURCE=200809L -D_BSD_SOURCE -D_GNU_SOURCE -D_DEFAULT_SOURCE \
	-I. -I/usr/local/include -I/usr/X11R6/include \
	-I/usr/include/freetype2 -I/usr/X11R6/include/freetype2 \
	${CPPFLAGS}

PROG_CFLAGS = \
	-std=c99 -pedantic \
	${PROG_CPPFLAGS} \
	${CFLAGS}

PROG_LDFLAGS = \
	-L/usr/local/lib -L/usr/X11R6/lib \
	-lfontconfig -lXft -lX11 -lXext -lXcursor -lXrender -lXpm -lpthread \
	${LDFLAGS} ${LDLIBS}

DEBUG_FLAGS = \
	-g -O0 -DDEBUG -Wall -Wextra -Wpedantic

all: ${PROG}
${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${PROG_LDFLAGS}
.c.o:
	${CC} ${PROG_CFLAGS} -o $@ -c $<

debug: ${DEBUG_PROG}
${DEBUG_PROG}: ${DEBUG_OBJS}
	${CC} -o $@ ${DEBUG_OBJS} ${PROG_LDFLAGS} ${DEBUG_FLAGS}
.c.dbg:
	${CC} ${PROG_CFLAGS} ${DEBUG_FLAGS} -o $@ -c $<

# Brace expansion in makefile targets is a {GNU,BSD} extension.
# Should we make this portable? (How?)
control/selection.{dbg,o}: control/selection.h
control/dragndrop.{dbg,o}: control/dragndrop.h control/selection.h
control/font.{dbg,o}:      control/font.h
xfiles.{dbg,o}:  util.h widget.h icons/file.xpm icons/folder.xpm
widget.{dbg,o}:  util.h widget.h icons/x.xpm control/selection.h control/dragndrop.h control/font.h
icons.{dbg,o}:   ${ICONS} ${WINICONS}

lint: ${SCRIPTS} ${MANS}
	-shellcheck ${SCRIPTS}
	-mandoc -T lint -W warning ${MANS}

tags: ${SRCS}
	ctags ${SRCS}

clean:
	rm -f ${OBJS} ${PROG} ${PROG:=.core}

distclean: clean
	rm -f ${DEBUG_OBJS} ${DEBUG_PROG} ${DEBUG_PROG:=.core} tags
	rm -f tags

.PHONY: all debug lint clean distclean

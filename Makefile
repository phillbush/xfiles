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

PROG_CPPFLAGS = \
	-D_POSIX_C_SOURCE=200809L -D_BSD_SOURCE -D_DEFAULT_SOURCE \
	-I/usr/local/include -I/usr/X11R6/include \
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

PROG_DEBUG = \
	-g -Og -Wall -Wextra -Wpedantic
#	-Wdouble-promotion -Wconversion

.SUFFIXES: .c .o .abgr .xpm

all: ${PROG}

${PROG}: ${OBJS}
	${CC} -o $@ ${OBJS} ${PROG_LDFLAGS}

.c.o:
	${CC} ${PROG_CFLAGS} -o $@ -c $<

xfiles.o:  util.h widget.h icons/file.xpm icons/folder.xpm
widget.o:  util.h widget.h ctrlsel.h ctrlfnt.h icons/x.xpm
ctrlsel.o: ctrlsel.h
ctrlfnt.o: ctrlfnt.h
icons.o:   ${ICONS} ${WINICONS}

tags: ${SRCS}
	ctags ${SRCS}

# Run "make all" with explicit debugging flags
all-debug:
	@${MAKE} all \
	CFLAGS="${CFLAGS} ${PROG_DEBUG}" \
	LDFLAGS="${LDFLAGS} ${PROG_DEBUG}"

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

# Grep for commented-out code and TODO/XXX comments; lint the manual
# and the source code.  Requires shellcheck, mandoc, and clang.  You
# do not need to run this target.  The source code is released after
# it has been linted.
lint: ${SRCS}
	-@fgrep -e '	//' -e 'TODO' -e 'XXX' ${SRCS} || true
	-shellcheck ${SCRIPTS}
	-mandoc -T lint -W warning ${MAN} ctrlfnt.3 ctrlsel.3
	-clang-tidy ${SRCS} -- -std=c99 ${PROG_CPPFLAGS}

clean:
	rm -f ${OBJS} ${PROG} ${PROG:=.core} tags

.PHONY: all all-debug clean lint tags

.SUFFIXES: .xpm .abgr

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

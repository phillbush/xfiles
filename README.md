# XFiles

<p align="center">
  <img src="https://user-images.githubusercontent.com/63266536/210860567-1fc5893a-504b-4dff-9f7b-0dcf2376e367.png", title="demo"/>
</p>

This is a file manager for X11.  It can navigate through directories,
select files, call a script to open files, call a script to generate
thumbnails, and call a command to run on right mouse button click.

XFiles depends on a few environment variables to be usable:

* `OPENER`: Program to be called to open files.  Defaults to `xdg-open`.
* `XFILES_ICONS`: Newline-separated list of icon specifications (see below).
* `XFILES_THUMBNAILER`: Program to be called for generating thumbnails.
* `XFILES_THUMBNAILDIR`: Path to directory where thumbnails must be cached.
* `XFILES_CONTEXTCMD`: Program to be called when right clicking.

XFiles can be customized by setting the following X resources:

* `XFiles.faceName`:      Font for drawing text.
* `XFiles.background`:    Background color.
* `XFiles.foreground`:    Text color.
* `XFiles.selbackground`: Background color for selected entries.
* `XFiles.selforeground`: Text color for selected entries.


## Icons

Each line of the `$XFILES_ICONS` environment variable must have the
following format:

```
[TYPE:][GLOB1|GLOB2|...|GLOBN]=PATH
```

TYPE is a string of characters that must be in the file's type as
specified by the first column of `ls -l`.  For example, the TYPE "d"
matches a directory, and "-x" matches any regular executable file.

GLOB1 til GLOBN are bar-delimited glob strings that must match the entry
being displayed by XFiles, for example, "*.png|*.jpg" matches files with
the png or jpg extensions.

PATH is the path to the icon in .xpm format.

The following is how I set my `XFILES_ICONS`.  It sets an icon for the
directory up (dot-dot) entry, my $HOME, and for arbitrary directories;
and also icons for some file types and other files.

```
export XFILES_ICONS="
d:..=$ICONPATH/64x64/folder-up.xpm
d:$HOME=$ICONPATH/64x64/folder-home.xpm
d:*=$ICONPATH/64x64/folder.xpm
-:*.zip|*.tar.*|*.tar|*.gz|*.rar=$ICONPATH/64x64/file-archive.xpm
-:*.mp4|*.webm|*.mkv|*.mov=$ICONPATH/64x64/file-video.xpm
-:*.xpm|*.xbm|*.png|*.jpg|*.ppm|*.jpeg|*.gif=$ICONPATH/64x64/file-image.xpm
-:*.mp[23]|*.ogg|*.opus|*.flac=$ICONPATH/64x64/file-music.xpm
-:*.pdf|*.epub|*.txt|*.ps|*.eps|*.djvu=$ICONPATH/64x64/file-text.xpm
*=$ICONPATH/64x64/file.xpm
"
```


## Thumbnails

The thumbnailer program is called with two arguments: the original
file's path and the path to a ppm file to be generated.  The thumbnail
must be in the .ppm format and must be at most 64x64 in size.

XFiles comes with a thumbnailer script for generating thumbnails.  Such
script depends on the following programs:

* ImageMagick's convert(1): To generate thumbnail for images.
* ffmpegthumbnail(1): To generate thumbnail for videos.
* rsvg-convert(1): To generate thumbnail for svg files.
* pdftoppm(1): To generate thumbnail for pdf.
* ffmpeg(1): To generate thumbnail for audio files.


## Opening

The opener program is called with the highlighted file's path as
argument.  If not specified with the `OPENER` environment variable, it
defaults to xdg-open (I recommend you to use [plumb] instead).

[plumb]: https://github.com/phillbush/plumb


## Context menu

The context command is called with the paths to the selected files as
arguments.  If not specified with the `XFILES_CONTEXTCMD` environment
variable, nothing is used.  I recommend you to use [xmenu] or [pmenu].

[xmenu]: https://github.com/phillbush/xmenu
[pmenu]: https://github.com/phillbush/pmenu


## Questions

### How do I set custom icons?

You must set the `XFILES_ICONS` environment variable.
XFiles comes with a set of icons in `./icons`; you can use them,
or you can use any set of 64x64 `.xpm` icons.
(Yes, they must be 64x64 pixels wide and in the `.xpm` format).


### How can I open/edit xpm icons?

You can open xpm files with [feh](https://feh.finalrewind.org/), or 
your preferred image viewer if it supports it.

[Xpm](https://en.wikipedia.org/wiki/X_PixMap) is a file format meant
solely for icons, and is not intended to be used for general images
(especially colorful, vivid ones).  It has the feature that it can be
easily [edited in a text editor](https://upload.wikimedia.org/wikipedia/commons/b/b3/Screenshot-xterm-linux.xpm-GVIM.png).
The GIMP (GNU Image Manipulation Program) also supports exporting into
XPM files.


### Why there are no thumbnail/miniatures?

You must set the `XFILES_THUMBNAILER` and `XFILES_THUMBNAILDIR` environment variables.

### Why XPM for icons?

XPM is the default file format for icons in X11.  It is used for small
images with few colors, just like icons.  One can easily draw xpm icons
with a text editor and change the colors on a bunch of xpm files with a
simple shell script.

### Why PPM for thumbnails?

Thumbnails need to be quickly decoded and processed for a smooth file
browsing.  PPM is a raw image format that needs no decoding.  That
means, however, that there is no compression involved.  But thumbnails
are at most 64x64 pixels in size, so there's no need for compression as
the files are small.

### What is that thing that opens on middle click?

That's the scroller, it replaces the scrollbar.  It can be used either
as a scrollbar, by dragging the handle with the left mouse button; or as
Firefox's autoScroll feature, by moving the cursor up and down while
holding no button at all.

### How can I enable autoScroll on Firefox?

Set `general.autoScroll` to True on about:config.
(Bro tip: do it).

### Is there a context menu?

Not natively, You should use a menu application like xmenu or pmenu
for that.


### How can I copy/move/rename/delete files?

Use the context command for such operations.  For copying and moving, I
save the files into the clipboard with `xsel(1)` and then retrieve it
when pasting.  You can check my context script at my [dotfiles].

[dotfiles]: https://github.com/phillbush/home/blob/668c9929b724417671d95432e1eedc98b1d82cb2/execs/xfiles-menu


## License

The code and manual are under the MIT/X license.
See `./LICENSE` for more information.

The icons are in CC0/Public Domain.

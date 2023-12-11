# XFiles

<p align="center">
  <img src="./demo.png", title="demo"/>
</p>

XFiles is a file manager for X11.  It can navigate through directories,
show icons for files, select files, call a command to open files,
generate thumbnails, and call a command to run on right mouse button
click.

## Options
XFiles understand the following command-line options in addition to a
directory given as argument.

* `-a`:           List all files, including dotfiles.
* `-N name`:      Specify a resource/instance name for XFiles.
* `-X resources`: Specify X resources for XFiles.

## Environment
XFiles depends on a few environment variables to be usable:

* `OPENER`: Program to be called to open files.  Defaults to `xdg-open`.
* `CACHEDIR` or `XDG_CACHE_HOME`: Path to directory where thumbnails must be cached.

## Scripting
XFiles depends on a few scripts that the user must have in their
`$PATH`.  XFiles comes with examples for those scripts.

* `xfilesctl`:
  Script called when the user interacts with XFiles.  This script can
  handle menus, file dropping and key presses.  The example `xfilesctl`
  script that comes with XFiles uses `xmenu` for context menus and
  `dmenu` for the URL bar.  It also uses `xclip(1)` for dealing with the
  clipboard.

* `xfilesthumb`:
  Script called for generating thumbnails.  Thumbnails are 64x64 images
  in the PPM format containing miniatures for files.  The example script
  `xfilesthumb` script that comes with XFiles uses ImageMagick,
  ffmpegthumbnailer, librsvg and `pdftoppm` to generate thumbnails for
  different types of files.  For thumbnails to work, either the
  environment variable `CACHEDIR` or `XDG_CACHE_HOME` must be set to the
  path of an existing writeable and readable directory where a directory
  will be created for thumbnails to be cached.

## Customization
XFiles can be customized by setting the following X resources, either
before invoking XFiles, or while it is running (XFiles updates its theme
after a updating the X resources database with xrdb).

* `XFiles.faceName`:         Font for drawing text.
* `XFiles.faceSize`:         Font size.
* `XFiles.fileIcons`:        File glob patterns to icons associations.
* `XFiles.opacity`:          Background opacity (from 0.0 to 1.0).
* `XFiles.background`:       Background color.
* `XFiles.foreground`:       Text color.
* `XFiles.activeBackground`: Background color for selected entries.
* `XFiles.activeForeground`: Text color for selected entries.
* `XFiles.statusBarEnable`:  Whether to enable the statusbar.

## Non-features
XFiles does not come with any of the following features:

* Context menu:     Use `xmenu` or `pmenu` for that.
* URL bar:          Use `dmenu` for that.
* File operations:  Use `mv`, `cp` for that.
* File opening:     Use `xdg-open` for that.
* Clipboard copy:   Use `xclip` for that.

## Installation
Run `make all` to build.  Then install the binary file `./xfiles` somewhere
in your `$PATH`; and install the manual file `./xfiles.1` somewhere in your
`$MANPATH`.

## Usage

* Double left click on files open them.
* Dragging files perform drag-and-drop.
* Dragging empty space performs rectangular selection.
* Middle click opens the scroller (a kind of scrollbar + autoScroll).
* Right click invokes `xfilesctl`.

## License
The code and manual are under the MIT/X license.
The icons are in CC0/Public Domain.
See `./LICENSE` for more information.

## Epilogue
**Read the manual.**

# xfwm4-gl

This is xfwm4 with a second way to draw the composited screen, using
OpenGL 2.0. The original way, XRender, is still there as a fallback.

Upstream xfwm4 lives at https://gitlab.xfce.org/xfce/xfwm4. Everything here is
under the same licence, GPL v2 or later. See [README.md](README.md) for the
normal xfwm4 information.

**This is experimental. It builds cleanly but has not been run yet. Expect
visual glitches on the first try, and read the recovery note below before you
start.**

## Why this exists
---------------

Windows are handed to the graphics card as textures and drawn straight to the
screen. The old way builds the whole screen in an off screen image first and
then copies that image to the screen, so every frame is drawn twice.

Four things come out of that:

* One less full screen copy per frame.
* Only the parts of the screen that changed are redrawn.
* Window shadows no longer need the processor. The old code blurs a shadow on
  the processor every time a window changes size, which you can feel while
  dragging a window edge. The new code does it in a small shader.
* Vsync becomes a real choice. You can now composite on the graphics card with
  vsync off, or let it tear only when a frame is already late.

There is also a line in Window Manager Tweaks that tells you which of the two
is in use, because it can change by itself if your driver cannot cope.

What it does not change: XRender is not slow because it runs on the processor.
On most systems the X server already draws XRender on the graphics card. The
gain here is fewer steps, not moving work off the processor.

## Performance

[blah]

## Requirements
------------

[if libepoxy is also required in xfwm4, we shouldn't mention; just mention what's new requirement]
* libepoxy and the usual xfwm4 build dependencies
* OpenGL 2.0, frame buffer objects, and `GLX_EXT_texture_from_pixmap`

Anything missing and you simply get the old XRender path.

Building
--------

    % ./autogen.sh --enable-epoxy --enable-xpresent
    % make

Trying it without installing
----------------------------

Turn the new renderer on, then replace your running window manager:

    % xfconf-query -c xfwm4 -p /general/use_gl_compositing -n -t bool -s true
    % G_MESSAGES_DEBUG=all ./src/xfwm4 --replace

The setting has to be created by hand like this until the package is installed,
because your installed copy of `/usr/share/xfwm4/defaults` does not know about
it yet.

Keep that terminal open, warnings appear there. If something goes wrong, put
your old window manager back with:

    % /usr/bin/xfwm4 --replace

Settings
--------

`/general/use_gl_compositing` turns the OpenGL renderer on and off. There is a
checkbox for it in Window Manager Tweaks, under Compositor.

`/general/vblank_mode`, or `--vblank` on the command line:

| value | what happens |
| --- | --- |
| `auto` | sync every frame to the screen, the default |
| `glx` | same, chosen explicitly |
| `tear` | sync unless the frame is already late, so it tears only when needed |
| `off` | no sync at all, fastest, tears |

Checking which renderer is running
----------------------------------

Window Manager Tweaks shows it, and so does:

    % xprop -root _XFWM4_RENDER_BACKEND

You get `opengl` with the name of your card, or `xrender`, or nothing at all
when compositing is switched off.

When it falls back on its own
-----------------------------

The OpenGL path is skipped, quietly and without breaking your session, if:

* libepoxy was not there when it was built
* your driver reports a software renderer, such as llvmpipe
* OpenGL 2.0, frame buffer objects or texture from pixmap are missing
* a shader fails to build
* a frame fails while running, in which case it gives up for the rest of the
  session and says so in the log

Known gaps
----------

* Never executed. Shadows, the magnifier and windows on screens whose driver
  reports flipped textures are the most likely places to see something wrong.
* Shadows for windows smaller than the blur still use the old processor code.
  This is on purpose, the shortcut the shader takes does not hold for them.
* Shadows may differ from the old ones by a step or two of transparency.
* Only tested against one graphics driver family, and only at build level.

If you try it, please report what you see, with the terminal output.

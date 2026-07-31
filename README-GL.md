# xfwm4-gl

xfwm4-gl is a compositor forked from Xfce xfwm4 adding support to OpenGL
compositing. The original way, XRender, is still available as a fallback.

Upstream xfwm4 lives at https://gitlab.xfce.org/xfce/xfwm4. Everything here is
under the same licence, GPL v2 or later. See [README.md](README.md) for the
normal xfwm4 information.

## Why this exists

A compositor has to take the picture of every window and put those pictures
together into the screen you see. xfwm4 does that with XRender, an old drawing
interface of the X server. It works, but it forces a shape on the code: the
screen is built in an off screen image first, and that image is then copied to
the screen, so every frame is drawn twice. It also has to ask the X server, for
every window of every frame, what area that window covers, and wait for the
answer each time.

Windows are now handed to the graphics card as textures and drawn straight to
the screen instead.

Five things come out of that:

* One less full screen copy per frame.
* Only the parts of the screen that changed are redrawn.
* Nothing is asked of the X server per window any more. The areas windows cover
  are worked out here, and a window that is not shaped needs no question at all
  since its area is simply its rectangle.
* Window shadows no longer need the processor. The old code blurs a shadow on
  the processor every time a window changes size, which you can feel while
  dragging a window edge. The new code does it in a small shader.
* Vsync becomes a real choice. You can now composite on the graphics card with
  vsync off, or let it tear only when a frame is already late.

What it does not change: XRender is not slow because it runs on the processor.
On most systems the X server already draws XRender on the graphics card. The
gain here is fewer steps and less waiting, not moving work off the processor.

## Performance

There are no benchmark numbers here yet, and there will not be any until they
are measured properly. What can be said is what the machine no longer does per
frame: one full screen copy, one round trip to the X server for every window,
and a gaussian blur on the processor every time a window with a shadow changes
size.

If you want to measure it yourself, a fair comparison needs vsync out of the
way, otherwise every result is pinned to your refresh rate:

    % xfconf-query -c xfwm4 -p /general/vblank_mode -s off

Then run the same test twice, once with `use_gl_compositing` true and once
false, restarting the compositor in between, and watch the processor time of
the xfwm4 process while you drag and resize windows. Frame rates of full screen
games say little about either renderer, because compositing is suspended for
them anyway, see below.

## Requirements

Building needs nothing xfwm4 does not already need. libepoxy is already an
optional dependency upstream, where it is used for vsync; here it is also what
the OpenGL renderer is built on, so it is no longer optional in practice.

At runtime the graphics driver has to offer:

* OpenGL 2.0
* frame buffer objects
* `GLX_EXT_texture_from_pixmap`

Anything missing and you simply get the old XRender path.

## Usage

Nothing to do. OpenGL compositing is on by default, and if your driver cannot
do it you get XRender without noticing. Both are controlled from Window Manager
Tweaks, under Compositor.

Three settings there matter:

| setting | default | what it does |
| --- | --- | --- |
| Use OpenGL for compositing | on | picks the renderer, falls back on its own |
| Suspend compositing for focused fullscreen windows | on | gives fullscreen applications the whole graphics card |
| Display fullscreen overlay windows directly | on | for applications that ask to bypass the compositor |

The second one is worth knowing about if you play games: while a fullscreen
window has focus, compositing stops altogether and starts again as soon as that
window closes or loses focus. That is where the frame rate of a game comes from,
not from the choice of renderer.

## Building

Same as xfwm4 but now having `--enable-epoxy` and `--enable-xpresent` as
requirements:

    % ./autogen.sh --prefix=/usr --enable-epoxy --enable-xpresent
    % make

Do not leave `--prefix` out. Without it the binary looks for its data under
`/usr/local`, finds no `defaults` file and exits at startup, which is a poor
surprise when it happens to be your window manager. Note also that `make` does
not rebuild after a `configure` with a different prefix, so run `make clean`
first if you change it.

To try it without installing, replace the running window manager:

    % G_MESSAGES_DEBUG=all ./src/xfwm4 --replace

Keep that terminal open, warnings appear there. If something goes wrong, put
your installed window manager back the same way:

    % /usr/bin/xfwm4 --replace

## Settings

`/general/use_gl_compositing` turns the OpenGL renderer on and off. Changing it
restarts the compositor, so it takes effect immediately.

`/general/suspend_compositing_fullscreen` turns off compositing while a
fullscreen window has focus.

`/general/vblank_mode`, or `--vblank` on the command line. This one is read at
startup only:

| value | what happens |
| --- | --- |
| `auto` | sync every frame to the screen, the default |
| `glx` | same, chosen explicitly |
| `tear` | sync unless the frame is already late, so it tears only when needed |
| `off` | no sync at all, fastest, tears |

`tear` needs `GLX_EXT_swap_control_tear` from your driver. Without it you get
`auto` behaviour, and the dialog will say so rather than claim otherwise.

## Checking which renderer is running

Window Manager Tweaks shows it:

[screenshot here -- I'll do it]

Or

    % xprop -root _XFWM4_RENDER_BACKEND _XFWM4_VSYNC

The first gives `opengl` with the name of your card, or `xrender`, or nothing at
all when compositing is switched off. The second is `on`, `off` or `adaptive`,
and reports what actually happened rather than what was asked for.

## When it falls back on its own

The OpenGL path is skipped, quietly and without breaking your session, if:

* libepoxy was not there when it was built
* your driver reports a software renderer, such as llvmpipe
* OpenGL 2.0, frame buffer objects or texture from pixmap are missing
* no texture target works, either because no frame buffer config can bind
  windows of both 24 and 32 bit depth, or because the shader for it does not
  compile
* a frame fails while running, in which case it hands the windows back to
  XRender for the rest of the session and says so in the log

## Limitations

* Tested on AMD hardware with Mesa. Other drivers should work, and will fall
  back if they cannot, but nobody has tried them.
* Shadows of windows smaller than the blur still use the old processor code.
  This is on purpose, the shortcut the shader takes does not hold for them, and
  those shadows are cheap anyway.
* Shadows may differ from the XRender ones by a step or two of transparency.
* Windows of unusual colour depths, neither 24 nor 32 bit, are left to XRender
  window by window: they are not drawn by the GL path, and are not allowed to
  hide what is behind them either.

If something looks wrong, please report it with the output of the terminal you
started xfwm4 from.

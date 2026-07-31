# xfwm4-gl

xfwm4-gl is a compositor forked from Xfce
[`xfwm4`](https://gitlab.xfce.org/xfce/xfwm4), adding support for OpenGL
compositing. The original way, XRender, is still available as a fallback.

It also adds an option to switch the compositor off automatically while a
fullscreen application has focus, which improves performance and addresses one
of the weakest points of `xfwm4`, as seen in the latest
[Phoronix desktop benchmark](https://www.phoronix.com/review/cachyos-desktops-july-2026/2).

## Why this exists

A compositor has to take the picture of every window and put those pictures
together into the screen you see. `xfwm4` does that with XRender, an old drawing
interface of the X server, and that choice shapes the code: the screen is built
in an off screen image first, and that image is then copied to the screen, so
every frame is drawn twice. On top of that, the X server is asked what area each
window covers, for every window on every frame, and each answer has to be waited
for.

With `xfwm4-gl` windows are handed to the graphics card as textures and drawn
straight to the screen instead.

Four things come out of that:

* One less full screen copy per frame.
* Only the parts of the screen that changed are redrawn.
* The X server is asked once per frame for the area that changed, instead of
  once for every window on every frame.
* Window shadows no longer need the CPU.

Vsync changes too, allowing adaptive vsync, so a frame that is already late is
shown right away instead of waiting for the next refresh. See
[Settings](#settings).

## Usage

OpenGL compositing is on by default, and if the driver cannot do it, XRender is
used instead.

To check which renderer is currently in use, open Window Manager Tweaks and
select the Compositor tab:

[screenshot here -- I'll do it]

* **Use OpenGL for compositing (default on)** — enables the OpenGL renderer, or
  falls back to XRender.
* **Suspend compositing for focused fullscreen windows (default on)** —
  temporarily disables compositing while a fullscreen application has focus.
  This is especially useful for heavy applications such as games. While
  compositing is off, avoiding tearing is up to the application: if it uses
  OpenGL or Vulkan this should not be a problem, otherwise it may tear. An
  application can also ask never to be bypassed, and then compositing is left
  running for it, which is what `mpv --x11-bypass-compositor=never` does.
* **Display fullscreen overlay windows directly (default on)** — also present
  in the original `xfwm4`, and here it only gained a tooltip. It covers
  fullscreen windows that bypass the window manager without saying anything
  about compositing, which in practice means quite old games and players. An
  application that explicitly asks to bypass the compositor is let through
  whether this option is on or off.

## Requirements

No extra dependency was added for building. `xfwm4` already builds against
`libepoxy` for vsync, and `xfwm4-gl` uses it for the OpenGL renderer as well, so
check that the configure summary says `Epoxy support: yes`.

At runtime a graphics driver with OpenGL 2.0 or newer is enough. That is every
driver of the last fifteen years or so, and anything older simply gets XRender.

## When it falls back to XRender

The OpenGL path is skipped, quietly and without breaking your session, if:

* `libepoxy` was missing when it was built
* the driver reports a software renderer, such as `llvmpipe`
* the driver is older than OpenGL 2.0
* the driver cannot hand windows over to OpenGL as textures
* the graphics context is lost while running, after a driver reset for
  instance. The windows are handed back to XRender for the rest of the session,
  and the reason is written to the log.

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
| `tear` | sync unless the frame is already late, so it tears only when needed |
| `off` | no sync at all, fastest, tears |

`tear` needs `GLX_EXT_swap_control_tear` from the driver. Without it you get
`auto` behaviour, and Window Manager Tweaks says so rather than claim otherwise.

Two more values exist, `glx` and `xpresent`, which force how XRender puts its
image on the screen. They do nothing while the OpenGL renderer is in use and are
only of interest when chasing a driver problem.

## Limitations

* On an unusual screen depth, ten bit colour for instance, a window may not be
  drawn at all and you will see straight through it. Turning OpenGL compositing
  off is the way out.

## License

Same as `xfwm4`: GNU General Public License, version 2 or later. See
[COPYING](COPYING) for the full text.

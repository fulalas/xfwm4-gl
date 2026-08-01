# xfwm4-gl

xfwm4-gl is a compositor forked from Xfce
[xfwm4](https://gitlab.xfce.org/xfce/xfwm4), adding support for OpenGL
compositing. The original way, XRender, is still available as a fallback.

It also adds an option to switch the compositor off automatically while a
fullscreen application has focus, which improves performance and addresses one
of the weakest points of `xfwm4`, as seen in the latest
[Phoronix desktop benchmark](https://www.phoronix.com/review/cachyos-desktops-july-2026/2).

## Why this exists

A compositor has to take the picture of every window and put those pictures
together into the screen. `xfwm4` does that with XRender, an old drawing
interface of the X server, and that choice shapes the code: the screen is built
in an off-screen image first, and that image is then copied to the screen, so
every frame is drawn twice. On top of that, the X server is asked what area a
window covers, once for every window on every frame, and each answer has to be
waited for.

With `xfwm4-gl`, windows are handed to the graphics card as textures and drawn
straight to the screen instead.

Four things come out of that:

* One less copy of the whole screen per frame.
* Only the parts of the screen that changed are redrawn.
* The X server is asked once per frame for the area that changed, instead of
  once for every window on every frame.
* Window shadows are drawn by the graphics card instead of the CPU. Very small
  windows, such as tooltips, are the exception.

Adaptive vsync can now be picked, rather than only happening when the driver
supports it, and it works with both renderers. See [Settings](#settings).

## Usage

All files respect the same names and paths as the original, so once `xfwm4-gl`
is installed the session should load it automatically.

It is also possible to try it without installing, by replacing the window
manager that is already running:

    ./path_to_new_build/xfwm4 --replace

## Features

OpenGL compositing is on by default, and if the driver cannot do it, XRender is
used instead.

To check which renderer is currently in use, open Window Manager Tweaks and
select the Compositor tab:

<img src="https://github.com/user-attachments/assets/65d1a24a-5ed6-4682-97f4-a0bc43e528cb" />

* **Use OpenGL for compositing (default on)** — enables the OpenGL renderer;
  XRender is used instead if it cannot start.
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

## When it falls back to XRender

The OpenGL path is skipped, quietly and without breaking your session, if:

* `libepoxy` was missing when it was built
* the driver is older than OpenGL 2.0, is missing frame buffer objects, or
  cannot hand windows over to OpenGL as textures
* the driver reports a software renderer such as `llvmpipe`, or a virtual GPU
  such as VMware `SVGA3D` or virtio `virgl`
* a window turns up with a colour depth the driver cannot hand over, in which
  case the whole screen goes back to XRender for the rest of the session
* the graphics context is lost while running, after a driver reset for instance,
  in which case the windows are handed back to XRender for the rest of the
  session

## Settings

`/general/use_gl_compositing` turns the OpenGL renderer on and off. Changing it
restarts the compositor, so it takes effect immediately.

`/general/suspend_compositing_fullscreen` turns off compositing while a
fullscreen window has focus.

`/general/vblank_mode`, or `--vblank` on the command line, is read at startup
only:

| value | description |
| --- | --- |
| `auto` | (default) sync every frame to the screen, if the driver offers swap control at all |
| `tear` | (new) sync unless the frame is already late, so it tears only when needed (requires `GLX_EXT_swap_control_tear`, otherwise falls back to `auto`) |
| `off` | no sync at all, fastest, tears |

Two more values exist: `glx` and `xpresent`. While the OpenGL renderer is
running it presents its own frames and never goes through XPresent, so both
behave like `auto`. They differ only once the compositor has fallen back to
XRender, where they pick which of the two ways of presenting is used.

## Requirements

No extra dependency was added for building. `xfwm4` already builds against
`libepoxy` for vsync, and `xfwm4-gl` uses it for the OpenGL renderer as well.
It is optional upstream, so nothing complains when it is missing: check that the
configure summary says `Epoxy support: yes`.

At runtime the driver needs OpenGL 2.0 or newer, frame buffer objects, and
`GLX_EXT_texture_from_pixmap`. Every driver of the last fifteen years or so has
all three.

Both VirtualBox and QEMU work as long as they have 3D acceleration turned on.

## License

Same as `xfwm4`: GNU General Public License, version 2 or later. See
[COPYING](COPYING) for the full text.

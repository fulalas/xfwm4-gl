# xfwm4-gl

xfwm4-gl is a compositor forked from Xfce (https://gitlab.xfce.org/xfce/xfwm4)[xfwm4] adding support to OpenGL
compositing. The original way, XRender, is still available as a fallback.

A new option is added for automatically disabling the compositor when an application
in fullscreen is focused, improving performance and fixing one of the weakest points
of `xfwm4`, as seen in last Phoronix desktop (https://www.phoronix.com/review/cachyos-desktops-july-2026/2)[environment benchmark].

## Why this exists

A compositor has to take the picture of every window and put those pictures
together into the screen you see. `xfwm4` does that with XRender, an old drawing
interface of the X server that works by forcing a shape on the code: the
screen is built in an off screen image first, and that image is then copied to
the screen, so every frame is drawn twice. It also has to ask the X server, for
every window of every frame, what area that window covers, and wait for the
answer each time.

With `xfwm4-gl` windows are now handed to the graphics card as textures and drawn
straight to the screen instead.

Five things come out of that:

* One less full screen copy per frame.
* Only the parts of the screen that changed are redrawn.
* Nothing is asked of the X server per window any more.
* Window shadows no longer need the CPU.
* Vsync becomes a real choice. You can now composite on the graphics card with
  vsync off, or let it tear only when a frame is already late.

The gain here is fewer steps and less waiting, not moving work off the CPU.

## Usage

OpenGL compositing is on by default, and if the driver cannot
do it, the XRender is used instead.

To check which render is currently in use, open Window Manager
Tweaks and select the Compositor tab:

[screenshot here -- I'll do it]

- **Use OpenGL for compositing (default on)** — enables OpenGL render or fallback to XRender.
- **Suspend compositing for focused fullscreen windows (default on)** — temporarily disables compositing if an application in fullscreen is focused. This is especially useful while running heavy applications such as games.
- **Display fullscreen overlay windows directly (default on)** — this is also present in the original `xfwm4` and here only received a tooltip to explain this option usually applies to really old applications.

## When it falls back to XRender

The OpenGL path is skipped, quietly and without breaking your session, if:

* `libepoxy` was not there when it was built
* your driver reports a software renderer, such as `llvmpipe`
* OpenGL 2.0, frame buffer objects or texture from pixmap are missing
* no texture target works, either because no frame buffer config can bind
  windows of both 24 and 32 bit depth, or because the shader for it does not
  compile
* a frame fails while running [how?]

## Requirements

No extra dependency was added for building. `--enable-epoxy` is already
a `xfwm4` dependency to have vsync support, and with `xfwm4-gl` this is
required for the OpenGL renderer.

At runtime the graphics driver has to offer:

* OpenGL 2.0
* frame buffer objects
* `GLX_EXT_texture_from_pixmap`

Anything missing and you simply get the old XRender path.

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

## Limitations

* Shadows of windows smaller than the blur still use the old CPU code. This is
  on purpose, the shortcut the shader takes does not hold for them, and those
  shadows are cheap anyway.
* A window whose colour depth the driver cannot bind as a texture is not drawn,
  and you see what is behind it. Depths are looked up as windows turn up, so
  this only happens on a driver that cannot bind that depth at all.

## License

Same as `xfwm4` [explain].
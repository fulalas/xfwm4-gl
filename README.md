# xfwm4-gl

`xfwm4-gl` is a fork of Xfce [xfwm4](https://gitlab.xfce.org/xfce/xfwm4) that
adds OpenGL compositing. XRender, the original way, stays as a fallback.

There is also a new option, on by default, that turns compositing off while a
fullscreen application has focus. It is worth about 3% more frames for
applications that do not ask for that themselves.

Compared to XRender it uses **about a third less power** for the same work and
provides **about 10% more frames per second** to applications. The gap grows
with load, up to **25%** with eight windows drawing flat out.

<table>
<tr><th>Driver</th><th>Renderer</th><th>FPS</th><th>CPU (ms/s)</th><th>CPU (W)</th><th>GPU (W)</th></tr>
<tr><td rowspan="3">AMD (Mesa radeonsi)</td><td>no compositor</td><td>180.5</td><td>-</td><td colspan="2" align="center">18.9</td></tr>
<tr><td>XRender</td><td>161.6</td><td>11.2</td><td colspan="2" align="center">21.0</td></tr>
<tr><td>OpenGL</td><td>174.2</td><td>13.2</td><td colspan="2" align="center">20.2</td></tr>
<tr><td rowspan="3">NVIDIA proprietary</td><td>no compositor</td><td>166.3</td><td>-</td><td>4.0</td><td>37.2</td></tr>
<tr><td>XRender</td><td>160.9</td><td>17.5</td><td>5.8</td><td>43.0</td></tr>
<tr><td>OpenGL</td><td>158.6</td><td>16.2</td><td>4.7</td><td>43.2</td></tr>
<tr><td rowspan="3">NVIDIA Mesa (zink)</td><td>no compositor</td><td>149.6</td><td>-</td><td>6.8</td><td>N/A</td></tr>
<tr><td>XRender</td><td>149.1</td><td>23.5</td><td>14.8</td><td>N/A</td></tr>
<tr><td>OpenGL</td><td>149.1</td><td>22.5</td><td>14.8</td><td>N/A</td></tr>
</table>

FPS is measured in a windowed benchmark running as fast as it can; CPU and
power are measured while the same benchmark is locked at 60 fps. The AMD
machine has the graphics inside the processor, so its one power figure covers
both.

## Features

OpenGL compositing is on by default; if the driver cannot do it, XRender is
used instead.

OpenGL talks to the driver through EGL by default, because it was the cheapest
on the processor on every driver tested. `XFWM4_GL_BACKEND=glx` switches to
GLX, and if EGL cannot start, GLX takes over on its own.

Only the parts of the screen that changed are painted, then the whole buffer
is swapped, which the display hardware does for free. `XFWM4_GL_PRESENT` set
to `copy` or `fbo` picks other ways of getting the frame on screen; they are
for drivers that behave differently, not for everyday use.

Adaptive vsync can be picked now, with either renderer: frames wait for the
screen while they can keep up, and stop waiting when they cannot, so a slow
moment costs no extra lag. See [Settings](#settings).

To check which renderer is in use, open Window Manager Tweaks, Compositor tab:

<img src="https://github.com/user-attachments/assets/1f2c71c2-1027-4006-b626-6dea17beae0f" />

* **Use OpenGL for compositing (default on)** — enables the OpenGL renderer;
  XRender is used if it cannot start.
* **Suspend compositing for focused fullscreen windows (default on)** — turns
  compositing off while a fullscreen application has focus. Useful for games.
  While it is off, avoiding tearing is up to the application: fine for OpenGL
  and Vulkan, others may tear. An application can ask never to be bypassed,
  which is what `mpv --x11-bypass-compositor=never` does.
* **Display fullscreen overlay windows directly (default on)** — same as the
  original `xfwm4`; it only gained a tooltip. It covers quite old games and
  players that bypass the window manager without saying anything.

## Usage

All files keep the same names and paths as the original, so once installed the
session loads it automatically.

To try it without installing, replace the running window manager:

    ./path_to_new_build/src/xfwm4 --replace

## Settings

`/general/use_gl_compositing` turns the OpenGL renderer on and off. It takes
effect immediately.

`/general/suspend_compositing_fullscreen` turns compositing off while a
fullscreen window has focus.

`/general/vblank_mode`, or `--vblank` on the command line, read at startup
only:

| value | description |
| --- | --- |
| `auto` | (default) sync every frame to the screen |
| `adaptive` | (new) turns vsync off when the frame rate falls below the refresh rate, avoiding stutter and input lag (needs `GLX_EXT_swap_control_tear`, otherwise same as `auto`) |
| `off` | no sync, fastest, tears |

Two more values exist, `glx` and `xpresent`, but with the OpenGL renderer both
behave like `auto`. They only differ after falling back to XRender.

## When it falls back to XRender

The OpenGL path is skipped, quietly and without breaking the session, if:

* `libepoxy` was missing at build time
* the driver is a software renderer such as `llvmpipe` or `swrast`
* the driver is older than OpenGL 2.0, has no frame buffer objects, or cannot
  hand windows over as textures
* the graphics context is lost while running, after a driver reset for
  instance
* a colour depth the driver cannot hand over

## Other changes to xfwm4

Everything this fork changes outside the OpenGL renderer, compared with
[xfwm4](https://gitlab.xfce.org/xfce/xfwm4/) 4.20.0.

New features:

* two new boxes in Window Manager Tweaks, under Compositor: one turns the
  OpenGL renderer on and off, one turns off the fullscreen behaviour above.
  The dialog also says which renderer is running
* compositing switches itself off while a fullscreen window has focus, so games
  and video get the screen to themselves
* `--vblank` parameter now supports the `adaptive` value
* buttons in the title bar can light up under the pointer on a window that is
  not focused, from a new `inactive-prelight` image in the theme
* any tool can read the current renderer and vsync mode by running
  `xprop -root _XFWM4_RENDER_BACKEND _XFWM4_VSYNC`

Fixes:

* a window keeps its border while it is resized, instead of losing an edge for
  a moment on every step
* a window that is resized and moved at the same time, by dragging a corner,
  no longer appears to jump and come back
* a shrinking window leaves no stale strip behind along the edge it gave up
* title bar buttons are redrawn only when they really change
* the whole screen is drawn once when compositing starts, so nothing stale is
  left on it, and switching compositing off no longer risks a crash

## Requirements

No new build dependency. `xfwm4` already builds against `libepoxy` for vsync
and `xfwm4-gl` uses it for the OpenGL renderer too. It is optional upstream,
so check that the configure summary says `Epoxy support: yes`.

At runtime the driver needs OpenGL 2.0 or newer, frame buffer objects, and the
ability to hand windows over as textures. Every driver of the last 15 years or
so has all three.

VirtualBox and QEMU can use the OpenGL renderer as long as they have their
3D acceleration on.

## License

Same as `xfwm4`: GNU General Public License, version 2 or later. See
[COPYING](COPYING) for the full text.

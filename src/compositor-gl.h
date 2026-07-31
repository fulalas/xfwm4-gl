/*      $Id$

        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2, or (at your option)
        any later version.

        This program is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with this program; if not, write to the Free Software
        Foundation, Inc., Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

        xfwm4 - (c) 2002-2011 Olivier Fourdan

        OpenGL rendering backend for the compositor.

 */

#ifndef INC_COMPOSITOR_GL_H
#define INC_COMPOSITOR_GL_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_COMPOSITOR

#include "screen.h"
#include "compositor-priv.h"

#ifdef HAVE_EPOXY

/*
 * Set up the GL renderer on a screen whose GLX context is already current.
 * Returns FALSE if the driver lacks anything required, in which case the
 * caller must keep using the XRender path.
 */
gboolean         xfwmGLScreenInit               (ScreenInfo *);
void             xfwmGLScreenFinish             (ScreenInfo *);

/*
 * Paint the whole screen from the window textures. Returns FALSE when the
 * frame could not be painted, the caller then falls back to XRender.
 */
gboolean         xfwmGLPaintAll                 (ScreenInfo *,
                                                 XserverRegion);

void             xfwmGLScreenSizeChanged        (ScreenInfo *);
void             xfwmGLFreeWindowData           (CWindow *);
void             xfwmGLFreeWindowShadow         (CWindow *);
void             xfwmGLInvalidateRootTexture    (ScreenInfo *);
gboolean         xfwmGLUpdateWindowShadow       (CWindow *,
                                                 gdouble,
                                                 gint,
                                                 gint);

#endif /* HAVE_EPOXY */

#endif /* HAVE_COMPOSITOR */

#endif /* INC_COMPOSITOR_GL_H */

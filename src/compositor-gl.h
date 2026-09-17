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

gboolean         xfwmGLScreenInit               (ScreenInfo *);
void             xfwmGLScreenFinish             (ScreenInfo *);

gboolean         xfwmGLPaintAll                 (ScreenInfo *,
                                                 XserverRegion);

gboolean         xfwmGLTakeRetryPaint           (ScreenInfo *);

void             xfwmGLNoteFenceWait            (ScreenInfo *);

void             xfwmGLScreenSizeChanged        (ScreenInfo *);
void             xfwmGLFreeWindowData           (CWindow *);
void             xfwmGLInvalidateWindowRegions  (CWindow *);
void             xfwmGLTranslateWindowRegions   (CWindow *,
                                                 gint,
                                                 gint);
void             xfwmGLSetOpaqueRects           (CWindow *,
                                                 XRectangle *,
                                                 gint);
void             xfwmGLFreeWindowShadow         (CWindow *);
void             xfwmGLInvalidateRootTexture    (ScreenInfo *);
gboolean         xfwmGLRootPixmapCoversScreen   (ScreenInfo *);
gboolean         xfwmGLDamageRootPixmap         (ScreenInfo *,
                                                 Drawable);
const gchar     *xfwmGLGetRendererName          (ScreenInfo *);
void             xfwmGLUpdateWindowShadow       (CWindow *,
                                                 gdouble,
                                                 gint,
                                                 gint);

#endif /* HAVE_EPOXY */

#endif /* HAVE_COMPOSITOR */

#endif

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

        oroborus - (c) 2001 Ken Lynch
        xfwm4    - (c) 2002-2011 Olivier Fourdan

        Definitions shared between the compositor and its rendering backends.

 */

#ifndef INC_COMPOSITOR_PRIV_H
#define INC_COMPOSITOR_PRIV_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_COMPOSITOR

#include <X11/Xlib.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <glib.h>
#include <cairo.h>

#include "display.h"
#include "screen.h"
#include "client.h"

#ifndef SHADOW_RADIUS
#define SHADOW_RADIUS   12
#endif /* SHADOW_RADIUS */

#ifndef SHADOW_OFFSET_X
#define SHADOW_OFFSET_X (-3 * SHADOW_RADIUS / 2)
#endif /* SHADOW_OFFSET_X */

#ifndef SHADOW_OFFSET_Y
#define SHADOW_OFFSET_Y (-3 * SHADOW_RADIUS / 2)
#endif /* SHADOW_OFFSET_Y */

#ifndef TIMEOUT_REPAINT_PRIORITY
#define TIMEOUT_REPAINT_PRIORITY   G_PRIORITY_DEFAULT
#endif /* TIMEOUT_REPAINT_PRIORITY */

#ifndef TIMEOUT_THROTTLED_REPAINT_PRIORITY
#define TIMEOUT_THROTTLED_REPAINT_PRIORITY   G_PRIORITY_LOW
#endif /* TIMEOUT_THROTTLED_REPAINT_PRIORITY */

#ifndef TIMEOUT_REPAINT_MS
#define TIMEOUT_REPAINT_MS   1
#endif /* TIMEOUT_REPAINT_MS */

#ifndef TIMEOUT_THROTTLED_REPAINT_MS
#define TIMEOUT_THROTTLED_REPAINT_MS   500
#endif /* TIMEOUT_THROTTLED_REPAINT_MS */

#ifndef MONITOR_ROOT_PIXMAP
#define MONITOR_ROOT_PIXMAP   1
#endif /* MONITOR_ROOT_PIXMAP */

/* Some convenient macros */
#define WIN_HAS_CLIENT(cw)              (cw->c)
#define WIN_HAS_FRAME(cw)               (WIN_HAS_CLIENT(cw) && CLIENT_HAS_FRAME(cw->c))
#define WIN_NO_SHADOW(cw)               ((cw->c) && \
                                           (FLAG_TEST (cw->c->flags, CLIENT_FLAG_FULLSCREEN | CLIENT_FLAG_BELOW) || \
                                            (cw->c->type & WINDOW_DESKTOP)))
#define WIN_IS_DOCK(cw)                 (WIN_HAS_CLIENT(cw) && (cw->c->type & WINDOW_DOCK))
#define WIN_IS_OVERRIDE(cw)             (cw->attr.override_redirect)
#define WIN_IS_ARGB(cw)                 (cw->argb)
#define WIN_IS_OPAQUE(cw)               ((cw->opacity == NET_WM_OPAQUE) && !WIN_IS_ARGB(cw))
#define WIN_IS_NATIVE_OPAQUE(cw)        ((cw->native_opacity) && !WIN_IS_ARGB(cw))
#define WIN_IS_FULLSCREEN(cw)           ((cw->attr.x <= 0) && \
                                           (cw->attr.y <= 0) && \
                                           (cw->attr.width + 2 * cw->attr.border_width >= cw->screen_info->width) && \
                                           (cw->attr.height + 2 * cw->attr.border_width >= cw->screen_info->height))
#define WIN_IS_SHAPED(cw)               ((WIN_HAS_CLIENT(cw) && FLAG_TEST (cw->c->flags, CLIENT_FLAG_HAS_SHAPE)) || \
                                           (WIN_IS_OVERRIDE(cw) && (cw->shaped)))
#define WIN_IS_MAXIMIZED(cw)            (WIN_HAS_CLIENT(cw) && FLAG_TEST_ALL (cw->c->flags, CLIENT_FLAG_MAXIMIZED))
#define WIN_IS_VIEWABLE(cw)             (cw->viewable)
#define WIN_HAS_DAMAGE(cw)              (cw->damage)
#define WIN_IS_VISIBLE(cw)              (WIN_IS_VIEWABLE(cw) && WIN_HAS_DAMAGE(cw))
#define WIN_IS_DAMAGED(cw)              (cw->damaged)
#define WIN_IS_REDIRECTED(cw)           (cw->redirected)
#define WIN_IS_SHADED(cw)               (WIN_HAS_CLIENT(cw) && FLAG_TEST (cw->c->flags, CLIENT_FLAG_SHADED))

typedef struct _CWindow CWindow;
struct _CWindow
{
    ScreenInfo *screen_info;
    Client *c;
    Window id;
    XWindowAttributes attr;

    gboolean damaged;
    gboolean viewable;
    gboolean shaped;
    gboolean redirected;
    gboolean fulloverlay;
    gboolean argb;
    gboolean skipped;
    gboolean native_opacity;
    gboolean opacity_locked;

    Damage damage;
#if HAVE_NAME_WINDOW_PIXMAP
    Pixmap name_window_pixmap;
#endif /* HAVE_NAME_WINDOW_PIXMAP */
    Picture picture;
    Picture saved_picture;
    Picture shadow;
    Picture alphaPict;
    Picture alphaBorderPict;

    XserverRegion borderSize;
    XserverRegion clientSize;
    XserverRegion borderClip;
    XserverRegion extents;
    XserverRegion opaque_region;

    gint shadow_dx;
    gint shadow_dy;
    gint shadow_width;
    gint shadow_height;

    guint32 opacity;
    guint32 bypass_compositor;

#ifdef HAVE_EPOXY
    /* GL backend per window data, see compositor-gl.c */
    GLXPixmap gl_pixmap;
    GLuint gl_texture;
    GLuint gl_shadow_texture;
    gfloat gl_shadow_opacity;
    gboolean gl_texture_bound;
    gboolean gl_content_dirty;
    /*
     * Client side regions, so the paint loop never has to ask the X server
     * what a window covers. Cached until the geometry, the shape or the
     * opaque region changes, see xfwmGLInvalidateWindowRegions().
     */
    cairo_region_t *gl_shape;
    cairo_region_t *gl_opaque;
    cairo_region_t *gl_paint_clip;
#endif /* HAVE_EPOXY */
};

/*
 * Helpers of compositor.c shared with the rendering backends.
 */
XImage          *make_shadow                    (ScreenInfo *,
                                                 gdouble,
                                                 gint,
                                                 gint);
void             shadow_size                    (ScreenInfo *,
                                                 gint,
                                                 gint,
                                                 gint *,
                                                 gint *);
Pixmap           root_background_pixmap         (ScreenInfo *);
void             ensure_win_shadow              (CWindow *);
XserverRegion    win_extents                    (CWindow *);
XserverRegion    border_size                    (CWindow *);
XserverRegion    client_size                    (CWindow *);
void             clip_opaque_region             (CWindow *,
                                                 XserverRegion);

#endif /* HAVE_COMPOSITOR */

#endif /* INC_COMPOSITOR_PRIV_H */

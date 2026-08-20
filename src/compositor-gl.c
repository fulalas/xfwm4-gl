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

        Windows are bound as textures with GLX_EXT_texture_from_pixmap and
        composited by the GPU straight into the back buffer of the overlay,
        which removes the intermediate root buffer the XRender path needs.

        The visible region maths are the same as the XRender path: a top down
        pass collects the opaque areas and shrinks the region left to paint,
        then a bottom up pass draws shadows and translucent windows.

 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_COMPOSITOR

#include <math.h>

#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

#include <glib.h>

#include "display.h"
#include "screen.h"
#include "client.h"
#include "frame.h"
#include "hints.h"
#include "compositor-priv.h"
#include "compositor-gl.h"

#ifdef HAVE_EPOXY

#include <epoxy/egl.h>

#define GL_DAMAGE_HISTORY       8
/* Well past the handful of depths an X server can advertise, so it cannot fill */
#define GL_MAX_DEPTHS           32
#define GL_MAX_ROOT_TILES       256
/*
 * A pixmap the driver refused once may well bind on the next frame, a refusal
 * for want of video memory being the one that goes away. After this many frames
 * in a row it is not going to, and XRender takes the screen over.
 */
#define GL_MAX_BIND_RETRIES     3

/*
 * Binding a pixmap as a texture needs a frame buffer config matching the depth
 * of that pixmap. Windows are nearly always 24 or 32 bit, but a screen can run
 * at another depth, ten bit colour for instance, so the configs are looked up
 * per depth as windows turn up and kept here.
 */
typedef struct
{
    gint depth;
    GLXFBConfig fbconfig;
    gboolean usable;
} XfwmGLDepth;

/*
 * How a finished frame reaches the screen. SWAP paints only the damage and
 * swaps the whole screen. COPY paints the whole screen and copies only the
 * damage, because a copy leaves the back buffer undefined. FBO paints only the
 * damage into a texture that is guaranteed to survive, then copies only the
 * damage out of it, so it saves on both sides. FBO is the normal GLX choice
 * when the driver has the copy operation; SWAP remains the safe fallback.
 *
 * SCENE is FBO's way of drawing with SWAP's way of presenting, for drivers
 * with no copy operation, the EGL backend among them. The scene is kept in a
 * texture and only what changed is composited into it; the buffer that is
 * about to be swapped in is a few frames old, so what it is missing is blitted
 * out of that texture, which is far less work than compositing it again.
 */
typedef enum
{
    GL_PRESENT_SWAP,
    GL_PRESENT_COPY,
    GL_PRESENT_FBO,
    GL_PRESENT_SCENE
} XfwmGLPresentMode;

/* More rectangles than this are presented as their bounding box instead */
#define GL_MAX_PRESENT_RECTS    32

/*
 * Making an image out of a pixmap is allowed to throw the pixmap's content
 * away unless it is asked to keep it: the default of EGL_IMAGE_PRESERVED_KHR
 * is false. Without this a window comes up holding nothing until it draws
 * itself again, which is exactly the black first frame wait_for_pixmap() was
 * written for.
 */
static const EGLint preserved_image[] = {
    EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
    EGL_NONE
};

/* Timer queries in flight for XFWM4_GL_PROFILE, enough to never wait on one */
#define GL_PROF_QUERIES         4

/* Buffer age histogram buckets for XFWM4_GL_PROFILE, the last collects the rest */
#define GL_PROF_AGE_BUCKETS     12

typedef struct
{
    GLuint program_win;
    GLuint program_2d;
    GLuint program_shadow_profile;
    GLint u_opacity_win;
    GLint u_opacity_2d;
    GLint u_prof_size;
    GLint u_prof_ramp;
    GLint u_prof_opacity;

    GLuint shadow_profile;
    gfloat shadow_profile_peak;

    GLenum tex_type;
    GLenum tex_target;

    XfwmGLDepth depths[GL_MAX_DEPTHS];
    guint n_depths;

    gboolean has_buffer_age;
    XfwmGLPresentMode present_mode;

    /*
     * XFWM4_GL_FENCE=off: do not fence the frame. The repaint loop waits for
     * the fence of the last frame before starting the next, and while it is
     * unsignalled it retries on a 1 ms timer, so a frame the GPU is slow to
     * finish costs a burst of wakeups that each ask the driver again.
     */
    gboolean use_fence;

    /*
     * XFWM4_GL_NOPAINT=1: a diagnostic. Everything a frame does except putting
     * pixels anywhere: the damage is fetched, the regions are worked out, the
     * textures are bound and the frame is presented, but no quad is drawn. The
     * screen is garbage, so this is for measuring only. What the application
     * then reaches is the ceiling the compositor could have if its drawing were
     * free, which bounds what is left to win.
     */
    gboolean no_paint;

    /*
     * XFWM4_GL_PIXMAP_WAIT=off: do not read a pixel back from a window's pixmap
     * after binding it. That read is a blocking round trip for every window that
     * appears and it costs about 2% on a workload of menus opening and closing.
     * It is on by default because the comment on wait_for_pixmap() records a
     * measured defect without it, and because no test here can show that defect
     * is gone: resize_check, with teeth proven by injecting a black frame, sees
     * nothing either way over hundreds of resizes. Absence of evidence, so the
     * default does not move.
     */
    gboolean wait_new_pixmap;

    /*
     * XFWM4_GL_BACKEND=egl: the same renderer on an EGL context instead of a
     * GLX one. Windows are sampled through EGL images rather than
     * GLX_EXT_texture_from_pixmap, and frames are presented with
     * eglSwapBuffersWithDamage, which passes the damage to the driver and
     * leaves the synchronisation to it. Present modes other than swap are
     * GLX experiments and do not apply here. Whether this backend is in use
     * is screen_info->use_egl_backend, decided once in setup_gl().
     */
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig egl_config;
    /* The KHR or EXT entry point, whichever the driver has, or NULL */
    EGLBoolean (*egl_swap_with_damage) (EGLDisplay, EGLSurface,
                                        EGLint *, EGLint);
    gpointer root_egl_image;

    /* XFWM4_GL_STATS: paints a second and pixels presented, printed every 5 s */
    gboolean stats;
    guint stat_frames;
    gdouble stat_pixels;
    /* What actually changed, against what had to be painted for it */
    gdouble stat_damage_pixels;
    gdouble stat_paint_pixels;
    /*
     * The frames the buffer age was read for. Not the same as the frames that
     * reached the screen: a frame can be dropped after the age was read, and
     * dividing the age numbers by the presented frames made percentages that
     * could pass 100.
     */
    gdouble prof_age_frames;
    gint64 stat_since;

    /*
     * XFWM4_GL_PROFILE: where the processor time of a paint goes, printed
     * every 5 s. Measured on this thread's own clock, not the wall clock, so
     * the driver's command submission thread cannot be counted as ours; the
     * wall clock figure is kept alongside so the difference is visible.
     */
    gboolean profile;
    gboolean prof_gpu;
    gdouble prof_damage;
    gdouble prof_pass1;
    gdouble prof_root;
    gdouble prof_pass2;
    gdouble prof_present;
    gdouble prof_wall;
    gdouble prof_quads;
    gdouble prof_rects;
    gdouble prof_windows;
    gdouble prof_shadows;
    gdouble prof_bind;
    gdouble prof_binds;
    gdouble prof_fence_waits;
    gdouble prof_age_over;
    gdouble prof_full_repaint;
    guint prof_age_max;
    gdouble prof_age_hist[GL_PROF_AGE_BUCKETS];

    /*
     * How long the graphics card spends on our drawing, as opposed to how
     * long we spend asking it to. A ring of queries so that reading one never
     * waits for the frame that is still being drawn.
     */
    GLuint prof_query[GL_PROF_QUERIES];
    gboolean prof_query_busy[GL_PROF_QUERIES];
    guint prof_query_next;
    gint prof_query_active;
    gdouble prof_gpu_ns;
    gdouble prof_gpu_frames;

    cairo_region_t *damage_history[GL_DAMAGE_HISTORY];
    guint damage_index;
    gboolean full_repaint;

    GLXPixmap root_glx_pixmap;
    GLuint root_texture;
    GLenum root_tex_type;
    gboolean root_repeat;
    GLuint black_texture;
    gboolean root_missing;
    gint root_width;
    gint root_height;
    Pixmap root_pixmap;
    Damage root_damage;
    gboolean root_dirty;

    GLuint fbo;
    GLuint fbo_texture;
    gint fbo_width;
    gint fbo_height;
    GLenum fbo_filter;
    gboolean fbo_failed;

    GLuint cursor_texture;
    gint cursor_width;
    gint cursor_height;
    unsigned long cursor_serial;

    gchar *renderer;

    /* A window turned up that this GPU cannot bind, so GL cannot draw the screen */
    gboolean give_up;
    /*
     * A window pixmap could not be bound this frame, so the frame has a hole in
     * it and is dropped rather than shown. Counted so that a refusal that never
     * goes away does not drop every frame from here on.
     */
    gboolean retry_paint;
    guint bind_failures;
} XfwmGLData;

static const gchar *vertex_source =
    "varying vec2 uv;\n"
    "void main (void)\n"
    "{\n"
    "    gl_Position = gl_Vertex;\n"
    "    uv = gl_MultiTexCoord0.xy;\n"
    "}\n";

static const gchar *fragment_source_2d =
    "uniform sampler2D tex;\n"
    "uniform float opacity;\n"
    "varying vec2 uv;\n"
    "void main (void)\n"
    "{\n"
    "    gl_FragColor = texture2D (tex, uv) * opacity;\n"
    "}\n";

static const gchar *fragment_source_rect =
    "#extension GL_ARB_texture_rectangle : enable\n"
    "uniform sampler2DRect tex;\n"
    "uniform float opacity;\n"
    "varying vec2 uv;\n"
    "void main (void)\n"
    "{\n"
    "    gl_FragColor = texture2DRect (tex, uv) * opacity;\n"
    "}\n";

/*
 * A box blurred by a gaussian is separable, so the shadow of any window big
 * enough is the product of one horizontal and one vertical edge profile. That
 * turns every shadow into a single quad sampling a small profile texture, with
 * no per window gaussian to compute and no per window texture to keep.
 */
static const gchar *fragment_shadow_profile =
    "uniform sampler2D prof;\n"
    "uniform vec2 size;\n"
    "uniform float ramp;\n"
    "uniform float opacity;\n"
    "varying vec2 uv;\n"
    "void main (void)\n"
    "{\n"
    "    vec2 p = uv * size;\n"
    "    vec2 q = clamp (min (p, size - p) / ramp, 0.0, 1.0);\n"
    "    float a = texture2D (prof, vec2 (q.x, 0.5)).a\n"
    "            * texture2D (prof, vec2 (q.y, 0.5)).a;\n"
    "    gl_FragColor = vec4 (0.0, 0.0, 0.0, a * opacity);\n"
    "}\n";

/*
 * Everything below works on client side regions. Asking the X server what a
 * window covers means waiting for a reply, and doing that for every window of
 * every frame is the most expensive thing a compositor can do. The shape of a
 * window only changes when the window does, so it is worked out once and kept.
 */
/*
 * How big a window is drawn this frame, and where that reaches on the screen.
 * Normally the window's own size; during a resize the pixmap behind it can
 * still be the old one, and nothing can be drawn from pixels that do not
 * exist yet. See window_painted_size().
 */
static void
window_painted_size (CWindow *cw, gint *width, gint *height);

/* The whole window as the X server sees it, border included */
static void
get_window_pixmap_size (CWindow *cw, gint *width, gint *height)
{
    /*
     * The pixmap's measured size, never the attributes': during a resize the
     * attributes run a step ahead, and drawing old content at the new size
     * stretches it a few pixels differently every frame, which the eye reads
     * as the window wobbling. The attributes remain the fallback for the
     * moment before a pixmap has ever been named.
     */
    if (cw->gl_pixmap_width > 0 && cw->gl_pixmap_height > 0)
    {
        *width = cw->gl_pixmap_width;
        *height = cw->gl_pixmap_height;

        return;
    }
    *width = cw->attr.width + 2 * cw->attr.border_width;
    *height = cw->attr.height + 2 * cw->attr.border_width;
}

/*
 * A window is drawn at its own size, or at the size of the pixmap behind it,
 * whichever is smaller. During a resize the pixmap is a step behind, and the
 * strip the window has just gained is in neither of them: not in the pixmap,
 * which is still the old size, and not anywhere else either, because the
 * client has not drawn it and the window manager has not drawn its border
 * into it. Nothing can paint pixels that do not exist.
 *
 * What is left is the choice of what shows there for that one frame, and the
 * strip is deliberately left holding what the screen already had: the same
 * window, its border and all, one step of the resize ago. The two
 * alternatives are worse. Stretching the edge of the pixmap into the strip
 * smears the border across the client's own area - a band down the side of
 * the window, which is what it looked like before. Painting the desktop there
 * takes the border away for that frame, and a window resized without its
 * bottom and right edges is what a person notices.
 */
static void
window_painted_size (CWindow *cw, gint *width, gint *height)
{
    gint pixmap_width, pixmap_height;
    gint border = 2 * cw->attr.border_width;

    get_window_pixmap_size (cw, &pixmap_width, &pixmap_height);

    *width = MIN (cw->attr.width + border, pixmap_width);
    *height = MIN (cw->attr.height + border, pixmap_height);
}

static cairo_region_t *
region_from_rects (XRectangle *rects, gint nrects, gint dx, gint dy)
{
    cairo_region_t *region;
    cairo_rectangle_int_t *boxes;
    gint i;

    if (nrects <= 0)
    {
        return cairo_region_create ();
    }

    boxes = g_new (cairo_rectangle_int_t, nrects);
    for (i = 0; i < nrects; i++)
    {
        boxes[i].x = rects[i].x + dx;
        boxes[i].y = rects[i].y + dy;
        boxes[i].width = rects[i].width;
        boxes[i].height = rects[i].height;
    }
    region = cairo_region_create_rectangles (boxes, nrects);
    g_free (boxes);

    return region;
}

/* The area a window covers on screen, its shape included */
static cairo_region_t *
window_shape (CWindow *cw)
{
    ScreenInfo *screen_info = cw->screen_info;
    DisplayInfo *display_info = screen_info->display_info;

    if (cw->gl_shape != NULL)
    {
        return cw->gl_shape;
    }

    /*
     * cw->shaped, not WIN_IS_SHAPED(): the latter tests the client, while the
     * window tracked here can be the frame, which themes with rounded corners
     * shape on their own. Treating such a frame as a rectangle draws the
     * undefined corners of its pixmap and hides what is really behind them.
     */
    if (cw->shaped)
    {
        XRectangle *rects;
        gint nrects = 0, ordering;
        gboolean answered;

        /*
         * Xlib hands back a null pointer both when the window has no shape
         * rectangles at all and when the request failed, so the error trap is
         * what tells the two apart. An empty shape means the window covers
         * nothing, and it must not fall through to the whole window below.
         */
        myDisplayErrorTrapPush (display_info);
        rects = XShapeGetRectangles (myScreenGetXDisplay (screen_info), cw->id,
                                     ShapeBounding, &nrects, &ordering);
        answered = (myDisplayErrorTrapPop (display_info) == Success);

        if (rects != NULL || answered)
        {
            cw->gl_shape = region_from_rects (rects, nrects,
                                              cw->attr.x + cw->attr.border_width,
                                              cw->attr.y + cw->attr.border_width);
            if (rects != NULL)
            {
                XFree (rects);
            }

            return cw->gl_shape;
        }
    }

    {
        cairo_rectangle_int_t r;

        r.x = cw->attr.x;
        r.y = cw->attr.y;
        get_window_pixmap_size (cw, &r.width, &r.height);
        cw->gl_shape = cairo_region_create_rectangle (&r);
    }

    return cw->gl_shape;
}

/*
 * The client area of a framed window, the whole window for anything else.
 * Returns FALSE when the window has no frame, as client_area() does.
 */
static gboolean
window_client_area (CWindow *cw, cairo_rectangle_int_t *r)
{
    /* The rule lives in compositor.c so both renderers read the same one */
    return client_area (cw, &r->x, &r->y, &r->width, &r->height);
}

/*
 * What the window itself says is opaque, in screen coordinates. Windows with an
 * alpha channel use this to tell us which part of them still hides what is
 * below, which is how most toolkit windows with rounded corners behave.
 */
static cairo_region_t *
window_opaque_region (CWindow *cw)
{
    cairo_rectangle_int_t client;
    gint dx, dy;

    if (cw->gl_opaque != NULL)
    {
        return cw->gl_opaque;
    }
    /*
     * The rectangles come from update_opaque_region(), which already read the
     * property, so the paint loop never asks the X server for them. A count of
     * zero is the cached answer that this window claims nothing.
     */
    if (cw->gl_n_opaque_rects == 0)
    {
        return NULL;
    }

    /*
     * The rectangles are relative to the client window. Taking the origin from
     * the client area rather than from the client keeps this cache, the bounding
     * shape and the drawing on the same coordinates while a move is still on its
     * way to the X server. A frameless window starts inside its own border.
     */
    if (window_client_area (cw, &client))
    {
        dx = client.x;
        dy = client.y;
    }
    else
    {
        dx = cw->attr.x + cw->attr.border_width;
        dy = cw->attr.y + cw->attr.border_width;
    }

    cw->gl_opaque = region_from_rects (cw->gl_opaque_rects,
                                       cw->gl_n_opaque_rects, dx, dy);

    /* Never claim more than the window covers */
    cairo_region_intersect (cw->gl_opaque, window_shape (cw));
    cairo_region_intersect_rectangle (cw->gl_opaque, &client);

    return cw->gl_opaque;
}

/*
 * Only what the window itself says is opaque. The bounding shape is a separate
 * fact and a change of one says nothing about the other.
 */
static void
xfwmGLInvalidateOpaqueRegion (CWindow *cw)
{
    g_return_if_fail (cw != NULL);

    if (cw->gl_opaque != NULL)
    {
        cairo_region_destroy (cw->gl_opaque);
        cw->gl_opaque = NULL;
    }
}

void
xfwmGLInvalidateWindowRegions (CWindow *cw)
{
    g_return_if_fail (cw != NULL);

    if (cw->gl_shape != NULL)
    {
        cairo_region_destroy (cw->gl_shape);
        cw->gl_shape = NULL;
    }
    if (cw->gl_paint_clip != NULL)
    {
        cairo_region_destroy (cw->gl_paint_clip);
        cw->gl_paint_clip = NULL;
    }
    xfwmGLInvalidateOpaqueRegion (cw);
}

/*
 * A window that only moved covers the same shape somewhere else, so the cached
 * regions are shifted rather than thrown away and asked for again.
 */
void
xfwmGLTranslateWindowRegions (CWindow *cw, gint dx, gint dy)
{
    g_return_if_fail (cw != NULL);

    if (cw->gl_shape != NULL)
    {
        cairo_region_translate (cw->gl_shape, dx, dy);
    }
    if (cw->gl_opaque != NULL)
    {
        cairo_region_translate (cw->gl_opaque, dx, dy);
    }
    if (cw->gl_paint_clip != NULL)
    {
        cairo_region_destroy (cw->gl_paint_clip);
        cw->gl_paint_clip = NULL;
    }
}

/*
 * Keep the rectangles _NET_WM_OPAQUE_REGION gave the compositor, so the paint
 * loop can build the region from them without reading the property again.
 */
void
xfwmGLSetOpaqueRects (CWindow *cw, XRectangle *rects, gint nrects)
{
    g_return_if_fail (cw != NULL);

    g_free (cw->gl_opaque_rects);
    cw->gl_opaque_rects = NULL;
    cw->gl_n_opaque_rects = 0;
    xfwmGLInvalidateOpaqueRegion (cw);

    /* Nothing reads a copy of these unless the GL renderer is the one drawing */
    if (cw->screen_info->gl_data == NULL)
    {
        return;
    }

    if (rects != NULL && nrects > 0)
    {
        cw->gl_opaque_rects = g_memdup2 (rects, nrects * sizeof (XRectangle));
        cw->gl_n_opaque_rects = nrects;
    }
}

static void
set_tex_params (GLenum target, GLint filter)
{
    glTexParameteri (target, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri (target, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri (target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static gboolean build_shadow_profile (ScreenInfo *screen_info);
static gdouble thread_cpu_ms (void);

static XfwmGLData *
gl_data (ScreenInfo *screen_info)
{
    return (XfwmGLData *) screen_info->gl_data;
}

/*
 * Whether GL calls would land in this screen's context. Any context being
 * current is not good enough: with a context of another screen current,
 * deleting a name would free someone else's object and leak ours, and GL
 * would say nothing about it.
 */
static gboolean
gl_context_is_current (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data != NULL && screen_info->use_egl_backend)
    {
        return (data->egl_context != EGL_NO_CONTEXT) &&
               (eglGetCurrentContext () == data->egl_context);
    }

    return (screen_info->glx_context != None) &&
           (glXGetCurrentContext () == screen_info->glx_context);
}

static GLuint
compile_shader (GLenum type, const gchar *source)
{
    GLuint shader;
    GLint status;

    shader = glCreateShader (type);
    if (shader == 0)
    {
        return 0;
    }

    glShaderSource (shader, 1, &source, NULL);
    glCompileShader (shader);
    glGetShaderiv (shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE)
    {
        gchar log[1024];
        GLsizei len = 0;

        glGetShaderInfoLog (shader, sizeof (log) - 1, &len, log);
        log[len] = '\0';
        g_warning ("Cannot compile GL shader: %s", log);
        glDeleteShader (shader);

        return 0;
    }

    return shader;
}

static GLuint
link_program (const gchar *fragment_source)
{
    GLuint program, vertex, fragment;
    GLint status;

    vertex = compile_shader (GL_VERTEX_SHADER, vertex_source);
    if (vertex == 0)
    {
        return 0;
    }

    fragment = compile_shader (GL_FRAGMENT_SHADER, fragment_source);
    if (fragment == 0)
    {
        glDeleteShader (vertex);
        return 0;
    }

    program = glCreateProgram ();
    glAttachShader (program, vertex);
    glAttachShader (program, fragment);
    glLinkProgram (program);
    /* The shaders are kept alive by the program */
    glDeleteShader (vertex);
    glDeleteShader (fragment);

    glGetProgramiv (program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE)
    {
        gchar log[1024];
        GLsizei len = 0;

        glGetProgramInfoLog (program, sizeof (log) - 1, &len, log);
        log[len] = '\0';
        g_warning ("Cannot link GL program: %s", log);
        glDeleteProgram (program);

        return 0;
    }

    return program;
}

/*
 * Find a frame buffer config able to bind a pixmap of that depth as a texture
 * on the given target. See pick_texture_target() for how the target is chosen.
 */
static gboolean
find_fbconfig (ScreenInfo *screen_info, gint depth, GLenum want_target,
               GLXFBConfig *fbconfig)
{
    Display *dpy = myScreenGetXDisplay (screen_info);
    GLint attribs[] = {
        GLX_DRAWABLE_TYPE,   GLX_PIXMAP_BIT,
        GLX_X_RENDERABLE,    True,
        GLX_RENDER_TYPE,     GLX_RGBA_BIT,
        GLX_BUFFER_SIZE,     depth,
        GLX_DEPTH_SIZE,      0,
        GLX_STENCIL_SIZE,    0,
        None
    };
    GLXFBConfig *configs;
    gint n_configs, i;
    gboolean found = FALSE;

    configs = glXChooseFBConfig (dpy, screen_info->screen, attribs, &n_configs);
    if (configs == NULL)
    {
        return FALSE;
    }

    for (i = 0; i < n_configs; i++)
    {
        XVisualInfo *visual_info;
        int value, status;
        gboolean depth_match;

        visual_info = glXGetVisualFromFBConfig (dpy, configs[i]);
        depth_match = (visual_info != NULL && visual_info->depth == depth);
        if (visual_info)
        {
            XFree (visual_info);
        }
        if (!depth_match)
        {
            continue;
        }

        status = glXGetFBConfigAttrib (dpy, configs[i],
                                       (depth == 32) ? GLX_BIND_TO_TEXTURE_RGBA_EXT
                                                     : GLX_BIND_TO_TEXTURE_RGB_EXT,
                                       &value);
        if (status != Success || value != True)
        {
            continue;
        }

        status = glXGetFBConfigAttrib (dpy, configs[i],
                                       GLX_BIND_TO_TEXTURE_TARGETS_EXT, &value);
        if (status != Success)
        {
            continue;
        }

        /*
         * Every depth has to end up on the same target, the pixmaps are all
         * bound and sampled by the same code.
         */
        if (!(value & ((want_target == GLX_TEXTURE_2D_EXT)
                       ? GLX_TEXTURE_2D_BIT_EXT : GLX_TEXTURE_RECTANGLE_BIT_EXT)))
        {
            continue;
        }

        *fbconfig = configs[i];
        found = TRUE;
        break;
    }
    XFree (configs);

    return found;
}

/*
 * Sync the swaps to the screen. The XRender path does this on the pixmap it
 * presents, here the frames go straight to the overlay window.
 *
 *   off      no sync at all, the fastest but it tears
 *   adaptive sync, except that a late frame is shown at once rather than
 *            held back until the next refresh
 *   other    sync to every vblank
 */
static void
set_swap_interval_gl (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    gint interval;

    interval = wanted_swap_interval (screen_info);

    if (data != NULL && screen_info->use_egl_backend)
    {
        /*
         * EGL knows no adaptive interval, so a late frame waits like any
         * other: that is an interval of one. Zero would be no sync at all.
         */
        if (interval < 0)
        {
            interval = 1;
        }
        screen_info->glx_swap_control =
            eglSwapInterval (data->egl_display, interval);
        screen_info->glx_swap_interval = interval;
    }
    else
    {
        if ((screen_info->vblank_mode == VBLANK_ADAPTIVE) && (interval >= 0))
        {
            g_info ("GLX_EXT_swap_control_tear is missing, syncing to every vblank");
        }

        /* Recorded on the ScreenInfo so vsync_state() reads one place for both renderers */
        screen_info->glx_swap_control = apply_swap_interval (screen_info,
                                                             screen_info->glx_window,
                                                             &interval);
        screen_info->glx_swap_interval = interval;
    }

    if (screen_info->glx_swap_control)
    {
        g_info ("GL swap interval set to %i", interval);
    }
    else
    {
        g_info ("No swap control available, frames are not synced to the screen");
    }
}

/*
 * The config for a depth, looked up once and remembered, including the answer
 * that there is none.
 */
static XfwmGLDepth *
depth_config (ScreenInfo *screen_info, gint depth)
{
    XfwmGLData *data = gl_data (screen_info);
    XfwmGLDepth *entry;
    guint i;

    for (i = 0; i < data->n_depths; i++)
    {
        if (data->depths[i].depth == depth)
        {
            return &data->depths[i];
        }
    }

    if (data->n_depths == GL_MAX_DEPTHS)
    {
        return NULL;
    }

    entry = &data->depths[data->n_depths++];
    entry->depth = depth;
    entry->usable = find_fbconfig (screen_info, depth, data->tex_target,
                                   &entry->fbconfig);
    if (!entry->usable)
    {
        g_info ("No GLX config to bind a window of depth %i as a texture", depth);
    }

    return entry;
}

/* There is no entry at all once the table is full, which is not usable either */
static gboolean
depth_is_usable (ScreenInfo *screen_info, gint depth)
{
    XfwmGLDepth *dc = depth_config (screen_info, depth);

    return (dc != NULL && dc->usable);
}

/*
 * Drivers that offer the rectangle texture target and then do not honour it:
 * binding a window to it is accepted and then samples black, which blacks out
 * the screen. The same windows draw correctly on the 2D target, so they get
 * that one.
 */
static gboolean
renderer_needs_2d_target (const char *renderer)
{
    const char *needs_2d[] = { "virgl", NULL };

    if (renderer == NULL)
    {
        return FALSE;
    }

    return renderer_matches_any (renderer, needs_2d);
}

/*
 * Rectangle textures come first: addressed in pixels, they cannot disagree with
 * the real width of the pixmap, while a normalised GL_TEXTURE_2D relies on the
 * driver mapping 1.0 onto the last texel. Where it pads the allocation instead,
 * the window is drawn slightly stretched and the content wobbles as it resizes.
 *
 * The target picked has to work for opaque and ARGB windows alike, and its
 * shader has to compile: rectangle pixmaps can be offered without sampler2DRect.
 */
static gboolean
pick_texture_target (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    gboolean prefer_2d;
    guint i;

    /*
     * Rectangle textures first, since they are addressed in pixels and so cannot
     * be stretched by a driver that pads its allocations, except where the
     * driver cannot be trusted with them.
     */
    prefer_2d = renderer_needs_2d_target ((const char *) glGetString (GL_RENDERER));

    for (i = 0; i < 2; i++)
    {
        GLenum target;

        if (prefer_2d)
        {
            target = (i == 0) ? GLX_TEXTURE_2D_EXT : GLX_TEXTURE_RECTANGLE_EXT;
        }
        else
        {
            target = (i == 0) ? GLX_TEXTURE_RECTANGLE_EXT : GLX_TEXTURE_2D_EXT;
        }

        if (target == GLX_TEXTURE_RECTANGLE_EXT &&
            !epoxy_has_gl_extension ("GL_ARB_texture_rectangle") &&
            epoxy_gl_version () < 31)
        {
            continue;
        }

        data->n_depths = 0;
        data->tex_target = target;
        data->tex_type = (target == GLX_TEXTURE_2D_EXT) ? GL_TEXTURE_2D
                                                        : GL_TEXTURE_RECTANGLE_ARB;

        /* Opaque and ARGB windows both have to work, they are always around */
        if (!depth_is_usable (screen_info, 24) ||
            !depth_is_usable (screen_info, 32))
        {
            continue;
        }

        data->program_win = link_program ((data->tex_type == GL_TEXTURE_2D)
                                          ? fragment_source_2d
                                          : fragment_source_rect);
        if (data->program_win != 0)
        {
            g_info ("Using the %s texture target",
                    (data->tex_type == GL_TEXTURE_2D) ? "2D" : "rectangle");

            return TRUE;
        }

        g_info ("The shader for this texture target does not compile, trying another");
    }

    return FALSE;
}

/*
 * eglGetDisplay() hands every screen of the same X display the same EGLDisplay,
 * and eglTerminate() is not reference counted: terminating for one screen would
 * take the other screens' contexts and images with it. So the users are counted
 * here and the display is only terminated by the last one out.
 */
static guint egl_display_users = 0;

/* Unbind and drop the surface; the context and everything in it stay */
static void
egl_release_surface (XfwmGLData *data)
{
    /*
     * Only if it is ours. Every screen of the display shares the EGLDisplay,
     * so unbinding blind would take another screen's context off the thread.
     */
    if (eglGetCurrentContext () == data->egl_context)
    {
        eglMakeCurrent (data->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        EGL_NO_CONTEXT);
    }
    if (data->egl_surface != EGL_NO_SURFACE)
    {
        eglDestroySurface (data->egl_display, data->egl_surface);
        data->egl_surface = EGL_NO_SURFACE;
    }
}

/*
 * A window surface on the output window, made current. Shared by the first
 * start and by coming back from a suspend, where the output window is new.
 */
static gboolean
egl_attach_output_surface (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    data->egl_surface =
        eglCreateWindowSurface (data->egl_display, data->egl_config,
                                (EGLNativeWindowType) screen_info->output,
                                NULL);

    return (data->egl_surface != EGL_NO_SURFACE &&
            eglMakeCurrent (data->egl_display, data->egl_surface,
                            data->egl_surface, data->egl_context));
}

/*
 * Everything EGL holds for this screen. Safe to call on a half built setup,
 * which is how a failed initialisation cleans up after itself.
 */
static void
egl_screen_finish (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data->egl_display == EGL_NO_DISPLAY)
    {
        return;
    }

    egl_release_surface (data);
    if (data->egl_context != EGL_NO_CONTEXT)
    {
        eglDestroyContext (data->egl_display, data->egl_context);
        data->egl_context = EGL_NO_CONTEXT;
    }
    if (egl_display_users > 0 && --egl_display_users == 0)
    {
        eglTerminate (data->egl_display);
        eglReleaseThread ();
    }
    data->egl_display = EGL_NO_DISPLAY;
}

/*
 * Bring up EGL on the output window: a desktop GL context, a window surface
 * on the visual the output already has, and the extensions this backend
 * cannot do without. Leaves its context current on success. On failure
 * everything is torn down again and the GLX context is put back.
 */
static gboolean
egl_screen_init (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    Display *dpy = myScreenGetXDisplay (screen_info);
    XWindowAttributes attr;
    /* Deep visuals sort last, so the list must hold everything offered */
    EGLConfig configs[256];
    EGLConfig config = NULL;
    EGLint n_configs = 0, visual_id, major, minor, i;
    const EGLint wanted[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_NONE
    };

    data->egl_display = eglGetDisplay ((EGLNativeDisplayType) dpy);
    if (data->egl_display == EGL_NO_DISPLAY ||
        !eglInitialize (data->egl_display, &major, &minor))
    {
        g_warning ("Cannot initialise EGL, staying on GLX.");
        data->egl_display = EGL_NO_DISPLAY;

        return FALSE;
    }
    egl_display_users++;

    if (!eglBindAPI (EGL_OPENGL_API))
    {
        g_warning ("EGL offers no desktop OpenGL here, staying on GLX.");
        goto failed;
    }

    /* The one way this backend has of reading a window pixmap */
    if (!epoxy_has_egl_extension (data->egl_display, "EGL_KHR_image_pixmap"))
    {
        g_warning ("EGL_KHR_image_pixmap is missing, staying on GLX.");
        goto failed;
    }

    /* The surface sits on the output window, so its visual decides the config */
    if (!XGetWindowAttributes (dpy, screen_info->output, &attr) ||
        !eglChooseConfig (data->egl_display, wanted, configs,
                          (EGLint) G_N_ELEMENTS (configs), &n_configs))
    {
        g_warning ("No usable EGL configs, staying on GLX.");
        goto failed;
    }
    for (i = 0; i < n_configs; i++)
    {
        if (eglGetConfigAttrib (data->egl_display, configs[i],
                                EGL_NATIVE_VISUAL_ID, &visual_id) &&
            (VisualID) visual_id == XVisualIDFromVisual (attr.visual))
        {
            config = configs[i];
            break;
        }
    }
    if (config == NULL)
    {
        g_warning ("No EGL config matches the output window "
                   "(%d configs, visual 0x%lx), staying on GLX.",
                   (gint) n_configs,
                   (gulong) XVisualIDFromVisual (attr.visual));
        goto failed;
    }

    data->egl_context = eglCreateContext (data->egl_display, config,
                                          EGL_NO_CONTEXT, NULL);
    if (data->egl_context == EGL_NO_CONTEXT)
    {
        g_warning ("Cannot create an EGL context, staying on GLX.");
        goto failed;
    }

    data->egl_config = config;
    if (!egl_attach_output_surface (screen_info))
    {
        g_warning ("Cannot make the EGL context current, staying on GLX.");
        goto failed;
    }

    /* The GL side of sampling an EGL image, asked with the context current */
    if (!epoxy_has_gl_extension ("GL_OES_EGL_image"))
    {
        g_warning ("GL_OES_EGL_image is missing, staying on GLX.");
        goto failed;
    }

    /* Normally learnt from the GLX context, which does not exist here */
    screen_info->has_ext_arb_sync = epoxy_has_gl_extension ("GL_ARB_sync");
    data->has_buffer_age = epoxy_has_egl_extension (data->egl_display,
                                                    "EGL_EXT_buffer_age");
    if (epoxy_has_egl_extension (data->egl_display,
                                 "EGL_KHR_swap_buffers_with_damage"))
    {
        data->egl_swap_with_damage = eglSwapBuffersWithDamageKHR;
    }
    else if (epoxy_has_egl_extension (data->egl_display,
                                      "EGL_EXT_swap_buffers_with_damage"))
    {
        data->egl_swap_with_damage = eglSwapBuffersWithDamageEXT;
    }

    g_info ("GL compositing through EGL %d.%d (buffer age %s, "
            "swap with damage %s)", major, minor,
            data->has_buffer_age ? "yes" : "no",
            data->egl_swap_with_damage != NULL ? "yes" : "no");

    return TRUE;

failed:
    egl_screen_finish (screen_info);
    /* Whatever was current before eglMakeCurrent() may have been lost */
    if (screen_info->glx_context != None)
    {
        glXMakeCurrent (dpy, screen_info->glx_window, screen_info->glx_context);
    }

    return FALSE;
}

gboolean
xfwmGLScreenInit (ScreenInfo *screen_info)
{
    XfwmGLData *data;
    Display *dpy;
    gboolean no_ext;

    g_return_val_if_fail (screen_info != NULL, FALSE);
    TRACE ("entering");

    dpy = myScreenGetXDisplay (screen_info);

    /*
     * A window is drawn from the pixmap the Composite extension names for it,
     * and there is nothing else to draw it from here: the XRender path can fall
     * back to the window drawable itself, a texture cannot. Without this the
     * binding below would fail on every window, one at a time, forever.
     */
    if (!screen_info->display_info->have_name_window_pixmap)
    {
        g_warning ("The X server cannot name window pixmaps, GL compositing disabled.");
        return FALSE;
    }

    data = g_new0 (XfwmGLData, 1);
    screen_info->gl_data = data;

    /*
     * The same renderer on an EGL context instead of a GLX one. The choice was
     * made once, in setup_gl(); re-reading the environment here is how the
     * automatic selection silently initialised the wrong backend on NVIDIA.
     */
    if (screen_info->use_egl_backend)
    {
        egl_screen_init (screen_info);

        if (data->egl_context == EGL_NO_CONTEXT)
        {
            /* setup_gl() made no GLX context in this mode, so EGL or nothing */
            if (screen_info->glx_context == None)
            {
                xfwmGLScreenFinish (screen_info);
                return FALSE;
            }
            /*
             * There is a GLX context to fall back on, so carry on with it -
             * but not while still claiming to be on EGL, or every EGL call
             * below would be made against a display that was never opened.
             */
            screen_info->use_egl_backend = FALSE;
        }
    }

    if (!screen_info->use_egl_backend &&
        !epoxy_has_glx_extension (dpy, screen_info->screen,
                                  "GLX_EXT_texture_from_pixmap"))
    {
        g_warning ("GLX_EXT_texture_from_pixmap is missing, GL compositing disabled.");
        xfwmGLScreenFinish (screen_info);
        return FALSE;
    }

    /* Asked of whichever context the backend above left current */
    if (epoxy_gl_version () < 20)
    {
        g_warning ("OpenGL 2.0 is required for GL compositing, disabled.");
        xfwmGLScreenFinish (screen_info);
        return FALSE;
    }

    if (!epoxy_has_gl_extension ("GL_ARB_framebuffer_object") &&
        !epoxy_has_gl_extension ("GL_EXT_framebuffer_object"))
    {
        g_warning ("Frame buffer objects are missing, GL compositing disabled.");
        xfwmGLScreenFinish (screen_info);
        return FALSE;
    }

    if (screen_info->use_egl_backend)
    {
        /*
         * EGL images always come out on the 2D target, and every depth the
         * server can hand us is theirs to translate, so nothing has to be
         * picked per depth here.
         */
        data->tex_target = GLX_TEXTURE_2D_EXT;
        data->tex_type = GL_TEXTURE_2D;
        data->program_win = link_program (fragment_source_2d);
        if (data->program_win == 0)
        {
            g_warning ("The window shader does not compile, GL compositing disabled.");
            xfwmGLScreenFinish (screen_info);
            return FALSE;
        }
    }
    else if (!pick_texture_target (screen_info))
    {
        g_warning ("No GLX config to bind windows as textures, GL compositing disabled.");
        xfwmGLScreenFinish (screen_info);

        return FALSE;
    }

    /*
     * Textures of any size are part of OpenGL 2.0, which is required above, and
     * everything uploaded here stays within what even the earliest hardware to
     * offer them can do: no mipmaps and no repeating. So there is nothing left
     * to check for separately.
     */

    data->u_opacity_win = glGetUniformLocation (data->program_win, "opacity");
    glUseProgram (data->program_win);
    glUniform1i (glGetUniformLocation (data->program_win, "tex"), 0);

    /*
     * Textures we upload ourselves are always plain 2D: the shadows, the
     * cursor and the scene the magnifier scales back up. A driver that only
     * offers the rectangle target for window pixmaps still needs a 2D program
     * for those, so there is always one.
     */
    if (data->tex_type == GL_TEXTURE_2D)
    {
        data->program_2d = data->program_win;
        data->u_opacity_2d = data->u_opacity_win;
    }
    else
    {
        data->program_2d = link_program (fragment_source_2d);
        if (data->program_2d == 0)
        {
            xfwmGLScreenFinish (screen_info);
            return FALSE;
        }
        data->u_opacity_2d = glGetUniformLocation (data->program_2d, "opacity");
        glUseProgram (data->program_2d);
        glUniform1i (glGetUniformLocation (data->program_2d, "tex"), 0);
    }

    /* Shadows of windows large enough are drawn straight from a profile */
    data->program_shadow_profile = link_program (fragment_shadow_profile);
    if (data->program_shadow_profile != 0)
    {
        data->u_prof_size = glGetUniformLocation (data->program_shadow_profile, "size");
        data->u_prof_ramp = glGetUniformLocation (data->program_shadow_profile, "ramp");
        data->u_prof_opacity = glGetUniformLocation (data->program_shadow_profile, "opacity");
        glUseProgram (data->program_shadow_profile);
        glUniform1i (glGetUniformLocation (data->program_shadow_profile, "prof"), 0);
    }

    data->renderer = g_strdup ((const gchar *) glGetString (GL_RENDERER));
    /* Used wherever a plain black area has to be filled */
    {
        static const guchar black[4] = { 0, 0, 0, 0xff };

        glGenTextures (1, &data->black_texture);
        glBindTexture (GL_TEXTURE_2D, data->black_texture);
        set_tex_params (GL_TEXTURE_2D, GL_NEAREST);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, black);
        glBindTexture (GL_TEXTURE_2D, 0);
    }

    if (!screen_info->use_egl_backend)
    {
        /* The EGL backend answered this from its own extension list */
        data->has_buffer_age = epoxy_has_glx_extension (dpy, screen_info->screen,
                                                        "GLX_EXT_buffer_age");
    }

    /*
     * XFWM4_GL_NO_EXT=1: behave as though the driver offered neither
     * GLX_EXT_buffer_age nor GLX_MESA_copy_sub_buffer, which is the worst case
     * a GL compositor can be handed. Without the age the swap cannot know what
     * the buffer it was given still holds, so every frame paints the whole
     * screen; without the Mesa copy there is no scene buffer to fall back to.
     * It exists to price that case on hardware that does not have it, which is
     * the only way to guess at what a driver like the NVIDIA one would do.
     */
    no_ext = (g_getenv ("XFWM4_GL_NO_EXT") != NULL);
    if (no_ext)
    {
        data->has_buffer_age = FALSE;
    }
    data->full_repaint = TRUE;

    /*
     * Swap by default. Moving fewer pixels through the presentation path
     * sounds cheaper, and the other three modes do exactly that, but a
     * whole-screen swap is a page flip the display engine performs for
     * nothing, while a partial copy is real work the GPU has to do in the
     * middle of the application's. The other modes stay available through
     * XFWM4_GL_PRESENT for drivers that behave differently.
     */
    data->present_mode = GL_PRESENT_SWAP;
    /*
     * The scene buffer presents by swapping, so it needs nothing the swap mode
     * does not have and works on both backends. It is not the default: on
     * radeonsi it composites less and costs more. Measured against swapping on
     * the same work, whole benchmark, two runs each: 13.14 W against 13.13 W,
     * 3.23 s of processor time against 3.00 s, 168.9 fps left to the
     * application against 174.6, and 96 MB more video memory for the screen
     * sized texture. During a resize it composites 98 Mpix a second where
     * swapping composites 115, so the overdraw the buffer age causes is real -
     * it is just only 17%, and blitting the stale region out of the scene
     * costs more than compositing it again. Kept for drivers where partial
     * repaint is dearer than that, which is what the mode exists to price.
     */
    if (g_strcmp0 (g_getenv ("XFWM4_GL_PRESENT"), "scene") == 0)
    {
        data->present_mode = GL_PRESENT_SCENE;
        g_info ("GL presentation mode: scene buffer");
    }
    else if (!screen_info->use_egl_backend)
    {
        const gchar *mode = g_getenv ("XFWM4_GL_PRESENT");
        gboolean has_copy_sub_buffer = !no_ext &&
            epoxy_has_glx_extension (dpy, screen_info->screen,
                                     "GLX_MESA_copy_sub_buffer");

        if (mode == NULL || g_strcmp0 (mode, "auto") == 0)
        {
            /*
             * Swap. The persistent-scene path used to be preferred here
             * because it looked much cheaper for the processor, but that was
             * read from whole-machine CPU time, which cannot resolve a
             * compositor at all on this hardware. Measured by package power
             * instead, against the same screen with compositing switched off,
             * swapping costs 1.13 W where the scene buffer costs 1.52 W and
             * XRender costs 1.74 W, and it leaves the application 173.7 fps
             * where the scene buffer leaves it 163.1. Swapping wins both.
             *
             * With one exception. Swapping only paints the damage because the
             * buffer age says what the buffer we are given still holds;
             * without that extension every frame paints the whole screen, and
             * the scene buffer, which owes nothing to the age, is then the
             * better of the two. No driver we can test on takes this branch.
             */
            if (data->has_buffer_age || !has_copy_sub_buffer)
            {
                data->present_mode = GL_PRESENT_SWAP;
            }
            else
            {
                data->present_mode = GL_PRESENT_FBO;
            }
        }
        else if (g_strcmp0 (mode, "front") == 0)
        {
            g_warning ("The front presentation mode was removed, it tears "
                       "over anything that repaints continuously; swapping.");
        }
        else if (g_strcmp0 (mode, "copy") == 0 ||
                 g_strcmp0 (mode, "fbo") == 0)
        {
            if (!has_copy_sub_buffer)
            {
                g_warning ("XFWM4_GL_PRESENT=%s wanted but "
                           "GLX_MESA_copy_sub_buffer is missing, swapping.", mode);
            }
            else if (g_strcmp0 (mode, "copy") == 0)
            {
                data->present_mode = GL_PRESENT_COPY;
            }
            else
            {
                data->present_mode = GL_PRESENT_FBO;
            }
        }
        else if (g_strcmp0 (mode, "swap") != 0)
        {
            g_warning ("Unknown XFWM4_GL_PRESENT=%s, swapping.", mode);
        }
        g_info ("GL presentation mode: %s",
                data->present_mode == GL_PRESENT_SWAP ? "swap" :
                data->present_mode == GL_PRESENT_COPY ? "copy" :
                "fbo");
    }
    data->wait_new_pixmap =
        (g_strcmp0 (g_getenv ("XFWM4_GL_PIXMAP_WAIT"), "off") != 0);
    data->no_paint = (g_getenv ("XFWM4_GL_NOPAINT") != NULL);
    if (data->no_paint)
    {
        g_warning ("XFWM4_GL_NOPAINT is set: nothing will be drawn, the screen "
                   "will be garbage. This is a measurement mode.");
    }
    data->use_fence =
        (g_strcmp0 (g_getenv ("XFWM4_GL_FENCE"), "off") != 0);
    if (!data->use_fence)
    {
        g_info ("GL frames are not fenced, the repaint loop will not wait");
    }
    data->stats = (g_getenv ("XFWM4_GL_STATS") != NULL);
    data->profile = (g_getenv ("XFWM4_GL_PROFILE") != NULL);
    /* Timer queries arrived after the GL 2.0 this renderer asks for */
    data->prof_gpu = data->profile &&
                     (epoxy_gl_version () >= 33 ||
                      epoxy_has_gl_extension ("GL_ARB_timer_query"));
    if (data->profile && !data->prof_gpu)
    {
        g_info ("This driver has no timer queries, the profile shows no GPU time");
    }
    data->stats |= data->profile;
    data->stat_since = g_get_monotonic_time ();

    set_swap_interval_gl (screen_info);

    glDisable (GL_DEPTH_TEST);
    glDepthMask (GL_FALSE);
    glDisable (GL_CULL_FACE);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    g_info ("GL compositing enabled (%s, buffer age %s)",
            (data->tex_type == GL_TEXTURE_2D) ? "texture 2D" : "texture rectangle",
            data->has_buffer_age ? "yes" : "no");

    return TRUE;
}

/*
 * The background pixmap belongs to whoever drew the desktop and may already be
 * gone by the time we let go of it, so this carries its own error trap.
 */
static void
free_root_texture (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    DisplayInfo *display_info = screen_info->display_info;
    Display *dpy = myScreenGetXDisplay (screen_info);

    if (data == NULL)
    {
        return;
    }

    if (data->root_damage != None)
    {
        myDisplayErrorTrapPush (display_info);
        XDamageDestroy (dpy, data->root_damage);
        myDisplayErrorTrapPopIgnored (display_info);
        data->root_damage = None;
    }
    if (data->root_glx_pixmap != None)
    {
        myDisplayErrorTrapPush (display_info);
        if ((data->root_texture != 0) && gl_context_is_current (screen_info))
        {
            /* Releasing acts on the bound texture, see xfwmGLFreeWindowData() */
            glBindTexture (data->root_tex_type, data->root_texture);
            glXReleaseTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT);
            glBindTexture (data->root_tex_type, 0);
        }
        glXDestroyPixmap (dpy, data->root_glx_pixmap);
        myDisplayErrorTrapPopIgnored (display_info);
        data->root_glx_pixmap = None;
    }
    if (data->root_egl_image != NULL)
    {
        eglDestroyImageKHR (data->egl_display, (EGLImageKHR) data->root_egl_image);
        data->root_egl_image = NULL;
    }
    if (data->root_texture != 0 && gl_context_is_current (screen_info))
    {
        glDeleteTextures (1, &data->root_texture);
    }
    data->root_texture = 0;
    data->root_repeat = FALSE;
    data->root_pixmap = None;
    data->root_dirty = FALSE;
    data->root_missing = FALSE;
}

/*
 * Whoever drew the desktop may paint into the same pixmap again without
 * announcing it, and the contents of a bound texture are undefined once that
 * happens. Damage on the background pixmap is what says the image has to be
 * taken again, the same way a window says it with cw->gl_content_dirty.
 */
gboolean
xfwmGLDamageRootPixmap (ScreenInfo *screen_info, Drawable drawable)
{
    XfwmGLData *data;

    g_return_val_if_fail (screen_info != NULL, FALSE);

    data = gl_data (screen_info);
    if (data == NULL || data->root_pixmap == None ||
        data->root_pixmap != (Pixmap) drawable)
    {
        return FALSE;
    }

    data->root_dirty = TRUE;

    return TRUE;
}

/*
 * Whether damage on the background pixmap can be taken as screen damage as it
 * is: only when one copy of the pixmap covers the screen do its coordinates
 * mean the same as screen coordinates. A smaller background is tiled, so one
 * dirty spot on it shows in many places.
 */
gboolean
xfwmGLRootPixmapCoversScreen (ScreenInfo *screen_info)
{
    XfwmGLData *data;

    g_return_val_if_fail (screen_info != NULL, FALSE);

    data = gl_data (screen_info);

    return (data != NULL) &&
           (data->root_width >= screen_info->width) &&
           (data->root_height >= screen_info->height);
}

void
xfwmGLInvalidateRootTexture (ScreenInfo *screen_info)
{
    g_return_if_fail (screen_info != NULL);

    /* free_root_texture() copes with a screen that has no GL data */
    free_root_texture (screen_info);
}

static void
free_fbo (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data->fbo != 0)
    {
        glDeleteFramebuffers (1, &data->fbo);
        data->fbo = 0;
    }
    if (data->fbo_texture != 0)
    {
        glDeleteTextures (1, &data->fbo_texture);
        data->fbo_texture = 0;
    }
    data->fbo_width = 0;
    data->fbo_height = 0;
    data->fbo_filter = 0;
}

/*
 * The cursor image is only drawn by the magnifier, but it has nothing to do
 * with the frame buffer the magnifier renders into: it does not change with the
 * size of the screen and it is cheap to keep, so it only goes when the
 * magnifier is turned off altogether.
 */
static void
free_cursor_texture (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data->cursor_texture != 0)
    {
        glDeleteTextures (1, &data->cursor_texture);
        data->cursor_texture = 0;
        data->cursor_serial = 0;
    }
}

void
xfwmGLScreenFinish (ScreenInfo *screen_info)
{
    XfwmGLData *data;
    guint i;

    g_return_if_fail (screen_info != NULL);
    TRACE ("entering");

    data = gl_data (screen_info);
    if (data == NULL)
    {
        return;
    }

    /*
     * Without a context the driver already dropped everything that lived in it,
     * or is about to when the context goes. The GLX pixmap of the background is
     * not one of those, it belongs to the X server, so it has to go either way.
     */
    free_root_texture (screen_info);

    if (gl_context_is_current (screen_info))
    {
        free_fbo (screen_info);
        free_cursor_texture (screen_info);

        if (data->black_texture != 0)
        {
            glDeleteTextures (1, &data->black_texture);
        }
        if (data->program_win != 0)
        {
            glDeleteProgram (data->program_win);
        }
        if (data->program_2d != 0 && data->program_2d != data->program_win)
        {
            glDeleteProgram (data->program_2d);
        }
        if (data->program_shadow_profile != 0)
        {
            glDeleteProgram (data->program_shadow_profile);
        }
        if (data->shadow_profile != 0)
        {
            glDeleteTextures (1, &data->shadow_profile);
        }
    }

    for (i = 0; i < GL_DAMAGE_HISTORY; i++)
    {
        if (data->damage_history[i] != NULL)
        {
            cairo_region_destroy (data->damage_history[i]);
        }
    }

    /* Terminating the display takes every EGL image and surface with it */
    egl_screen_finish (screen_info);

    g_free (data->renderer);
    g_free (data);
    screen_info->gl_data = NULL;
}

const gchar *
xfwmGLGetRendererName (ScreenInfo *screen_info)
{
    XfwmGLData *data;

    g_return_val_if_fail (screen_info != NULL, NULL);

    data = gl_data (screen_info);

    return (data != NULL) ? data->renderer : NULL;
}

/*
 * Called after the drawable has been made again following a suspend. The back
 * buffer of a brand new drawable holds nothing, so the next frame is whole.
 */
void
xfwmGLScreenReattached (ScreenInfo *screen_info)
{
    XfwmGLData *data;

    g_return_if_fail (screen_info != NULL);
    TRACE ("entering");

    data = gl_data (screen_info);
    if (data == NULL)
    {
        return;
    }

    /*
     * The EGL surface sat on the old output window. It is rebuilt before
     * anything below touches GL, so the context is current for it. Should
     * the new window refuse a surface, the next paint fails to make the
     * context current and the screen falls back to XRender from there.
     */
    if (screen_info->use_egl_backend)
    {
        egl_release_surface (data);
        if (!egl_attach_output_surface (screen_info))
        {
            g_warning ("Cannot rebuild the EGL surface.");
        }
    }

    data->full_repaint = TRUE;
    /*
     * The screen size or the desktop background may have changed while we were
     * away, so the background is bound again on the next frame. A size change
     * never reached xfwmGLScreenSizeChanged() while we were suspended, so a
     * frame buffer that was too big for the driver may fit the screen now.
     */
    data->fbo_failed = FALSE;
    free_root_texture (screen_info);

    set_swap_interval_gl (screen_info);
}

/*
 * Called when compositing suspends but the renderer is kept: the output window
 * is about to be destroyed, and an EGL surface must not outlive the window it
 * sits on. The context and everything built in it stay, the GLX equivalent is
 * detach_glx_window() on the compositor side.
 */
void
xfwmGLScreenDetached (ScreenInfo *screen_info)
{
    XfwmGLData *data;

    g_return_if_fail (screen_info != NULL);
    TRACE ("entering");

    data = gl_data (screen_info);
    if (data == NULL || !screen_info->use_egl_backend)
    {
        return;
    }

    egl_release_surface (data);
}

void
xfwmGLScreenSizeChanged (ScreenInfo *screen_info)
{
    g_return_if_fail (screen_info != NULL);

    /* Nothing to drop while suspended, the drawable is gone anyway */
    if (screen_info->gl_data == NULL || !gl_context_is_current (screen_info))
    {
        return;
    }

    free_root_texture (screen_info);
    free_fbo (screen_info);
    /* A frame buffer that was too big for the driver may fit the new size */
    gl_data (screen_info)->fbo_failed = FALSE;
    gl_data (screen_info)->full_repaint = TRUE;
}

void
xfwmGLFreeWindowData (CWindow *cw)
{
    ScreenInfo *screen_info;
    Display *dpy;

    g_return_if_fail (cw != NULL);

    screen_info = cw->screen_info;
    if (screen_info->gl_data == NULL)
    {
        return;
    }
    dpy = myScreenGetXDisplay (screen_info);

    /*
     * The GLX pixmap must go whatever happens, it is tied to an X pixmap that
     * is about to be freed. Only the texture calls need a current context.
     */
    if (cw->gl_pixmap != None)
    {
        if (cw->gl_texture_bound && (cw->gl_texture != 0) &&
            gl_context_is_current (screen_info))
        {
            /*
             * Releasing acts on whatever texture is bound to the target at the
             * time, the call names a drawable but not a texture, so this
             * window's texture has to be made current first. The paint loop
             * leaves nothing bound, so without this the release lands on
             * texture zero and does nothing, and the GLX pixmap below is
             * destroyed with its image still bound to a texture, which the
             * extension leaves undefined.
             */
            glBindTexture (gl_data (screen_info)->tex_type, cw->gl_texture);
            glXReleaseTexImageEXT (dpy, cw->gl_pixmap, GLX_FRONT_EXT);
            glBindTexture (gl_data (screen_info)->tex_type, 0);
        }
        cw->gl_texture_bound = FALSE;
        glXDestroyPixmap (dpy, cw->gl_pixmap);
        cw->gl_pixmap = None;
    }
    if (cw->egl_image != NULL)
    {
        eglDestroyImageKHR (gl_data (screen_info)->egl_display,
                            (EGLImageKHR) cw->egl_image);
        cw->egl_image = NULL;
        cw->gl_texture_bound = FALSE;
    }
    if (cw->gl_texture != 0 && gl_context_is_current (screen_info))
    {
        glDeleteTextures (1, &cw->gl_texture);
        cw->gl_texture = 0;
    }
    /* The size belonged to the pixmap that just went with the data above */
    cw->gl_pixmap_width = 0;
    cw->gl_pixmap_height = 0;
}

void
xfwmGLFreeWindowShadow (CWindow *cw)
{
    g_return_if_fail (cw != NULL);

    if (cw->screen_info->gl_data == NULL ||
        !gl_context_is_current (cw->screen_info))
    {
        return;
    }

    if (cw->gl_shadow_texture != 0)
    {
        glDeleteTextures (1, &cw->gl_shadow_texture);
        cw->gl_shadow_texture = 0;
    }
    cw->shadow_width = 0;
    cw->shadow_height = 0;
    cw->gl_shadow_opacity = 0.0f;
}

void
xfwmGLUpdateWindowShadow (CWindow *cw, gdouble opacity, gint width, gint height)
{
    ScreenInfo *screen_info;
    XfwmGLData *data;
    XImage *image;
    gint gaussian_size, shadow_width = 0, shadow_height = 0;

    g_return_if_fail (cw != NULL);

    screen_info = cw->screen_info;
    data = gl_data (screen_info);
    g_return_if_fail (data != NULL);
    g_return_if_fail (screen_info->gaussianMap != NULL);
    if (!gl_context_is_current (screen_info))
    {
        return;
    }

    xfwmGLFreeWindowShadow (cw);

    gaussian_size = screen_info->gaussianMap->size;
    shadow_size (screen_info, width, height, &shadow_width, &shadow_height);

    /*
     * The profile only holds for windows wider and taller than the blur, the
     * gaussian of a narrow box never saturates. Small windows keep the shadow
     * the XRender path builds, they are cheap anyway.
     */
    if ((data->program_shadow_profile != 0) &&
        (shadow_width >= 2 * gaussian_size) &&
        (shadow_height >= 2 * gaussian_size) &&
        build_shadow_profile (screen_info))
    {
        cw->shadow_width = shadow_width;
        cw->shadow_height = shadow_height;
        cw->gl_shadow_opacity = (gfloat) opacity * data->shadow_profile_peak;

        return;
    }

    image = make_shadow (screen_info, opacity, width, height);
    if (image == NULL)
    {
        return;
    }

    glGenTextures (1, &cw->gl_shadow_texture);
    glBindTexture (GL_TEXTURE_2D, cw->gl_shadow_texture);
    set_tex_params (GL_TEXTURE_2D, GL_LINEAR);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    /* One byte per pixel, so the stride in bytes is also the stride in pixels */
    glPixelStorei (GL_UNPACK_ROW_LENGTH, image->bytes_per_line);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_ALPHA,
                  image->width, image->height, 0,
                  GL_ALPHA, GL_UNSIGNED_BYTE, image->data);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture (GL_TEXTURE_2D, 0);

    cw->shadow_width = image->width;
    cw->shadow_height = image->height;
    XDestroyImage (image);
}

/*
 * Build the edge profile from a reference shadow of a large box, so the shape
 * comes from the very same gaussian tables the XRender path uses.
 */
static gboolean
build_shadow_profile (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    XImage *image;
    guchar *profile;
    guchar peak;
    gint gaussian_size, box, row, i;

    if (data->shadow_profile != 0)
    {
        return TRUE;
    }

    g_return_val_if_fail (screen_info->gaussianMap != NULL, FALSE);

    gaussian_size = screen_info->gaussianMap->size;
    if (gaussian_size < 2)
    {
        return FALSE;
    }

    /* A box far wider than the blur, so the middle of the profile saturates */
    box = 4 * gaussian_size;
    image = make_shadow (screen_info, 1.0, box, box);
    if (image == NULL)
    {
        return FALSE;
    }
    if (image->width < 2 * gaussian_size || image->height < 1)
    {
        XDestroyImage (image);
        return FALSE;
    }

    row = image->height / 2;
    peak = (guchar) image->data[row * image->bytes_per_line + image->width / 2];
    if (peak == 0)
    {
        XDestroyImage (image);
        return FALSE;
    }

    profile = g_malloc (gaussian_size);
    for (i = 0; i < gaussian_size; i++)
    {
        gint v = (guchar) image->data[row * image->bytes_per_line + i] * 255 / peak;

        profile[i] = (guchar) MIN (v, 255);
    }

    glGenTextures (1, &data->shadow_profile);
    glBindTexture (GL_TEXTURE_2D, data->shadow_profile);
    set_tex_params (GL_TEXTURE_2D, GL_LINEAR);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_ALPHA, gaussian_size, 1, 0,
                  GL_ALPHA, GL_UNSIGNED_BYTE, profile);
    glBindTexture (GL_TEXTURE_2D, 0);

    data->shadow_profile_peak = (gfloat) peak / 255.0f;

    /*
     * This can happen in the middle of a frame, so put back whatever program
     * was in use once the ramp is set.
     */
    {
        GLint current = 0;

        glGetIntegerv (GL_CURRENT_PROGRAM, &current);
        glUseProgram (data->program_shadow_profile);
        glUniform1f (data->u_prof_ramp, (gfloat) gaussian_size);
        glUseProgram ((GLuint) current);
    }

    g_free (profile);
    XDestroyImage (image);

    DBG ("Shadow profile built, %i samples, peak %.3f",
         gaussian_size, data->shadow_profile_peak);

    return TRUE;
}

/*
 * Nothing is going to change this window's mind about its depth, so skipping
 * it would leave a hole in the screen for good. Hand the whole screen to
 * XRender instead, which can draw any of them.
 */
static void
give_up_on_depth (XfwmGLData *data, gint depth)
{
    if (!data->give_up)
    {
        g_warning ("A window of depth %i cannot be bound as a texture, "
                   "falling back to XRender.", depth);
        data->give_up = TRUE;
    }
}

/*
 * Binding the image is what makes the X server hand out the storage behind the
 * pixmap, and the drawing that fills it can still be queued on the GPU when the
 * bind returns. Nothing on the GL side waits for that, so the first frame of a
 * new binding samples storage the server has not finished with, and the window
 * comes out black whatever the pixmap holds.
 *
 * Reading one pixel back is a round trip the server can only answer once that
 * drawing has really landed, which is the ordering that is missing. XSync() will
 * not do: it waits for the requests to be processed, and processing one only
 * queues the work behind it.
 *
 * Measured on radeonsi, 60 resizes each: nothing here leaves 1 to 5 black
 * frames, XSync() 4 to 8, glXWaitX() 8 to 13, reading the pixmap before the bind
 * 7 to 20, and reading it after the bind none in 480. So it is not the delay of
 * the round trip that fixes it, and it has to come after the bind.
 *
 * Asking the image to preserve the pixmap's content, which it now does, is a
 * different thing and does not replace this: with the attribute in place and
 * this taken out, 240 resizes leave no black frames at all but ten of them
 * show a band of old content along the edge the resize just exposed. The
 * attribute says what the image starts out holding; only the round trip says
 * the server has finished drawing it.
 */
static void
wait_for_pixmap (DisplayInfo *display_info, Pixmap pixmap)
{
    XImage *image;

    myDisplayErrorTrapPush (display_info);
    image = XGetImage (display_info->dpy, pixmap,
                       0, 0, 1, 1, AllPlanes, ZPixmap);
    if (image != NULL)
    {
        XDestroyImage (image);
    }
    myDisplayErrorTrapPopIgnored (display_info);
}

static gboolean
bind_window_texture (CWindow *cw)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);
    DisplayInfo *display_info = screen_info->display_info;
    Display *dpy = myScreenGetXDisplay (screen_info);
    XfwmGLDepth *dc;
    gboolean new_pixmap = FALSE;

    if (ensure_name_window_pixmap (cw) == None)
    {
        return FALSE;
    }

    if (screen_info->use_egl_backend)
    {
        if (cw->egl_image == NULL)
        {
            cw->egl_image =
                eglCreateImageKHR (data->egl_display, EGL_NO_CONTEXT,
                                   EGL_NATIVE_PIXMAP_KHR,
                                   (EGLClientBuffer) (guintptr)
                                   cw->name_window_pixmap, preserved_image);
            if (cw->egl_image == NULL)
            {
                /* Refusals go away, depths do not: treat it as the former */
                data->retry_paint = TRUE;

                return FALSE;
            }
            new_pixmap = TRUE;
        }
    }
    else
    {
        dc = depth_config (screen_info, cw->attr.depth);
        if (dc == NULL || !dc->usable)
        {
            give_up_on_depth (data, cw->attr.depth);

            return FALSE;
        }

        if (cw->gl_pixmap == None)
        {
            const gint attribs[] = {
                GLX_TEXTURE_TARGET_EXT, (gint) data->tex_target,
                GLX_TEXTURE_FORMAT_EXT, (cw->attr.depth == 32)
                                         ? GLX_TEXTURE_FORMAT_RGBA_EXT
                                         : GLX_TEXTURE_FORMAT_RGB_EXT,
                None
            };

            myDisplayErrorTrapPush (display_info);
            cw->gl_pixmap = glXCreatePixmap (dpy, dc->fbconfig,
                                             cw->name_window_pixmap, attribs);
            if (myDisplayErrorTrapPop (display_info) != Success)
            {
                cw->gl_pixmap = None;
            }
            if (cw->gl_pixmap == None)
            {
                /*
                 * The depth is one the driver said it could bind, so this is
                 * not a window of a kind we cannot draw but a refusal of this
                 * one pixmap, for want of video memory for instance. Those go
                 * away, so the frame is dropped and painted again rather than
                 * handing the screen to XRender for the rest of the session.
                 * See xfwmGLPaintAll().
                 */
                data->retry_paint = TRUE;

                return FALSE;
            }
            new_pixmap = TRUE;
        }
    }

    if (cw->gl_texture == 0)
    {
        glGenTextures (1, &cw->gl_texture);
        glBindTexture (data->tex_type, cw->gl_texture);
        set_tex_params (data->tex_type, GL_NEAREST);
    }
    else
    {
        glBindTexture (data->tex_type, cw->gl_texture);
    }

    /*
     * The two backends need very different amounts of work here. An EGL image
     * shares the pixmap's storage, so what the window draws is visible through
     * the texture without doing anything: the image only has to be targeted
     * when the pixmap itself is new. The ordering against the server that the
     * shared storage does not give is one eglWaitNative() per frame, in
     * xfwmGLPaintAll(). A GLX texture only guarantees fresh content across a
     * release and re-bind, so that pair runs whenever the window has actually
     * drawn something, which is what repair_win() records; a window repainted
     * merely because a neighbour changed still holds what it held at the last
     * bind.
     */
    if (!cw->gl_texture_bound || cw->gl_content_dirty)
    {
        gdouble prof_at = data->profile ? thread_cpu_ms () : 0.0;

        if (screen_info->use_egl_backend)
        {
            if (!cw->gl_texture_bound)
            {
                glEGLImageTargetTexture2DOES (GL_TEXTURE_2D, cw->egl_image);
            }
        }
        else
        {
            if (cw->gl_texture_bound)
            {
                glXReleaseTexImageEXT (dpy, cw->gl_pixmap, GLX_FRONT_EXT);
                cw->gl_texture_bound = FALSE;
            }
            glXBindTexImageEXT (dpy, cw->gl_pixmap, GLX_FRONT_EXT, NULL);
        }
        cw->gl_texture_bound = TRUE;

        if (data->profile)
        {
            data->prof_bind += thread_cpu_ms () - prof_at;
            data->prof_binds += 1.0;
        }

        /*
         * Only the first bind of a GLX pixmap needs it: the storage is handed
         * out once, and every later bind of the same one gets what the server
         * has already finished with. A window is given a new pixmap on every
         * resize, so this is once a frame while one is being resized, and never
         * for a window that is only moving or redrawing.
         */
        if (new_pixmap && data->wait_new_pixmap)
        {
            wait_for_pixmap (display_info, cw->name_window_pixmap);
        }
    }
    cw->gl_content_dirty = FALSE;

    return TRUE;
}

/*
 * Draw one textured quad, clipped to every rectangle of the region.
 * Source and destination are in screen pixels, the texture coordinates are
 * worked out from the size of the texture.
 *
 * Texture coordinates run downwards with the screen, which is how a pixmap is
 * laid out. GLX_Y_INVERTED_EXT is not consulted: every driver we can test on
 * answers "do not care" for it, so honouring it would only ever be guesswork,
 * and upstream xfwm4 leaves it alone for the same reason.
 */
static void
draw_quad (ScreenInfo *screen_info, GLenum tex_type,
           gint sx, gint sy, gint tex_width, gint tex_height,
           gint dx, gint dy, gint width, gint height,
           cairo_region_t *clip)
{
    XfwmGLData *data = gl_data (screen_info);
    gint nrects, i;

    if (width <= 0 || height <= 0 || tex_width <= 0 || tex_height <= 0)
    {
        return;
    }

    nrects = cairo_region_num_rectangles (clip);
    if (nrects == 0)
    {
        return;
    }

    if (data->profile)
    {
        data->prof_quads += 1.0;
        data->prof_rects += nrects;
    }
    /* The measurement mode, see no_paint. Counted, just not drawn. */
    if (data->no_paint)
    {
        return;
    }

    glBegin (GL_QUADS);
    for (i = 0; i < nrects; i++)
    {
        cairo_rectangle_int_t r;
        gint x1, y1, x2, y2;
        gfloat vx1, vy1, vx2, vy2;
        gfloat u1, v1, u2, v2;

        cairo_region_get_rectangle (clip, i, &r);

        /* Clip the quad to the rectangle, skip it when nothing is left */
        x1 = MAX (dx, r.x);
        y1 = MAX (dy, r.y);
        x2 = MIN (dx + width, r.x + r.width);
        y2 = MIN (dy + height, r.y + r.height);
        if (x1 >= x2 || y1 >= y2)
        {
            continue;
        }

        vx1 = 2.0f * (gfloat) x1 / (gfloat) screen_info->width - 1.0f;
        vx2 = 2.0f * (gfloat) x2 / (gfloat) screen_info->width - 1.0f;
        vy1 = 1.0f - 2.0f * (gfloat) y1 / (gfloat) screen_info->height;
        vy2 = 1.0f - 2.0f * (gfloat) y2 / (gfloat) screen_info->height;

        /* The texture follows the same clipping, in texture coordinates */
        u1 = (gfloat) (sx + x1 - dx);
        u2 = (gfloat) (sx + x2 - dx);
        v1 = (gfloat) (sy + y1 - dy);
        v2 = (gfloat) (sy + y2 - dy);
        if (tex_type != GL_TEXTURE_RECTANGLE_ARB)
        {
            u1 /= (gfloat) tex_width;
            u2 /= (gfloat) tex_width;
            v1 /= (gfloat) tex_height;
            v2 /= (gfloat) tex_height;
        }

        glTexCoord2f (u1, v1);
        glVertex2f (vx1, vy1);
        glTexCoord2f (u2, v1);
        glVertex2f (vx2, vy1);
        glTexCoord2f (u2, v2);
        glVertex2f (vx2, vy2);
        glTexCoord2f (u1, v2);
        glVertex2f (vx1, vy2);
    }
    glEnd ();
}

/*
 * A uniform belongs to one program, so the program and its opacity are always
 * set together. Setting one without the other writes to the wrong program and
 * GL says nothing about it. Every draw site calls this right before drawing,
 * so nothing relies on which program was left current.
 */
static void
use_program (GLuint program, GLint u_opacity, gfloat opacity)
{
    glUseProgram (program);
    glUniform1f (u_opacity, opacity);
}

static void
draw_window_part (CWindow *cw, gint sx, gint sy, gint dx, gint dy,
                  gint width, gint height, cairo_region_t *clip)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);
    gint tex_width, tex_height;

    get_window_pixmap_size (cw, &tex_width, &tex_height);

    draw_quad (screen_info, data->tex_type,
               sx, sy, tex_width, tex_height,
               dx, dy, width, height, clip);
}

/*
 * Paint a window, either its opaque part with blending off, or the whole
 * window blended. Mirrors paint_win() of the XRender path, including the
 * frame drawn separately when the title bar is translucent.
 */
static gboolean
paint_window_gl (CWindow *cw, gboolean solid_part, cairo_region_t *clip)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);
    gfloat opacity;

    if (!bind_window_texture (cw))
    {
        return FALSE;
    }

    opacity = solid_part ? 1.0f : (gfloat) cw->opacity / (gfloat) NET_WM_OPAQUE;

    if (WIN_HAS_TRANSLUCENT_FRAME(cw))
    {
        gint frame_top, frame_bottom, frame_left, frame_right;
        gint frame_width, frame_height, pixmap_width, pixmap_height;

        /*
         * The size the window is really drawn at, which during a resize is
         * not the size its attributes claim: the frame quads would sample
         * far past the edge of a pixmap that is still the old one, and paint
         * a band of stretched title bar down the side of the window the full
         * height of it. See window_painted_size().
         */
        window_painted_size (cw, &pixmap_width, &pixmap_height);
        /* That size counts the border twice over, these do not */
        frame_width = pixmap_width - 2 * cw->attr.border_width;
        frame_height = pixmap_height - 2 * cw->attr.border_width;
        frame_top = frameTop (cw->c);
        frame_bottom = frameBottom (cw->c);
        frame_left = frameLeft (cw->c);
        frame_right = frameRight (cw->c);

        /* The frame is only painted in the blended pass, never as a solid */
        if (!solid_part)
        {
            use_program (data->program_win, data->u_opacity_win,
                         opacity * (gfloat) screen_info->params->frame_opacity / 100.0f);

            /* Top border, the title bar */
            draw_window_part (cw, 0, 0, cw->attr.x, cw->attr.y,
                              frame_width, frame_top, clip);
            /* Bottom border */
            draw_window_part (cw, 0, frame_height - frame_bottom,
                              cw->attr.x, cw->attr.y + frame_height - frame_bottom,
                              frame_width, frame_bottom, clip);
            /* Left border */
            draw_window_part (cw, 0, frame_top,
                              cw->attr.x, cw->attr.y + frame_top,
                              frame_left, frame_height - frame_top - frame_bottom,
                              clip);
            /* Right border */
            draw_window_part (cw, frame_width - frame_right, frame_top,
                              cw->attr.x + frame_width - frame_right,
                              cw->attr.y + frame_top,
                              frame_right, frame_height - frame_top - frame_bottom,
                              clip);
        }

        use_program (data->program_win, data->u_opacity_win, opacity);
        draw_window_part (cw, frame_left, frame_top,
                          cw->attr.x + frame_left, cw->attr.y + frame_top,
                          frame_width - frame_left - frame_right,
                          frame_height - frame_top - frame_bottom,
                          clip);
    }
    else
    {
        gint width, height;

        window_painted_size (cw, &width, &height);
        use_program (data->program_win, data->u_opacity_win, opacity);
        draw_window_part (cw, 0, 0, cw->attr.x, cw->attr.y, width, height,
                          clip);
    }

    return TRUE;
}

static void
paint_shadow_gl (CWindow *cw, cairo_region_t *clip)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);

    if (cw->shadow_width <= 0 || cairo_region_is_empty (clip))
    {
        return;
    }

    glEnable (GL_BLEND);

    if (cw->gl_shadow_texture == 0)
    {
        use_program (data->program_shadow_profile, data->u_prof_opacity,
                     cw->gl_shadow_opacity);
        glUniform2f (data->u_prof_size,
                     (gfloat) cw->shadow_width, (gfloat) cw->shadow_height);
        glBindTexture (GL_TEXTURE_2D, data->shadow_profile);
    }
    else
    {
        /* The opacity is already baked into the image we uploaded */
        use_program (data->program_2d, data->u_opacity_2d, 1.0f);
        glBindTexture (GL_TEXTURE_2D, cw->gl_shadow_texture);
    }

    draw_quad (screen_info, GL_TEXTURE_2D,
               0, 0, cw->shadow_width, cw->shadow_height,
               cw->attr.x + cw->shadow_dx, cw->attr.y + cw->shadow_dy,
               cw->shadow_width, cw->shadow_height, clip);

    glBindTexture (GL_TEXTURE_2D, 0);
}

/*
 * Bind the root pixmap as a texture so the desktop background can be drawn
 * where no window covers it. Falls back to black.
 */
static gboolean
bind_root_texture (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    DisplayInfo *display_info = screen_info->display_info;
    Display *dpy = myScreenGetXDisplay (screen_info);
    XfwmGLDepth *dc;
    GLXFBConfig fbconfig;
    GLenum target;
    Pixmap pixmap;
    Window root_ret;
    gint x_ret, y_ret;
    gint tiles_x, tiles_y;
    guint width_ret, height_ret, border_ret, depth_ret;
    gint attribs[] = {
        GLX_TEXTURE_TARGET_EXT, 0,
        GLX_TEXTURE_FORMAT_EXT, 0,
        None
    };

    if (data->root_texture != 0)
    {
        glBindTexture (data->root_tex_type, data->root_texture);
        /*
         * Only when the desktop drew into the same pixmap again. Without a
         * damage handle there is nothing to say when that happened, so the
         * image is taken again on every frame as it used to be.
         */
        if (data->root_dirty || data->root_damage == None)
        {
            if (data->root_glx_pixmap != None)
            {
                glXReleaseTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT);
                glXBindTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT, NULL);
                data->root_dirty = FALSE;
            }
            else if (data->root_egl_image != NULL)
            {
                /* Ordered by the per-frame eglWaitNative() in xfwmGLPaintAll() */
                glEGLImageTargetTexture2DOES (GL_TEXTURE_2D,
                                              data->root_egl_image);
                data->root_dirty = FALSE;
            }
        }

        return TRUE;
    }

    if (data->root_missing)
    {
        return FALSE;
    }

    pixmap = root_background_pixmap (screen_info);
    if (pixmap == None)
    {
        /* Nothing advertises a background, do not ask again every frame */
        data->root_missing = TRUE;

        return FALSE;
    }

    /*
     * Every give up below latches root_missing as well. Retrying a background
     * that cannot be bound would cost two blocking questions to the X server
     * on every single frame for the rest of the session.
     */
    myDisplayErrorTrapPush (display_info);
    if (!XGetGeometry (dpy, pixmap, &root_ret, &x_ret, &y_ret,
                       &width_ret, &height_ret, &border_ret, &depth_ret))
    {
        myDisplayErrorTrapPopIgnored (display_info);
        data->root_missing = TRUE;

        return FALSE;
    }
    myDisplayErrorTrapPopIgnored (display_info);

    fbconfig = NULL;
    target = data->tex_target;
    data->root_tex_type = data->tex_type;
    data->root_repeat = FALSE;

    if (!screen_info->use_egl_backend)
    {
        /* The background is not always as deep as the screen, so ask the pixmap */
        dc = depth_config (screen_info, (gint) depth_ret);
        if (dc == NULL || !dc->usable)
        {
            data->root_missing = TRUE;

            return FALSE;
        }
        fbconfig = dc->fbconfig;
    }

    /*
     * A background pixmap smaller than the screen is tiled by the X server, so
     * it is tiled here too rather than stretched. GL repeats a texture by
     * itself, one quad for the whole desktop, but only on the 2D target: so a
     * pattern is bound there, and drawn tile by tile only where it is missing.
     */
    if (((gint) width_ret < screen_info->width) ||
        ((gint) height_ret < screen_info->height))
    {
        GLXFBConfig fbconfig_2d;

        if (data->tex_target == GLX_TEXTURE_2D_EXT)
        {
            data->root_repeat = TRUE;
        }
        else if (!screen_info->use_egl_backend &&
                 find_fbconfig (screen_info, (gint) depth_ret,
                                GLX_TEXTURE_2D_EXT, &fbconfig_2d))
        {
            fbconfig = fbconfig_2d;
            target = GLX_TEXTURE_2D_EXT;
            data->root_tex_type = GL_TEXTURE_2D;
            data->root_repeat = TRUE;
        }
    }

    /*
     * A pattern needing thousands of tiles would cost more than the frame is
     * worth, and a stray tiny pixmap must not stall the compositor. Tiles are
     * counted rather than areas compared: a wide short pattern needs many tiles
     * for a small area, and an area in pixels overflows on a large screen.
     */
    tiles_x = screen_info->width / (gint) width_ret + 2;
    tiles_y = screen_info->height / (gint) height_ret + 2;
    if (!data->root_repeat &&
        (tiles_x > GL_MAX_ROOT_TILES || tiles_y > GL_MAX_ROOT_TILES ||
         tiles_x * tiles_y > GL_MAX_ROOT_TILES))
    {
        g_warning ("The desktop background is a %ux%u tile and this driver "
                   "cannot repeat it, so the desktop is drawn black.",
                   width_ret, height_ret);
        data->root_missing = TRUE;

        return FALSE;
    }

    data->root_width = (gint) width_ret;
    data->root_height = (gint) height_ret;

    if (screen_info->use_egl_backend)
    {
        data->root_egl_image =
            eglCreateImageKHR (data->egl_display, EGL_NO_CONTEXT,
                               EGL_NATIVE_PIXMAP_KHR,
                               (EGLClientBuffer) (guintptr) pixmap,
                               preserved_image);
        if (data->root_egl_image == NULL)
        {
            data->root_missing = TRUE;

            return FALSE;
        }
    }
    else
    {
        attribs[1] = (gint) target;
        attribs[3] = (depth_ret == 32) ? GLX_TEXTURE_FORMAT_RGBA_EXT
                                       : GLX_TEXTURE_FORMAT_RGB_EXT;

        myDisplayErrorTrapPush (display_info);
        data->root_glx_pixmap = glXCreatePixmap (dpy, fbconfig, pixmap, attribs);
        if (myDisplayErrorTrapPop (display_info) != Success)
        {
            data->root_glx_pixmap = None;
        }
        if (data->root_glx_pixmap == None)
        {
            data->root_missing = TRUE;

            return FALSE;
        }
    }

    glGenTextures (1, &data->root_texture);
    glBindTexture (data->root_tex_type, data->root_texture);
    set_tex_params (data->root_tex_type, GL_NEAREST);
    if (data->root_repeat)
    {
        glTexParameteri (data->root_tex_type, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri (data->root_tex_type, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    if (screen_info->use_egl_backend)
    {
        /* Ordered by the per-frame eglWaitNative() in xfwmGLPaintAll() */
        glEGLImageTargetTexture2DOES (GL_TEXTURE_2D, data->root_egl_image);
    }
    else
    {
        glXBindTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT, NULL);
    }
    /* The background is handed out the same way a window is, see the note there */
    wait_for_pixmap (display_info, pixmap);

    /* Watch for a desktop that repaints in place, see xfwmGLDamageRootPixmap() */
    data->root_pixmap = pixmap;
    data->root_dirty = FALSE;
    myDisplayErrorTrapPush (display_info);
    data->root_damage = XDamageCreate (dpy, pixmap, XDamageReportNonEmpty);
    if (myDisplayErrorTrapPop (display_info) != Success)
    {
        data->root_damage = None;
    }

    return TRUE;
}

static void
paint_root_gl (ScreenInfo *screen_info, cairo_region_t *clip)
{
    XfwmGLData *data = gl_data (screen_info);

    if (cairo_region_is_empty (clip))
    {
        return;
    }

    glDisable (GL_BLEND);

    if (bind_root_texture (screen_info))
    {
        gint tex_width = data->root_width;
        gint tex_height = data->root_height;

        if (data->root_repeat)
        {
            /*
             * One quad for the whole screen, the texture repeats itself. The
             * pattern is on the 2D target here, see bind_root_texture(), so it
             * takes the plain 2D program rather than the one for windows.
             */
            use_program (data->program_2d, data->u_opacity_2d, 1.0f);
            draw_quad (screen_info, GL_TEXTURE_2D,
                       0, 0, tex_width, tex_height,
                       0, 0, screen_info->width, screen_info->height, clip);
        }
        else
        {
            cairo_rectangle_int_t area;
            gint x, y, first_x, first_y;

            use_program (data->program_win, data->u_opacity_win, 1.0f);
            /*
             * The background covers the screen, or the driver cannot repeat it
             * and it has to be laid down one tile at a time. Usually that is
             * one single tile, and only the tiles the repaint can touch are
             * drawn.
             */
            cairo_region_get_extents (clip, &area);
            first_x = (area.x / tex_width) * tex_width;
            first_y = (area.y / tex_height) * tex_height;

            for (y = first_y; y < area.y + area.height; y += tex_height)
            {
                for (x = first_x; x < area.x + area.width; x += tex_width)
                {
                    draw_quad (screen_info, data->root_tex_type,
                               0, 0, tex_width, tex_height,
                               x, y, tex_width, tex_height, clip);
                }
            }
        }
    }
    else
    {
        /* No background pixmap, plain black like the XRender path */
        use_program (data->program_2d, data->u_opacity_2d, 1.0f);
        glBindTexture (GL_TEXTURE_2D, data->black_texture);
        draw_quad (screen_info, GL_TEXTURE_2D, 0, 0, 1, 1,
                   0, 0, screen_info->width, screen_info->height, clip);
        glBindTexture (GL_TEXTURE_2D, 0);
    }
}

static void
paint_cursor_gl (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    cairo_rectangle_int_t rect;
    cairo_region_t *clip;

    if (screen_info->cursorSerial == 0)
    {
        return;
    }

    if (data->cursor_texture == 0 || data->cursor_serial != screen_info->cursorSerial)
    {
        XFixesCursorImage *cursor;
        guint32 *pixels;
        gboolean same_size;

        cursor = XFixesGetCursorImage (myScreenGetXDisplay (screen_info));
        if (cursor == NULL)
        {
            return;
        }

        pixels = cursor_pixels_to_argb32 (cursor);

        same_size = (data->cursor_texture != 0 &&
                     data->cursor_width == cursor->width &&
                     data->cursor_height == cursor->height);
        if (data->cursor_texture == 0)
        {
            glGenTextures (1, &data->cursor_texture);
        }
        glBindTexture (GL_TEXTURE_2D, data->cursor_texture);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
        if (same_size)
        {
            /* An animated cursor changes image but not size, keep the storage */
            glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0,
                             cursor->width, cursor->height,
                             GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        }
        else
        {
            set_tex_params (GL_TEXTURE_2D, GL_LINEAR);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, cursor->width, cursor->height,
                          0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
        }
        g_free (pixels);

        data->cursor_width = cursor->width;
        data->cursor_height = cursor->height;
        data->cursor_serial = screen_info->cursorSerial;
        XFree (cursor);
    }
    else
    {
        glBindTexture (GL_TEXTURE_2D, data->cursor_texture);
    }

    /* draw_quad() wants a clip, the cursor's own destination clips nothing away */
    rect.x = screen_info->cursorLocation.x;
    rect.y = screen_info->cursorLocation.y;
    rect.width = screen_info->cursorLocation.width;
    rect.height = screen_info->cursorLocation.height;
    clip = cairo_region_create_rectangle (&rect);

    glEnable (GL_BLEND);
    use_program (data->program_2d, data->u_opacity_2d, 1.0f);
    draw_quad (screen_info, GL_TEXTURE_2D,
               0, 0, data->cursor_width, data->cursor_height,
               screen_info->cursorLocation.x, screen_info->cursorLocation.y,
               screen_info->cursorLocation.width, screen_info->cursorLocation.height,
               clip);
    cairo_region_destroy (clip);
}

/*
 * When the magnifier is on the scene is drawn to a texture first, then that
 * texture is drawn back magnified.
 */
static gboolean
bind_zoom_fbo (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    /* Asking again every frame would build and drop a screen sized texture */
    if (data->fbo_failed)
    {
        return FALSE;
    }

    if (data->fbo_width != screen_info->width ||
        data->fbo_height != screen_info->height)
    {
        free_fbo (screen_info);
    }

    if (data->fbo == 0)
    {
        glGenFramebuffers (1, &data->fbo);
        glGenTextures (1, &data->fbo_texture);
        glBindTexture (GL_TEXTURE_2D, data->fbo_texture);
        set_tex_params (GL_TEXTURE_2D, GL_LINEAR);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA,
                      screen_info->width, screen_info->height, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        data->fbo_width = screen_info->width;
        data->fbo_height = screen_info->height;

        glBindFramebuffer (GL_FRAMEBUFFER, data->fbo);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, data->fbo_texture, 0);
        if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            g_warning ("Incomplete frame buffer object, magnifier disabled.");
            glBindFramebuffer (GL_FRAMEBUFFER, 0);
            free_fbo (screen_info);
            data->fbo_failed = TRUE;

            return FALSE;
        }
    }
    else
    {
        glBindFramebuffer (GL_FRAMEBUFFER, data->fbo);
    }

    return TRUE;
}

static void
draw_zoomed_scene (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    gdouble zoom, x_offset, y_offset;
    GLenum filter;

    glBindFramebuffer (GL_FRAMEBUFFER, 0);

    zoom = XFixedToDouble (screen_info->transform.matrix[0][0]);
    if (zoom <= 0.0)
    {
        zoom = 1.0;
    }
    /*
     * The XRender transform maps destination to source, the offsets are
     * already in screen pixels.
     */
    x_offset = XFixedToDouble (screen_info->transform.matrix[0][2]);
    y_offset = XFixedToDouble (screen_info->transform.matrix[1][2]);

    filter = ZOOM_SMOOTHING_WANTED (zoom) ? GL_LINEAR : GL_NEAREST;

    glDisable (GL_BLEND);
    glBindTexture (GL_TEXTURE_2D, data->fbo_texture);
    /* The filter only changes when the zoom crosses a threshold */
    if (data->fbo_filter != filter)
    {
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint) filter);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint) filter);
        data->fbo_filter = filter;
    }
    use_program (data->program_2d, data->u_opacity_2d, 1.0f);

    {
        /* The scene texture has its origin at the bottom left */
        gfloat u1 = (gfloat) (x_offset / screen_info->width);
        gfloat u2 = u1 + (gfloat) zoom;
        gfloat v2 = 1.0f - (gfloat) (y_offset / screen_info->height);
        gfloat v1 = v2 - (gfloat) zoom;

        glBegin (GL_QUADS);
        glTexCoord2f (u1, v2);
        glVertex2f (-1.0f, 1.0f);
        glTexCoord2f (u2, v2);
        glVertex2f (1.0f, 1.0f);
        glTexCoord2f (u2, v1);
        glVertex2f (1.0f, -1.0f);
        glTexCoord2f (u1, v1);
        glVertex2f (-1.0f, -1.0f);
        glEnd ();
    }
}

/*
 * Turn the damage the X server gave us into a client side region. This is the
 * one and only region that has to cross the wire each frame.
 */
static cairo_region_t *
fetch_damage (Display *dpy, XserverRegion damage)
{
    cairo_region_t *region;
    XRectangle *rects;
    gint nrects = 0;

    rects = XFixesFetchRegion (dpy, damage, &nrects);
    if (rects == NULL)
    {
        return cairo_region_create ();
    }

    region = region_from_rects (rects, nrects, 0, 0);
    XFree (rects);

    return region;
}

/* The whole screen as a region, the shape of every full repaint */
static cairo_region_t *
screen_region (ScreenInfo *screen_info)
{
    cairo_rectangle_int_t r = { 0, 0, screen_info->width, screen_info->height };

    return cairo_region_create_rectangle (&r);
}

/*
 * Work out what has to be repainted this frame. With GLX_EXT_buffer_age the
 * damage of the last frames is replayed, otherwise the whole screen is
 * redrawn because the content of the back buffer is undefined after a swap.
 */
static cairo_region_t *
get_paint_region (ScreenInfo *screen_info, cairo_region_t *damage)
{
    XfwmGLData *data = gl_data (screen_info);
    Display *dpy = myScreenGetXDisplay (screen_info);
    cairo_region_t *region;
    guint age = 0;
    guint i;

    /*
     * The frame buffer object never loses its content, so it only ever owes
     * this frame's damage. The back buffer age says nothing about it.
     */
    if ((data->present_mode == GL_PRESENT_FBO) && !data->full_repaint)
    {
        return cairo_region_copy (damage);
    }

    /*
     * In copy mode the age stays at zero, a copy leaves the back buffer
     * undefined, so the query is not even asked and the whole screen paints.
     */
    if (((data->present_mode == GL_PRESENT_SWAP) ||
         (data->present_mode == GL_PRESENT_SCENE)) &&
        data->has_buffer_age && !data->full_repaint)
    {
        if (screen_info->use_egl_backend)
        {
            EGLint egl_age = 0;

            eglQuerySurface (data->egl_display, data->egl_surface,
                             EGL_BUFFER_AGE_EXT, &egl_age);
            age = (egl_age > 0) ? (guint) egl_age : 0;
        }
        else
        {
            glXQueryDrawable (dpy, screen_info->glx_window,
                              GLX_BACK_BUFFER_AGE_EXT, &age);
        }
    }

    if (data->profile)
    {
        data->prof_age_frames += 1.0;
        if (age > data->prof_age_max)
        {
            data->prof_age_max = age;
        }
        data->prof_age_hist[MIN (age, GL_PROF_AGE_BUCKETS - 1)] += 1.0;
        if (age > GL_DAMAGE_HISTORY)
        {
            /* The buffer is older than the history, so the screen is repainted */
            data->prof_age_over += 1.0;
        }
        else if (age == 0)
        {
            data->prof_full_repaint += 1.0;
        }
    }

    /* A full repaint leaves the age at zero, it never asks the driver */
    if (age == 0 || age > GL_DAMAGE_HISTORY)
    {
        region = screen_region (screen_info);
    }
    else
    {
        gboolean complete = TRUE;

        region = cairo_region_copy (damage);
        /* Add back what the older frames in the buffer never saw */
        for (i = 0; i < age - 1; i++)
        {
            guint slot = (data->damage_index + GL_DAMAGE_HISTORY - i - 1) % GL_DAMAGE_HISTORY;

            if (data->damage_history[slot] == NULL)
            {
                /*
                 * A frame that early in the session never recorded anything,
                 * so what this buffer is missing cannot be known. Skipping the
                 * slot would quietly paint too little and leave stale pixels,
                 * so the screen is painted whole instead.
                 */
                complete = FALSE;
                break;
            }
            cairo_region_union (region, data->damage_history[slot]);
        }

        if (!complete)
        {
            cairo_region_destroy (region);
            region = screen_region (screen_info);
        }
    }

    return region;
}

/*
 * Only frames that reach the screen may advance the history, otherwise the
 * buffer age of the next frames points at the wrong entries and areas keep
 * stale pixels. Takes the region over, the caller must not touch it again.
 */
static void
record_damage (ScreenInfo *screen_info, cairo_region_t *damage)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data->damage_history[data->damage_index] != NULL)
    {
        cairo_region_destroy (data->damage_history[data->damage_index]);
    }
    data->damage_history[data->damage_index] = damage;
    data->damage_index = (data->damage_index + 1) % GL_DAMAGE_HISTORY;
}

/*
 * Swap through EGL, handing the damage to the driver where it takes it. The
 * damage says what changed against the frame on the screen, so a frame that
 * had to be painted whole because its buffer held nothing, or whose visible
 * content moved for other reasons, the magnifier, presents whole.
 */
static void
egl_swap (ScreenInfo *screen_info, cairo_region_t *frame_damage,
          gboolean whole)
{
    XfwmGLData *data = gl_data (screen_info);
    EGLint rects[4 * GL_MAX_PRESENT_RECTS];
    gint i, nrects;

    nrects = (frame_damage != NULL)
             ? cairo_region_num_rectangles (frame_damage) : 0;
    if (whole || nrects == 0 || data->egl_swap_with_damage == NULL)
    {
        eglSwapBuffers (data->egl_display, data->egl_surface);

        return;
    }

    /* Too many pieces are handed to the driver as their bounding box */
    if (nrects > GL_MAX_PRESENT_RECTS)
    {
        cairo_rectangle_int_t r;

        cairo_region_get_extents (frame_damage, &r);
        rects[0] = r.x;
        /* EGL counts the damage from the bottom left */
        rects[1] = screen_info->height - r.y - r.height;
        rects[2] = r.width;
        rects[3] = r.height;
        nrects = 1;
    }
    else
    {
        for (i = 0; i < nrects; i++)
        {
            cairo_rectangle_int_t r;

            cairo_region_get_rectangle (frame_damage, i, &r);
            rects[i * 4 + 0] = r.x;
            /* EGL counts the damage from the bottom left */
            rects[i * 4 + 1] = screen_info->height - r.y - r.height;
            rects[i * 4 + 2] = r.width;
            rects[i * 4 + 3] = r.height;
        }
    }

    data->egl_swap_with_damage (data->egl_display, data->egl_surface,
                                rects, nrects);
}

/*
 * Whether the last frame was dropped because a window could not be bound. The
 * dropped frame took that repaint's damage with it, so the compositor has to be
 * told to ask for another one or nothing would ever paint the screen again.
 * Answering clears it.
 */
gboolean
xfwmGLTakeRetryPaint (ScreenInfo *screen_info)
{
    XfwmGLData *data;

    g_return_val_if_fail (screen_info != NULL, FALSE);

    data = gl_data (screen_info);
    if (data == NULL || !data->retry_paint)
    {
        return FALSE;
    }
    data->retry_paint = FALSE;

    return TRUE;
}

/*
 * This thread's own processor time, in milliseconds. Only for XFWM4_GL_PROFILE:
 * the driver runs a submission thread of its own, and the process wide clock
 * cannot tell its work from ours.
 */
static gdouble
thread_cpu_ms (void)
{
    struct timespec ts;

    if (clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
    {
        return 0.0;
    }

    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

#define PROF_MARK(field)                                        \
    G_STMT_START {                                              \
        if (data->profile)                                      \
        {                                                       \
            gdouble prof_now = thread_cpu_ms ();                \
            data->field += prof_now - prof_mark;                \
            prof_mark = prof_now;                               \
        }                                                       \
    } G_STMT_END

/*
 * Start timing the graphics card on this frame, and collect whatever earlier
 * frames have finished. Only for XFWM4_GL_PROFILE.
 */
static void
prof_gpu_begin (XfwmGLData *data)
{
    guint i;

    if (data->prof_query[0] == 0)
    {
        glGenQueries (GL_PROF_QUERIES, data->prof_query);
    }

    for (i = 0; i < GL_PROF_QUERIES; i++)
    {
        GLuint ready = GL_FALSE;

        if (!data->prof_query_busy[i])
        {
            continue;
        }
        glGetQueryObjectuiv (data->prof_query[i], GL_QUERY_RESULT_AVAILABLE,
                             &ready);
        if (ready)
        {
            GLuint64 ns = 0;

            glGetQueryObjectui64v (data->prof_query[i], GL_QUERY_RESULT, &ns);
            data->prof_gpu_ns += (gdouble) ns;
            data->prof_gpu_frames += 1.0;
            data->prof_query_busy[i] = FALSE;
        }
    }

    /* Every query still in flight means this frame simply goes unmeasured */
    data->prof_query_active = -1;
    for (i = 0; i < GL_PROF_QUERIES; i++)
    {
        guint slot = (data->prof_query_next + i) % GL_PROF_QUERIES;

        if (!data->prof_query_busy[slot])
        {
            data->prof_query_active = (gint) slot;
            data->prof_query_next = (slot + 1) % GL_PROF_QUERIES;
            glBeginQuery (GL_TIME_ELAPSED, data->prof_query[slot]);
            break;
        }
    }
}

static void
prof_gpu_end (XfwmGLData *data)
{
    if (data->prof_query_active >= 0)
    {
        glEndQuery (GL_TIME_ELAPSED);
        data->prof_query_busy[data->prof_query_active] = TRUE;
        data->prof_query_active = -1;
    }
}

void
xfwmGLNoteFenceWait (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data != NULL && data->profile)
    {
        data->prof_fence_waits += 1.0;
    }
}

/*
 * XFWM4_GL_STATS and XFWM4_GL_PROFILE: count this paint, and every 5 seconds
 * print what the counters say and start them over. Kept out of the paint
 * function, which only calls it when the stats were asked for.
 */
static void
stats_note_paint (ScreenInfo *screen_info, cairo_region_t *present_region,
                  gint64 prof_wall)
{
    XfwmGLData *data = gl_data (screen_info);
    gint64 t = g_get_monotonic_time ();

    if (data->profile)
    {
        data->prof_wall += (t - prof_wall) / 1000.0;
    }
    data->stat_frames++;
    if (present_region != NULL)
    {
        cairo_rectangle_int_t r;
        gint i;

        for (i = 0; i < cairo_region_num_rectangles (present_region); i++)
        {
            cairo_region_get_rectangle (present_region, i, &r);
            data->stat_pixels += (gdouble) r.width * r.height;
        }
    }
    else
    {
        data->stat_pixels += (gdouble) screen_info->width * screen_info->height;
    }

    if (t - data->stat_since >= G_USEC_PER_SEC * 5)
    {
        gdouble s = (t - data->stat_since) / 1e6;

        g_message ("paints %.1f/s, %.1f Mpix/s presented, "
                   "%.1f Mpix/s painted for %.1f Mpix/s damaged",
                   data->stat_frames / s, data->stat_pixels / s / 1e6,
                   data->stat_paint_pixels / s / 1e6,
                   data->stat_damage_pixels / s / 1e6);
        if (data->profile && data->stat_frames > 0)
        {
            gdouble n = data->stat_frames;
            gdouble cpu = data->prof_damage + data->prof_pass1 +
                          data->prof_root + data->prof_pass2 +
                          data->prof_present;

            g_message ("  per paint (ms): damage %.3f  windows %.3f  "
                       "root %.3f  blended %.3f  present %.3f  "
                       "= %.3f cpu, %.3f wall",
                       data->prof_damage / n, data->prof_pass1 / n,
                       data->prof_root / n, data->prof_pass2 / n,
                       data->prof_present / n, cpu / n,
                       data->prof_wall / n);
            g_message ("  per paint: %.1f quads over %.1f clip rects, "
                       "%.1f windows blended, %.1f shadows, "
                       "%.1f texture rebinds costing %.3f ms, "
                       "%.1f fence waits",
                       data->prof_quads / n, data->prof_rects / n,
                       data->prof_windows / n, data->prof_shadows / n,
                       data->prof_binds / n, data->prof_bind / n,
                       data->prof_fence_waits / n);
            {
                GString *h = g_string_new ("  age histogram:");
                gdouble an = (data->prof_age_frames > 0.0)
                             ? data->prof_age_frames : n;
                guint k;

                for (k = 0; k < GL_PROF_AGE_BUCKETS; k++)
                {
                    if (data->prof_age_hist[k] > 0.0)
                    {
                        g_string_append_printf (h, "  %u%s:%.0f%%", k,
                                                (k == GL_PROF_AGE_BUCKETS - 1) ? "+" : "",
                                                data->prof_age_hist[k] / an * 100.0);
                    }
                    data->prof_age_hist[k] = 0.0;
                }
                g_message ("%s", h->str);
                g_string_free (h, TRUE);
            }
            g_message ("  buffer age: %.1f%% of paints older than the "
                       "%d frame history and so repainted whole, "
                       "%.1f%% owed everything anyway, deepest age %u",
                       data->prof_age_over /
                       ((data->prof_age_frames > 0.0) ? data->prof_age_frames : n)
                       * 100.0,
                       GL_DAMAGE_HISTORY,
                       data->prof_full_repaint /
                       ((data->prof_age_frames > 0.0) ? data->prof_age_frames : n)
                       * 100.0,
                       data->prof_age_max);
            if (data->prof_gpu_frames > 0.0)
            {
                gdouble per = data->prof_gpu_ns / data->prof_gpu_frames;

                g_message ("  GPU spends %.3f ms drawing each paint, "
                           "%.2f%% of the card over %.0f measured frames",
                           per / 1e6,
                           data->prof_gpu_ns / 1e6 / (s * 1000.0) * 100.0,
                           data->prof_gpu_frames);
                data->prof_gpu_ns = data->prof_gpu_frames = 0.0;
            }
            data->prof_damage = data->prof_pass1 = data->prof_root =
                data->prof_pass2 = data->prof_present =
                data->prof_wall = data->prof_quads = data->prof_rects =
                data->prof_windows = data->prof_shadows =
                data->prof_bind = data->prof_binds =
                data->prof_fence_waits = data->prof_age_over =
                data->prof_age_frames = data->prof_full_repaint = 0.0;
        }
        data->stat_frames = 0;
        data->stat_pixels = 0.0;
        data->stat_damage_pixels = 0.0;
        data->stat_paint_pixels = 0.0;
        data->stat_since = t;
    }
}

gboolean
xfwmGLPaintAll (ScreenInfo *screen_info, XserverRegion damage)
{
    gdouble prof_mark = 0.0;
    gint64 prof_wall = 0;
    XfwmGLData *data;
    DisplayInfo *display_info;
    Display *dpy;
    cairo_region_t *frame_damage;
    cairo_region_t *paint_region;
    cairo_region_t *present_region;
    cairo_region_t *clip;
    GList *list;
    CWindow *cw;
    gboolean zoomed;
    gboolean was_full_repaint;

    g_return_val_if_fail (screen_info != NULL, FALSE);
    TRACE ("entering");

    data = gl_data (screen_info);
    if (data == NULL)
    {
        return FALSE;
    }

    display_info = screen_info->display_info;
    dpy = myScreenGetXDisplay (screen_info);

    /*
     * Ours is nearly always current already, and a redundant MakeCurrent is
     * not free: the driver validates it and may flush. Both getters answer
     * on the client side.
     */
    if (screen_info->use_egl_backend)
    {
        if (!gl_context_is_current (screen_info) &&
            !eglMakeCurrent (data->egl_display, data->egl_surface,
                             data->egl_surface, data->egl_context))
        {
            g_warning ("Cannot make the GL context current, GL compositing disabled.");
            return FALSE;
        }
    }
    else if ((glXGetCurrentContext () != screen_info->glx_context ||
              glXGetCurrentDrawable () != screen_info->glx_window) &&
             !glXMakeCurrent (dpy, screen_info->glx_window, screen_info->glx_context))
    {
        g_warning ("Cannot make the GL context current, GL compositing disabled.");
        return FALSE;
    }

    myDisplayErrorTrapPush (display_info);

    if (data->profile)
    {
        prof_mark = thread_cpu_ms ();
        prof_wall = g_get_monotonic_time ();
    }

    /*
     * Without buffer age the history is never replayed and the whole screen
     * is painted anyway, see get_paint_region(), so the damage does not have
     * to cross the wire at all.
     */
    frame_damage = (data->has_buffer_age ||
                    data->present_mode != GL_PRESENT_SWAP ||
                    data->egl_swap_with_damage != NULL)
                   ? fetch_damage (dpy, damage) : NULL;
    paint_region = get_paint_region (screen_info, frame_damage);

    if (cairo_region_is_empty (paint_region))
    {
        /* Nothing reaches the screen, so nothing is recorded either */
        cairo_region_destroy (paint_region);
        if (frame_damage != NULL)
        {
            cairo_region_destroy (frame_damage);
        }
        myDisplayErrorTrapPopIgnored (display_info);

        return TRUE;
    }

    /*
     * What the front buffer is owed, taken before the passes eat the paint
     * region and before full_repaint is cleared. Copy mode paints the whole
     * screen but only the damage has to reach the front buffer; a full repaint
     * owes it everything, the front holds nothing usable either.
     */
    present_region = NULL;
    if ((data->present_mode == GL_PRESENT_FBO))
    {
        present_region = cairo_region_copy (paint_region);
    }
    else if (data->present_mode == GL_PRESENT_SCENE)
    {
        /*
         * Two different regions here, and that is the whole point of this
         * mode. The buffer about to be swapped in is a few frames old, so it
         * is owed everything that changed since, which is what the paint
         * region worked out from the buffer age: that much has to be blitted
         * into it out of the scene. The scene itself only ever loses the
         * pixels that changed this frame, so that is all that is composited.
         */
        present_region = paint_region;
        paint_region = (!data->full_repaint && frame_damage != NULL)
                       ? cairo_region_copy (frame_damage)
                       : screen_region (screen_info);
    }
    else if (data->present_mode == GL_PRESENT_COPY)
    {
        if (data->full_repaint)
        {
            present_region = screen_region (screen_info);
        }
        else
        {
            present_region = cairo_region_copy (frame_damage);
        }
    }

    was_full_repaint = data->full_repaint;
    data->full_repaint = FALSE;
    data->retry_paint = FALSE;

    if (data->stats)
    {
        gint i;

        for (i = 0; frame_damage != NULL &&
                    i < cairo_region_num_rectangles (frame_damage); i++)
        {
            cairo_rectangle_int_t r;

            cairo_region_get_rectangle (frame_damage, i, &r);
            data->stat_damage_pixels += (gdouble) r.width * r.height;
        }
        for (i = 0; i < cairo_region_num_rectangles (paint_region); i++)
        {
            cairo_rectangle_int_t r;

            cairo_region_get_rectangle (paint_region, i, &r);
            data->stat_paint_pixels += (gdouble) r.width * r.height;
        }
    }


    zoomed = screen_info->zoomed;
    if ((zoomed || (data->present_mode == GL_PRESENT_FBO) ||
         (data->present_mode == GL_PRESENT_SCENE)) &&
        !bind_zoom_fbo (screen_info))
    {
        zoomed = FALSE;
        if ((data->present_mode == GL_PRESENT_FBO) ||
            (data->present_mode == GL_PRESENT_SCENE))
        {
            /*
             * No frame buffer object, no experiment: back to swapping for the
             * rest of the session. This frame painted only the damage into a
             * back buffer that never got it, so it is dropped and asked again.
             */
            g_warning ("No frame buffer object, presenting with swaps instead.");
            data->present_mode = GL_PRESENT_SWAP;
            data->full_repaint = TRUE;
            data->retry_paint = TRUE;
            cairo_region_destroy (present_region);
            cairo_region_destroy (paint_region);
            if (frame_damage != NULL)
            {
                cairo_region_destroy (frame_damage);
            }
            myDisplayErrorTrapPopIgnored (display_info);

            return TRUE;
        }
    }
    if (!screen_info->zoomed)
    {
        /*
         * The magnifier holds a texture the size of the screen, so give it
         * back as soon as the magnifier is off. The XRender path frees its
         * own buffer the same way. The FBO experiment keeps the scene in that
         * texture, so there it stays.
         */
        if (data->fbo != 0 && (data->present_mode != GL_PRESENT_FBO) &&
            (data->present_mode != GL_PRESENT_SCENE))
        {
            free_fbo (screen_info);
        }
        free_cursor_texture (screen_info);
    }

    /*
     * The magnifier redraws the whole back buffer from the scene texture, so
     * the whole screen has to reach the front, whatever the damage was.
     */
    if (zoomed && present_region != NULL)
    {
        cairo_region_destroy (present_region);
        present_region = screen_region (screen_info);
    }

    PROF_MARK (prof_damage);

    if (screen_info->use_egl_backend)
    {
        /*
         * EGL orders nothing against the server by itself: without this,
         * NVIDIA samples window pixmaps the server is still rendering into,
         * and blocks of old content survive inside the new. Everything this
         * frame draws was rendered before the damage above was fetched, so
         * one wait here covers every window and the desktop background. A
         * frame that only samples content already waited for, a pure move
         * or a stacking change, skips it: the same flags tell the binds
         * below whether anything has to be taken again.
         */
        gboolean fresh = data->root_dirty ||
                         (!data->root_missing && data->root_texture == 0) ||
                         (data->root_egl_image != NULL &&
                          data->root_damage == None);

        for (list = screen_info->cwindows; !fresh && list;
             list = g_list_next (list))
        {
            cw = (CWindow *) list->data;
            fresh = cw->gl_content_dirty || !cw->gl_texture_bound;
        }
        if (fresh)
        {
            eglWaitNative (EGL_CORE_NATIVE_ENGINE);
        }
    }

    if (data->prof_gpu)
    {
        prof_gpu_begin (data);
    }

    glViewport (0, 0, screen_info->width, screen_info->height);
    glUseProgram (data->program_win);
    glActiveTexture (GL_TEXTURE0);

    /*
     * First pass, top to bottom: draw the opaque windows and take what they
     * cover out of the region left to paint.
     */
    for (list = screen_info->cwindows; list; list = g_list_next (list))
    {
        cairo_region_t *shape;
        gboolean opaque_window;

        cw = (CWindow *) list->data;

        /* Whatever was left over from the last frame says nothing about this one */
        if (cw->gl_paint_clip != NULL)
        {
            cairo_region_destroy (cw->gl_paint_clip);
            cw->gl_paint_clip = NULL;
        }

        /* The same windows the XRender path paints, by the same macros */
        if (!WIN_IS_VISIBLE(cw) || !WIN_IS_DAMAGED(cw) ||
            !WIN_IS_REDIRECTED(cw) || !WIN_IS_ON_SCREEN(cw))
        {
            cw->skipped = TRUE;
            continue;
        }

        /*
         * Keep the extents up to date. They are what the damage machinery uses
         * to work out the area a window is leaving behind when it moves or
         * resizes, so without this the vacated area is never repainted. Builds
         * the shadow of the window as a side effect.
         */
        if (cw->extents == None)
        {
            cw->extents = win_extents (cw);
        }

        shape = window_shape (cw);
        opaque_window = WIN_IS_OPAQUE(cw);



        if (opaque_window)
        {
            gboolean painted = TRUE;
            cairo_rectangle_int_t bounds;

            /*
             * A cheap extents test first: a window entirely outside the region
             * left to paint contributes nothing, so the copy and intersection
             * would only be thrown away.
             */
            cairo_region_get_extents (shape, &bounds);
            if (cairo_region_contains_rectangle (paint_region, &bounds) != CAIRO_REGION_OVERLAP_OUT)
            {
                clip = cairo_region_copy (paint_region);
                cairo_region_intersect (clip, shape);
                if (!cairo_region_is_empty (clip))
                {
                    glDisable (GL_BLEND);
                    painted = paint_window_gl (cw, TRUE, clip);
                }
                cairo_region_destroy (clip);
            }

            if (!painted)
            {
                /*
                 * We could not bind this window, so it is not on screen and
                 * must not hide what is below it either.
                 */
                cw->skipped = TRUE;
                continue;
            }

            /*
             * Nothing below shows through an opaque window. A window with a
             * translucent frame only covers its client area, and only the part
             * of it the window actually has: taking away more than was just
             * painted leaves whatever the back buffer held.
             */
            if (WIN_HAS_TRANSLUCENT_FRAME(cw))
            {
                cairo_rectangle_int_t client;

                window_client_area (cw, &client);
                clip = cairo_region_create_rectangle (&client);
                cairo_region_intersect (clip, shape);
                cairo_region_subtract (paint_region, clip);
                cairo_region_destroy (clip);
            }
            else
            {
                cairo_region_subtract (paint_region, shape);
            }
        }

        /*
         * What is still unpainted below this window, for the second pass. Taken
         * after the area it drew solid is claimed, so the blended pass does not
         * cover it again, but before its merely opaque region is taken out,
         * where the window itself still has to be drawn. Only the windows that
         * pass works on keep a copy.
         */
        if ((cw->shadow_width > 0) || !opaque_window ||
            WIN_HAS_TRANSLUCENT_FRAME(cw))
        {
            cairo_rectangle_int_t blended;
            gboolean any = FALSE;

            /*
             * Everything that pass draws lies inside the shadow rectangle or
             * inside the window, so a window with neither of them anywhere
             * near what is left to paint has no work there. Worth the two
             * rectangle tests: the copy below is of a region that covers most
             * of the screen early in the pass, and on a busy desktop most
             * windows are nowhere near the damage.
             */
            if (cw->shadow_width > 0)
            {
                blended.x = cw->attr.x + cw->shadow_dx;
                blended.y = cw->attr.y + cw->shadow_dy;
                blended.width = cw->shadow_width;
                blended.height = cw->shadow_height;
                any = TRUE;
            }
            if (!opaque_window || WIN_HAS_TRANSLUCENT_FRAME(cw))
            {
                cairo_rectangle_int_t win;

                cairo_region_get_extents (shape, &win);
                if (any)
                {
                    gint x2 = MAX (blended.x + blended.width, win.x + win.width);
                    gint y2 = MAX (blended.y + blended.height, win.y + win.height);

                    blended.x = MIN (blended.x, win.x);
                    blended.y = MIN (blended.y, win.y);
                    blended.width = x2 - blended.x;
                    blended.height = y2 - blended.y;
                }
                else
                {
                    blended = win;
                    any = TRUE;
                }
            }

            if (any &&
                cairo_region_contains_rectangle (paint_region, &blended) !=
                CAIRO_REGION_OVERLAP_OUT)
            {
                cw->gl_paint_clip = cairo_region_copy (paint_region);
            }
        }

        if (!opaque_window && (cw->opacity == NET_WM_OPAQUE) && !WIN_IS_SHADED(cw))
        {
            cairo_region_t *opaque = window_opaque_region (cw);

            if (opaque != NULL)
            {
                cairo_region_subtract (paint_region, opaque);
            }
        }

        cw->skipped = FALSE;
    }

    PROF_MARK (prof_pass1);

    /* The background shows wherever no opaque window is left */
    paint_root_gl (screen_info, paint_region);

    PROF_MARK (prof_root);

    /*
     * Second pass, bottom to top: shadows and everything that is blended.
     */
    for (list = g_list_last (screen_info->cwindows); list; list = g_list_previous (list))
    {
        cairo_region_t *shape;

        cw = (CWindow *) list->data;
        /* The first pass leaves a clip behind only where this pass has work */
        if (cw->gl_paint_clip == NULL)
        {
            continue;
        }

        shape = window_shape (cw);

        if (cw->shadow_width > 0)
        {
            cairo_rectangle_int_t sr;

            /*
             * Start from the shadow rectangle rather than from the whole clip.
             * The quad is limited to it anyway, so the result is the same, but
             * the subtraction then works on a handful of rectangles instead of
             * on a region that can cover the screen.
             */
            sr.x = cw->attr.x + cw->shadow_dx;
            sr.y = cw->attr.y + cw->shadow_dy;
            sr.width = cw->shadow_width;
            sr.height = cw->shadow_height;

            clip = cairo_region_create_rectangle (&sr);
            cairo_region_intersect (clip, cw->gl_paint_clip);
            cairo_region_subtract (clip, shape);
            if (data->profile && !cairo_region_is_empty (clip))
            {
                data->prof_shadows += 1.0;
            }
            paint_shadow_gl (cw, clip);
            cairo_region_destroy (clip);
        }

        if (!WIN_IS_OPAQUE(cw) || WIN_HAS_TRANSLUCENT_FRAME(cw))
        {
            /* The last use of the clip this frame, so consume it in place */
            cairo_region_intersect (cw->gl_paint_clip, shape);
            if (!cairo_region_is_empty (cw->gl_paint_clip))
            {
                if (data->profile)
                {
                    data->prof_windows += 1.0;
                }
                glEnable (GL_BLEND);
                paint_window_gl (cw, FALSE, cw->gl_paint_clip);
            }
        }

        cairo_region_destroy (cw->gl_paint_clip);
        cw->gl_paint_clip = NULL;
    }

    /*
     * The real pointer is hidden for as long as the magnifier is on, so the
     * cursor is painted whenever it is, even where the frame buffer is missing
     * and the scene ends up not magnified: otherwise there would be no pointer
     * on the screen at all.
     */
    if (screen_info->zoomed && screen_info->cursor_is_zoomed)
    {
        paint_cursor_gl (screen_info);
    }
    if (zoomed)
    {
        draw_zoomed_scene (screen_info);
    }

    PROF_MARK (prof_pass2);

    if (data->prof_gpu)
    {
        prof_gpu_end (data);
    }

    glUseProgram (0);
    glBindTexture (data->tex_type, 0);

    /*
     * A window could not be bound, so the screen has a hole where it should be
     * and this frame must not reach the screen. A colour depth this GPU cannot
     * bind would leave that hole there for the rest of the session, so the
     * screen goes back to XRender, which can draw any of them. A pixmap the
     * driver merely refused this once only costs the frame: the whole screen is
     * painted again, by which time it may have the memory it just refused us.
     */
    if (data->give_up || data->retry_paint)
    {
        if (data->retry_paint && !data->give_up &&
            (++data->bind_failures > GL_MAX_BIND_RETRIES))
        {
            g_warning ("A window pixmap could not be bound as a texture in %i "
                       "frames in a row, falling back to XRender.",
                       GL_MAX_BIND_RETRIES + 1);
            data->give_up = TRUE;
        }
        /* None of this frame was shown, so the next one owes the whole screen */
        data->full_repaint = TRUE;
        if (frame_damage != NULL)
        {
            cairo_region_destroy (frame_damage);
        }
        if (present_region != NULL)
        {
            cairo_region_destroy (present_region);
        }
        cairo_region_destroy (paint_region);
        myDisplayErrorTrapPopIgnored (display_info);

        return !data->give_up;
    }
    data->bind_failures = 0;

    if (data->present_mode == GL_PRESENT_SWAP)
    {
        /* The magnifier drew the whole back buffer, so a swap presents it */
        if (screen_info->use_egl_backend)
        {
            egl_swap (screen_info, frame_damage, was_full_repaint || zoomed);
        }
        else
        {
            glXSwapBuffers (dpy, screen_info->glx_window);
        }
    }
    else if (data->present_mode == GL_PRESENT_SCENE)
    {
        /*
         * The scene is in the texture and the back buffer is stale, so what
         * the back buffer is missing is blitted out of the scene before the
         * swap. The magnifier has already drawn the whole back buffer from
         * the same texture, so then there is nothing to blit.
         */
        if (!zoomed)
        {
            cairo_rectangle_int_t r;
            gint i, nrects;

            glBindFramebuffer (GL_READ_FRAMEBUFFER, data->fbo);
            glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);

            nrects = cairo_region_num_rectangles (present_region);
            if (nrects > GL_MAX_PRESENT_RECTS)
            {
                cairo_region_get_extents (present_region, &r);
                cairo_region_destroy (present_region);
                present_region = cairo_region_create_rectangle (&r);
                nrects = 1;
            }

            for (i = 0; i < nrects; i++)
            {
                gint gl_y;

                cairo_region_get_rectangle (present_region, i, &r);
                /* The blit counts y from the bottom left */
                gl_y = screen_info->height - r.y - r.height;
                glBlitFramebuffer (r.x, gl_y, r.x + r.width, gl_y + r.height,
                                   r.x, gl_y, r.x + r.width, gl_y + r.height,
                                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
            glBindFramebuffer (GL_FRAMEBUFFER, 0);
        }

        if (screen_info->use_egl_backend)
        {
            egl_swap (screen_info, frame_damage, was_full_repaint || zoomed);
        }
        else
        {
            glXSwapBuffers (dpy, screen_info->glx_window);
        }
    }
    else
    {
        cairo_rectangle_int_t r;
        gboolean from_fbo = (data->present_mode == GL_PRESENT_FBO) && !zoomed;
        gint i, nrects;

        /*
         * The scene lives in the texture, so the pieces the front buffer is
         * owed are blitted out of it into the back buffer, where the copy
         * reads. The magnifier already drew the whole back buffer itself.
         */
        if (from_fbo)
        {
            glBindFramebuffer (GL_READ_FRAMEBUFFER, data->fbo);
            glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);
        }

        nrects = cairo_region_num_rectangles (present_region);
        if (nrects > GL_MAX_PRESENT_RECTS)
        {
            cairo_region_get_extents (present_region, &r);
            cairo_region_destroy (present_region);
            present_region = cairo_region_create_rectangle (&r);
            nrects = 1;
        }

        for (i = 0; i < nrects; i++)
        {
            gint gl_y;

            cairo_region_get_rectangle (present_region, i, &r);
            /* Both the blit and the copy count y from the bottom left */
            gl_y = screen_info->height - r.y - r.height;

            if (from_fbo)
            {
                glBlitFramebuffer (r.x, gl_y, r.x + r.width, gl_y + r.height,
                                   r.x, gl_y, r.x + r.width, gl_y + r.height,
                                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
            glXCopySubBufferMESA (dpy, screen_info->glx_window,
                                  r.x, gl_y, r.width, r.height);
        }

        if (from_fbo)
        {
            glBindFramebuffer (GL_FRAMEBUFFER, 0);
        }
    }

    /*
     * The history holds what the scene changed each frame, and only frames
     * that reach the screen advance it. What was painted is not that: a
     * repaint mostly redraws pixels exactly as they were, and a history that
     * recorded it would replay ever growing regions until every frame painted
     * the whole screen for the rest of the session. Recording takes the
     * region over, so it has to come after the present above read it.
     */
    if (frame_damage != NULL && ((data->present_mode == GL_PRESENT_SWAP) ||
                                 (data->present_mode == GL_PRESENT_SCENE)))
    {
        record_damage (screen_info, frame_damage);
    }
    else if (frame_damage != NULL)
    {
        /* The other modes never replay the history, see get_paint_region() */
        cairo_region_destroy (frame_damage);
    }
    PROF_MARK (prof_present);

    if (data->stats)
    {
        stats_note_paint (screen_info, present_region, prof_wall);
    }

    if (present_region != NULL)
    {
        cairo_region_destroy (present_region);
    }

    /*
     * Let the repaint loop know when the GPU is done with this frame, it waits
     * on that fence before painting the next one.
     */
    if (screen_info->has_ext_arb_sync)
    {
#if defined (glDeleteSync)
        if (screen_info->gl_sync)
        {
            glDeleteSync (screen_info->gl_sync);
            screen_info->gl_sync = 0;
        }
#endif
#if defined (glFenceSync)
        if (data->use_fence)
        {
            screen_info->gl_sync = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
#endif
    }

    cairo_region_destroy (paint_region);
    myDisplayErrorTrapPopIgnored (display_info);

    return TRUE;
}

#endif /* HAVE_EPOXY */

#endif /* HAVE_COMPOSITOR */

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
#include <string.h>

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

#define GL_DAMAGE_HISTORY       3
#define GL_MAX_DEPTHS           8
#define GL_MAX_ROOT_TILES       256

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

typedef struct
{
    GLuint program_win;
    GLuint program_2d;
    GLuint program_shadow_profile;
    GLint u_tex_win;
    GLint u_opacity_win;
    GLint u_opacity_2d;
    GLint u_prof_tex;
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

    cairo_region_t *damage_history[GL_DAMAGE_HISTORY];
    guint damage_index;
    gboolean full_repaint;

    GLXPixmap root_glx_pixmap;
    GLuint root_texture;
    GLuint black_texture;
    gboolean root_missing;
    gint root_width;
    gint root_height;

    GLuint fbo;
    GLuint fbo_texture;
    gint fbo_width;
    gint fbo_height;

    GLuint cursor_texture;
    gint cursor_width;
    gint cursor_height;
    unsigned long cursor_serial;

    gchar *renderer;
    gint swap_interval;
    gboolean swap_control;

    /* A window turned up that this GPU cannot bind, so GL cannot draw the screen */
    gboolean give_up;
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
/* The whole window as the X server sees it, border included */
static void
get_window_pixmap_size (CWindow *cw, gint *width, gint *height)
{
    *width = cw->attr.width + 2 * cw->attr.border_width;
    *height = cw->attr.height + 2 * cw->attr.border_width;
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

    if (WIN_IS_SHAPED(cw))
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

/* The client area of a framed window, the whole window for anything else */
static void
window_client_area (CWindow *cw, cairo_rectangle_int_t *r)
{
    /* The rule lives in compositor.c so both renderers read the same one */
    client_area (cw, &r->x, &r->y, &r->width, &r->height);
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

    if (WIN_HAS_FRAME(cw))
    {
        dx = frameX (cw->c) + frameLeft (cw->c);
        dy = frameY (cw->c) + frameTop (cw->c);
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
    window_client_area (cw, &client);
    cairo_region_intersect_rectangle (cw->gl_opaque, &client);

    return cw->gl_opaque;
}

/*
 * Only what the window itself says is opaque. The bounding shape is a separate
 * fact and a change of one says nothing about the other.
 */
void
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

    if (rects != NULL && nrects > 0)
    {
        cw->gl_opaque_rects = g_malloc (nrects * sizeof (XRectangle));
        memcpy (cw->gl_opaque_rects, rects, nrects * sizeof (XRectangle));
        cw->gl_n_opaque_rects = nrects;
    }

    xfwmGLInvalidateOpaqueRegion (cw);
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

static XfwmGLData *
gl_data (ScreenInfo *screen_info)
{
    return (XfwmGLData *) screen_info->gl_data;
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
 *   off   no sync at all, the fastest but it tears
 *   tear  adaptive, sync unless the frame is already late
 *   other sync to every vblank
 */
static void
set_swap_interval_gl (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    Display *dpy = myScreenGetXDisplay (screen_info);
    gint interval;

    data->swap_control = FALSE;

    switch (screen_info->vblank_mode)
    {
        case VBLANK_OFF:
            interval = 0;
            break;
        case VBLANK_TEAR:
            if (screen_info->has_ext_swap_control_tear)
            {
                interval = -1;
                break;
            }
            g_info ("GLX_EXT_swap_control_tear is missing, syncing to every vblank");
            interval = 1;
            break;
        default:
            interval = 1;
            break;
    }

#if defined (glXSwapIntervalEXT)
    if (screen_info->has_ext_swap_control)
    {
        glXSwapIntervalEXT (dpy, screen_info->glx_window, interval);
        g_info ("GL swap interval set to %i", interval);
        data->swap_interval = interval;
        data->swap_control = TRUE;

        return;
    }
#endif
#if defined (glXSwapIntervalMESA)
    if (screen_info->has_mesa_swap_control)
    {
        /* MESA_swap_control knows nothing about negative intervals */
        interval = (interval < 0) ? 1 : interval;
        glXSwapIntervalMESA ((guint) interval);
        g_info ("GL swap interval set to %i", interval);
        data->swap_interval = interval;
        data->swap_control = TRUE;

        return;
    }
#endif

    g_info ("No swap control available, frames are not synced to the screen");
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

/*
 * Rectangle textures come first. They are addressed in pixels, so there is no
 * way for the sampling to disagree with the real width of the pixmap, while a
 * normalised GL_TEXTURE_2D relies on the driver mapping 1.0 exactly onto the
 * last texel. Drivers that pad the allocation do not, and the window is then
 * drawn very slightly stretched, which is visible as the content wobbling while
 * a window is resized.
 *
 * Whichever target is picked has to work for opaque and ARGB windows alike,
 * otherwise half of the windows cannot be bound at all, and the shader for it
 * has to compile: a driver can offer rectangle pixmaps without offering
 * sampler2DRect in its GLSL.
 */
static gboolean
pick_texture_target (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    guint i;

    for (i = 0; i < 2; i++)
    {
        GLenum target = (i == 0) ? GLX_TEXTURE_RECTANGLE_EXT : GLX_TEXTURE_2D_EXT;

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
        if (!depth_config (screen_info, 24)->usable ||
            !depth_config (screen_info, 32)->usable)
        {
            continue;
        }

        data->program_win = link_program ((data->tex_type == GL_TEXTURE_2D)
                                          ? fragment_source_2d
                                          : fragment_source_rect);
        if (data->program_win != 0)
        {
            return TRUE;
        }

        g_info ("The shader for this texture target does not compile, trying another");
    }

    return FALSE;
}

gboolean
xfwmGLScreenInit (ScreenInfo *screen_info)
{
    XfwmGLData *data;
    Display *dpy;

    g_return_val_if_fail (screen_info != NULL, FALSE);
    TRACE ("entering");

    dpy = myScreenGetXDisplay (screen_info);

    if (!epoxy_has_glx_extension (dpy, screen_info->screen,
                                  "GLX_EXT_texture_from_pixmap"))
    {
        g_warning ("GLX_EXT_texture_from_pixmap is missing, GL compositing disabled.");
        return FALSE;
    }

    if (epoxy_gl_version () < 20)
    {
        g_warning ("OpenGL 2.0 is required for GL compositing, disabled.");
        return FALSE;
    }

    if (!epoxy_has_gl_extension ("GL_ARB_framebuffer_object") &&
        !epoxy_has_gl_extension ("GL_EXT_framebuffer_object"))
    {
        g_warning ("Frame buffer objects are missing, GL compositing disabled.");
        return FALSE;
    }

    data = g_new0 (XfwmGLData, 1);
    screen_info->gl_data = data;

    if (!pick_texture_target (screen_info))
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

    data->u_tex_win = glGetUniformLocation (data->program_win, "tex");
    data->u_opacity_win = glGetUniformLocation (data->program_win, "opacity");
    glUseProgram (data->program_win);
    glUniform1i (data->u_tex_win, 0);

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
        data->u_prof_tex = glGetUniformLocation (data->program_shadow_profile, "prof");
        data->u_prof_size = glGetUniformLocation (data->program_shadow_profile, "size");
        data->u_prof_ramp = glGetUniformLocation (data->program_shadow_profile, "ramp");
        data->u_prof_opacity = glGetUniformLocation (data->program_shadow_profile, "opacity");
        glUseProgram (data->program_shadow_profile);
        glUniform1i (data->u_prof_tex, 0);
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

    data->has_buffer_age = epoxy_has_glx_extension (dpy, screen_info->screen,
                                                    "GLX_EXT_buffer_age");
    data->full_repaint = TRUE;

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

    if (data->root_glx_pixmap != None)
    {
        myDisplayErrorTrapPush (display_info);
        if (glXGetCurrentContext () != NULL)
        {
            glXReleaseTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT);
        }
        glXDestroyPixmap (dpy, data->root_glx_pixmap);
        myDisplayErrorTrapPopIgnored (display_info);
        data->root_glx_pixmap = None;
    }
    if (data->root_texture != 0 && glXGetCurrentContext () != NULL)
    {
        glDeleteTextures (1, &data->root_texture);
    }
    data->root_texture = 0;
    data->root_missing = FALSE;
}

void
xfwmGLInvalidateRootTexture (ScreenInfo *screen_info)
{
    g_return_if_fail (screen_info != NULL);

    if (screen_info->gl_data != NULL)
    {
        free_root_texture (screen_info);
    }
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

    if (glXGetCurrentContext () != NULL)
    {
        free_fbo (screen_info);

        if (data->cursor_texture != 0)
        {
            glDeleteTextures (1, &data->cursor_texture);
        }
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

    g_free (data->renderer);
    g_free (data);
    screen_info->gl_data = NULL;
}

gboolean
xfwmGLGetSwapInterval (ScreenInfo *screen_info, gint *interval)
{
    XfwmGLData *data;

    g_return_val_if_fail (screen_info != NULL, FALSE);

    data = gl_data (screen_info);
    if (data == NULL || !data->swap_control)
    {
        return FALSE;
    }
    *interval = data->swap_interval;

    return TRUE;
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

    data->full_repaint = TRUE;
    /*
     * The screen size or the desktop background may have changed while we were
     * away, so the background is bound again on the next frame.
     */
    free_root_texture (screen_info);
    set_swap_interval_gl (screen_info);
}

void
xfwmGLScreenSizeChanged (ScreenInfo *screen_info)
{
    g_return_if_fail (screen_info != NULL);

    /* Nothing to drop while suspended, the drawable is gone anyway */
    if (screen_info->gl_data == NULL || glXGetCurrentContext () == NULL)
    {
        return;
    }

    free_root_texture (screen_info);
    free_fbo (screen_info);
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
        if (cw->gl_texture_bound && glXGetCurrentContext () != NULL)
        {
            glXReleaseTexImageEXT (dpy, cw->gl_pixmap, GLX_FRONT_EXT);
        }
        cw->gl_texture_bound = FALSE;
        glXDestroyPixmap (dpy, cw->gl_pixmap);
        cw->gl_pixmap = None;
    }
    if (cw->gl_texture != 0 && glXGetCurrentContext () != NULL)
    {
        glDeleteTextures (1, &cw->gl_texture);
        cw->gl_texture = 0;
    }
}

void
xfwmGLFreeWindowShadow (CWindow *cw)
{
    g_return_if_fail (cw != NULL);

    if (cw->screen_info->gl_data == NULL || glXGetCurrentContext () == NULL)
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

gboolean
xfwmGLUpdateWindowShadow (CWindow *cw, gdouble opacity, gint width, gint height)
{
    ScreenInfo *screen_info;
    XfwmGLData *data;
    XImage *image;
    gint gaussian_size, shadow_width = 0, shadow_height = 0;

    g_return_val_if_fail (cw != NULL, FALSE);

    screen_info = cw->screen_info;
    data = gl_data (screen_info);
    g_return_val_if_fail (data != NULL, FALSE);
    g_return_val_if_fail (screen_info->gaussianMap != NULL, FALSE);
    if (glXGetCurrentContext () == NULL)
    {
        return FALSE;
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

        return TRUE;
    }

    image = make_shadow (screen_info, opacity, width, height);
    if (image == NULL)
    {
        return FALSE;
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

    return TRUE;
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

static gboolean
bind_window_texture (CWindow *cw)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);
    DisplayInfo *display_info = screen_info->display_info;
    Display *dpy = myScreenGetXDisplay (screen_info);
    XfwmGLDepth *dc;

    if (cw->name_window_pixmap == None)
    {
        myDisplayErrorTrapPush (display_info);
        cw->name_window_pixmap = XCompositeNameWindowPixmap (dpy, cw->id);
        if (myDisplayErrorTrapPop (display_info) != Success)
        {
            cw->name_window_pixmap = None;
        }
        if (cw->name_window_pixmap == None)
        {
            return FALSE;
        }
    }

    dc = depth_config (screen_info, cw->attr.depth);
    if (dc == NULL || !dc->usable)
    {
        /*
         * A depth with no config of its own will never get one, so the window
         * would stay invisible: hand the screen to XRender, which can draw it.
         * Running out of room in the table is a different thing, that one is
         * not the driver's answer, so it is left alone.
         */
        if ((dc != NULL) && !data->give_up)
        {
            g_warning ("A window of depth %i cannot be bound as a texture, "
                       "falling back to XRender.", cw->attr.depth);
            data->give_up = TRUE;
        }

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
            return FALSE;
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
     * The contents behind the GLX pixmap change as the window draws, and the
     * texture has to be released and bound again for the new content to be
     * guaranteed visible. Only worth doing when the window has actually drawn
     * something, which is what repair_win() records: a window repainted merely
     * because a neighbour changed still holds what it held at the last bind.
     */
    if (!cw->gl_texture_bound || cw->gl_content_dirty)
    {
        if (cw->gl_texture_bound)
        {
            glXReleaseTexImageEXT (dpy, cw->gl_pixmap, GLX_FRONT_EXT);
            cw->gl_texture_bound = FALSE;
        }
        glXBindTexImageEXT (dpy, cw->gl_pixmap, GLX_FRONT_EXT, NULL);
        cw->gl_texture_bound = TRUE;
        cw->gl_content_dirty = FALSE;
    }

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
 * GL says nothing about it.
 */
static void
use_program (GLuint program, GLint u_opacity, gfloat opacity)
{
    glUseProgram (program);
    glUniform1f (u_opacity, opacity);
}

static void
set_win_opacity (XfwmGLData *data, gfloat opacity)
{
    glUniform1f (data->u_opacity_win, opacity);
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
    gfloat opacity;

    if (!bind_window_texture (cw))
    {
        return FALSE;
    }

    opacity = solid_part ? 1.0f : (gfloat) cw->opacity / (gfloat) NET_WM_OPAQUE;

    if (WIN_HAS_FRAME(cw) && (screen_info->params->frame_opacity < 100))
    {
        gint frame_top, frame_bottom, frame_left, frame_right;
        gint frame_width, frame_height;

        frame_width = cw->attr.width;
        frame_height = cw->attr.height;
        frame_top = frameTop (cw->c);
        frame_bottom = frameBottom (cw->c);
        frame_left = frameLeft (cw->c);
        frame_right = frameRight (cw->c);

        /* The frame is only painted in the blended pass, never as a solid */
        if (!solid_part)
        {
            set_win_opacity (gl_data (screen_info),
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

        set_win_opacity (gl_data (screen_info), opacity);
        draw_window_part (cw, frame_left, frame_top,
                          cw->attr.x + frame_left, cw->attr.y + frame_top,
                          frame_width - frame_left - frame_right,
                          frame_height - frame_top - frame_bottom,
                          clip);
    }
    else
    {
        gint width, height;

        get_window_pixmap_size (cw, &width, &height);
        set_win_opacity (gl_data (screen_info), opacity);
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
    glUseProgram (data->program_win);
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
    Pixmap pixmap;
    Window root_ret;
    gint x_ret, y_ret;
    guint width_ret, height_ret, border_ret, depth_ret;
    gint attribs[] = {
        GLX_TEXTURE_TARGET_EXT, 0,
        GLX_TEXTURE_FORMAT_EXT, 0,
        None
    };

    if (data->root_texture != 0)
    {
        glBindTexture (data->tex_type, data->root_texture);
        /*
         * Whoever drew the desktop may have painted into the same pixmap
         * again, and the contents of a bound texture are undefined once that
         * happens, so the image is taken again. Same reason as for windows.
         */
        if (data->root_glx_pixmap != None)
        {
            glXReleaseTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT);
            glXBindTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT, NULL);
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

    /* The background is not always as deep as the screen, so ask the pixmap */
    dc = depth_config (screen_info, (gint) depth_ret);
    if (dc == NULL || !dc->usable)
    {
        data->root_missing = TRUE;

        return FALSE;
    }

    /*
     * The background is drawn one tile at a time, so a pattern small enough to
     * need thousands of them would cost more than the frame is worth. Nothing
     * that sets a wallpaper does this, but a stray tiny pixmap must not be able
     * to bring the compositor to a halt.
     */
    if (((gint) width_ret * (gint) height_ret) * GL_MAX_ROOT_TILES <
        (screen_info->width * screen_info->height))
    {
        g_warning ("The desktop background is a %ux%u tile, too small for the "
                   "GL renderer to repeat; drawing it plain.",
                   width_ret, height_ret);
        data->root_missing = TRUE;

        return FALSE;
    }

    data->root_width = (gint) width_ret;
    data->root_height = (gint) height_ret;

    attribs[1] = (gint) data->tex_target;
    attribs[3] = (depth_ret == 32) ? GLX_TEXTURE_FORMAT_RGBA_EXT
                                   : GLX_TEXTURE_FORMAT_RGB_EXT;

    myDisplayErrorTrapPush (display_info);
    data->root_glx_pixmap = glXCreatePixmap (dpy, dc->fbconfig, pixmap, attribs);
    if (myDisplayErrorTrapPop (display_info) != Success)
    {
        data->root_glx_pixmap = None;
    }
    if (data->root_glx_pixmap == None)
    {
        data->root_missing = TRUE;

        return FALSE;
    }

    glGenTextures (1, &data->root_texture);
    glBindTexture (data->tex_type, data->root_texture);
    set_tex_params (data->tex_type, GL_NEAREST);
    glXBindTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT, NULL);

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
        gint tex_width = (data->root_width > 0) ? data->root_width : screen_info->width;
        gint tex_height = (data->root_height > 0) ? data->root_height : screen_info->height;
        cairo_rectangle_int_t area;
        gint x, y, first_x, first_y;

        set_win_opacity (data, 1.0f);
        /*
         * A background pixmap smaller than the screen is tiled by the X
         * server, so tile it here too rather than stretch it. Usually that is
         * one single tile, and only the tiles the repaint can touch are drawn.
         */
        cairo_region_get_extents (clip, &area);
        first_x = (area.x / tex_width) * tex_width;
        first_y = (area.y / tex_height) * tex_height;

        for (y = first_y; y < area.y + area.height; y += tex_height)
        {
            for (x = first_x; x < area.x + area.width; x += tex_width)
            {
                draw_quad (screen_info, data->tex_type,
                           0, 0, tex_width, tex_height,
                           x, y, tex_width, tex_height, clip);
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
        glUseProgram (data->program_win);
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
        gint i, n;

        cursor = XFixesGetCursorImage (myScreenGetXDisplay (screen_info));
        if (cursor == NULL)
        {
            return;
        }

        n = cursor->width * cursor->height;
        pixels = g_new (guint32, n);
        /* XFixes hands out longs, GL wants 32 bits */
        for (i = 0; i < n; i++)
        {
            pixels[i] = (guint32) cursor->pixels[i];
        }

        if (data->cursor_texture == 0)
        {
            glGenTextures (1, &data->cursor_texture);
        }
        glBindTexture (GL_TEXTURE_2D, data->cursor_texture);
        set_tex_params (GL_TEXTURE_2D, GL_LINEAR);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, cursor->width, cursor->height,
                      0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
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

    rect.x = 0;
    rect.y = 0;
    rect.width = screen_info->width;
    rect.height = screen_info->height;
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
    GLfloat filter;

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

    filter = (zoom > 0.25 && zoom < 1.0) ? GL_LINEAR : GL_NEAREST;

    glDisable (GL_BLEND);
    glBindTexture (GL_TEXTURE_2D, data->fbo_texture);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint) filter);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint) filter);
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

    if (data->has_buffer_age && !data->full_repaint)
    {
        glXQueryDrawable (dpy, screen_info->glx_window,
                          GLX_BACK_BUFFER_AGE_EXT, &age);
    }

    if (age == 0 || age > GL_DAMAGE_HISTORY || data->full_repaint)
    {
        cairo_rectangle_int_t r;

        r.x = 0;
        r.y = 0;
        r.width = screen_info->width;
        r.height = screen_info->height;
        region = cairo_region_create_rectangle (&r);
    }
    else
    {
        region = cairo_region_copy (damage);
        /* Add back what the older frames in the buffer never saw */
        for (i = 0; i < age - 1; i++)
        {
            guint slot = (data->damage_index + GL_DAMAGE_HISTORY - i - 1) % GL_DAMAGE_HISTORY;

            if (data->damage_history[slot] != NULL)
            {
                cairo_region_union (region, data->damage_history[slot]);
            }
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

gboolean
xfwmGLPaintAll (ScreenInfo *screen_info, XserverRegion damage)
{
    XfwmGLData *data;
    DisplayInfo *display_info;
    Display *dpy;
    cairo_region_t *frame_damage;
    cairo_region_t *paint_region;
    cairo_region_t *clip;
    GList *list;
    CWindow *cw;
    gboolean zoomed;

    g_return_val_if_fail (screen_info != NULL, FALSE);
    TRACE ("entering");

    data = gl_data (screen_info);
    if (data == NULL)
    {
        return FALSE;
    }

    display_info = screen_info->display_info;
    dpy = myScreenGetXDisplay (screen_info);

    if (!glXMakeCurrent (dpy, screen_info->glx_window, screen_info->glx_context))
    {
        g_warning ("Cannot make the GL context current, GL compositing disabled.");
        return FALSE;
    }

    myDisplayErrorTrapPush (display_info);

    frame_damage = fetch_damage (dpy, damage);
    paint_region = get_paint_region (screen_info, frame_damage);

    if (cairo_region_is_empty (paint_region))
    {
        /* Nothing reaches the screen, so nothing is recorded either */
        cairo_region_destroy (paint_region);
        cairo_region_destroy (frame_damage);
        myDisplayErrorTrapPopIgnored (display_info);

        return TRUE;
    }

    record_damage (screen_info, frame_damage);
    data->full_repaint = FALSE;

    zoomed = screen_info->zoomed;
    if (zoomed && !bind_zoom_fbo (screen_info))
    {
        zoomed = FALSE;
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

        if (!WIN_IS_VISIBLE(cw) || !WIN_IS_DAMAGED(cw) || !WIN_IS_REDIRECTED(cw))
        {
            cw->skipped = TRUE;
            continue;
        }
        if ((cw->attr.x + cw->attr.width < 1) || (cw->attr.y + cw->attr.height < 1) ||
            (cw->attr.x >= screen_info->width) || (cw->attr.y >= screen_info->height))
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

            clip = cairo_region_copy (paint_region);
            cairo_region_intersect (clip, shape);
            if (!cairo_region_is_empty (clip))
            {
                glDisable (GL_BLEND);
                painted = paint_window_gl (cw, TRUE, clip);
            }
            cairo_region_destroy (clip);

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
            if (WIN_HAS_FRAME(cw) && (screen_info->params->frame_opacity < 100))
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
         * after the window has claimed the area it just drew solid, so the
         * blended pass does not draw over it again, but before the region the
         * window merely declares opaque is taken out: the window itself still
         * has to be drawn there. Same order as the XRender path.
         */
        cw->gl_paint_clip = cairo_region_copy (paint_region);

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

    /* The background shows wherever no opaque window is left */
    paint_root_gl (screen_info, paint_region);

    /*
     * Second pass, bottom to top: shadows and everything that is blended.
     */
    for (list = g_list_last (screen_info->cwindows); list; list = g_list_previous (list))
    {
        cairo_region_t *shape;

        cw = (CWindow *) list->data;
        if (cw->skipped || cw->gl_paint_clip == NULL)
        {
            continue;
        }

        shape = window_shape (cw);

        if (cw->shadow_width > 0)
        {
            clip = cairo_region_copy (cw->gl_paint_clip);
            cairo_region_subtract (clip, shape);
            paint_shadow_gl (cw, clip);
            cairo_region_destroy (clip);
        }

        if (!WIN_IS_OPAQUE(cw) ||
            (WIN_HAS_FRAME(cw) && (screen_info->params->frame_opacity < 100)))
        {
            clip = cairo_region_copy (cw->gl_paint_clip);
            cairo_region_intersect (clip, shape);
            if (!cairo_region_is_empty (clip))
            {
                glEnable (GL_BLEND);
                paint_window_gl (cw, FALSE, clip);
            }
            cairo_region_destroy (clip);
        }

        cairo_region_destroy (cw->gl_paint_clip);
        cw->gl_paint_clip = NULL;
    }

    if (zoomed)
    {
        if (screen_info->cursor_is_zoomed)
        {
            paint_cursor_gl (screen_info);
        }
        draw_zoomed_scene (screen_info);
    }

    glUseProgram (0);
    glBindTexture (data->tex_type, 0);

    glXSwapBuffers (dpy, screen_info->glx_window);

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
        }
#endif
#if defined (glFenceSync)
        screen_info->gl_sync = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
#endif
    }

    cairo_region_destroy (paint_region);
    myDisplayErrorTrapPopIgnored (display_info);

    /*
     * A window turned up whose colour depth this GPU cannot bind. Skipping it
     * would leave a hole in the screen for the rest of the session, so hand the
     * whole screen back to XRender, which can draw it.
     */
    if (data->give_up)
    {
        return FALSE;
    }

    return TRUE;
}

#endif /* HAVE_EPOXY */

#endif /* HAVE_COMPOSITOR */

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
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

#include <glib.h>

#include "display.h"
#include "screen.h"
#include "client.h"
#include "frame.h"
#include "compositor-priv.h"
#include "compositor-gl.h"

#ifdef HAVE_EPOXY

#define GL_DAMAGE_HISTORY       3
#define GL_DEPTH_RGB            0
#define GL_DEPTH_RGBA           1

typedef struct
{
    GLuint program_win;
    GLuint program_shadow;
    GLint u_tex_win;
    GLint u_opacity_win;
    GLint u_tex_shadow;
    GLint u_opacity_shadow;

    GLenum tex_type;
    GLenum tex_target;

    GLXFBConfig fbconfig[2];
    gboolean fbconfig_ok[2];
    gboolean y_inverted[2];

    gboolean has_buffer_age;

    XserverRegion damage_history[GL_DAMAGE_HISTORY];
    guint damage_index;
    gboolean full_repaint;

    GLXPixmap root_glx_pixmap;
    GLuint root_texture;
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

static const gchar *fragment_shadow_2d =
    "uniform sampler2D tex;\n"
    "uniform float opacity;\n"
    "varying vec2 uv;\n"
    "void main (void)\n"
    "{\n"
    "    gl_FragColor = vec4 (0.0, 0.0, 0.0, texture2D (tex, uv).a * opacity);\n"
    "}\n";

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
 * Find a frame buffer config able to bind a pixmap of that depth as a
 * texture. GL_TEXTURE_2D is preferred over the rectangle target so that
 * texture coordinates stay normalised.
 */
static gboolean
choose_fbconfig (ScreenInfo *screen_info, gint slot, gint depth)
{
    XfwmGLData *data = gl_data (screen_info);
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

        if (value & GLX_TEXTURE_2D_BIT_EXT)
        {
            data->tex_type = GL_TEXTURE_2D;
            data->tex_target = GLX_TEXTURE_2D_EXT;
        }
        else if (value & GLX_TEXTURE_RECTANGLE_BIT_EXT)
        {
            data->tex_type = GL_TEXTURE_RECTANGLE_ARB;
            data->tex_target = GLX_TEXTURE_RECTANGLE_EXT;
        }
        else
        {
            continue;
        }

        status = glXGetFBConfigAttrib (dpy, configs[i], GLX_Y_INVERTED_EXT, &value);
        data->y_inverted[slot] = (status == Success && value == True);
        data->fbconfig[slot] = configs[i];
        data->fbconfig_ok[slot] = TRUE;
        found = TRUE;
        break;
    }
    XFree (configs);

    return found;
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

    if (!choose_fbconfig (screen_info, GL_DEPTH_RGB, 24))
    {
        g_warning ("No GLX config to bind opaque windows, GL compositing disabled.");
        xfwmGLScreenFinish (screen_info);
        return FALSE;
    }
    /* An ARGB config is only needed for windows that have one */
    choose_fbconfig (screen_info, GL_DEPTH_RGBA, 32);

    if (data->tex_type == GL_TEXTURE_2D &&
        !epoxy_has_gl_extension ("GL_ARB_texture_non_power_of_two") &&
        epoxy_gl_version () < 30)
    {
        g_warning ("Non power of two textures are missing, GL compositing disabled.");
        xfwmGLScreenFinish (screen_info);
        return FALSE;
    }

    data->program_win = link_program ((data->tex_type == GL_TEXTURE_2D)
                                      ? fragment_source_2d : fragment_source_rect);
    if (data->program_win == 0)
    {
        xfwmGLScreenFinish (screen_info);
        return FALSE;
    }
    data->u_tex_win = glGetUniformLocation (data->program_win, "tex");
    data->u_opacity_win = glGetUniformLocation (data->program_win, "opacity");

    /* Shadows are alpha only textures we upload ourselves, always 2D */
    data->program_shadow = link_program (fragment_shadow_2d);
    if (data->program_shadow == 0)
    {
        xfwmGLScreenFinish (screen_info);
        return FALSE;
    }
    data->u_tex_shadow = glGetUniformLocation (data->program_shadow, "tex");
    data->u_opacity_shadow = glGetUniformLocation (data->program_shadow, "opacity");

    data->has_buffer_age = epoxy_has_glx_extension (dpy, screen_info->screen,
                                                    "GLX_EXT_buffer_age");
    data->full_repaint = TRUE;

    /*
     * Sync the swaps to the screen. The XRender path does this on the pixmap
     * drawable, here the frames go straight to the overlay window.
     */
#if defined (glXSwapIntervalEXT)
    if (screen_info->has_ext_swap_control)
    {
        glXSwapIntervalEXT (dpy, screen_info->glx_window, 1);
    }
    else
#endif
#if defined (glXSwapIntervalMESA)
    if (screen_info->has_mesa_swap_control)
    {
        glXSwapIntervalMESA (1);
    }
    else
#endif
    {
        g_info ("No swap control available, frames are not synced to the screen");
    }

    glDisable (GL_DEPTH_TEST);
    glDepthMask (GL_FALSE);
    glDisable (GL_CULL_FACE);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    g_info ("GL compositing enabled (%s, buffer age %s)",
            (data->tex_type == GL_TEXTURE_2D) ? "texture 2D" : "texture rectangle",
            data->has_buffer_age ? "yes" : "no");

    return TRUE;
}

static void
free_root_texture (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data == NULL)
    {
        return;
    }

    if (data->root_glx_pixmap != None)
    {
        glXReleaseTexImageEXT (myScreenGetXDisplay (screen_info),
                               data->root_glx_pixmap, GLX_FRONT_EXT);
        glXDestroyPixmap (myScreenGetXDisplay (screen_info), data->root_glx_pixmap);
        data->root_glx_pixmap = None;
    }
    if (data->root_texture != 0)
    {
        glDeleteTextures (1, &data->root_texture);
        data->root_texture = 0;
    }
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

    free_root_texture (screen_info);
    free_fbo (screen_info);

    if (data->cursor_texture != 0)
    {
        glDeleteTextures (1, &data->cursor_texture);
    }
    if (data->program_win != 0)
    {
        glDeleteProgram (data->program_win);
    }
    if (data->program_shadow != 0)
    {
        glDeleteProgram (data->program_shadow);
    }
    for (i = 0; i < GL_DAMAGE_HISTORY; i++)
    {
        if (data->damage_history[i] != None)
        {
            XFixesDestroyRegion (myScreenGetXDisplay (screen_info),
                                 data->damage_history[i]);
        }
    }

    g_free (data);
    screen_info->gl_data = NULL;
}

void
xfwmGLScreenSizeChanged (ScreenInfo *screen_info)
{
    g_return_if_fail (screen_info != NULL);

    if (screen_info->gl_data == NULL)
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

    if (cw->gl_pixmap != None)
    {
        if (cw->gl_texture_bound)
        {
            glXReleaseTexImageEXT (dpy, cw->gl_pixmap, GLX_FRONT_EXT);
            cw->gl_texture_bound = FALSE;
        }
        glXDestroyPixmap (dpy, cw->gl_pixmap);
        cw->gl_pixmap = None;
    }
    if (cw->gl_texture != 0)
    {
        glDeleteTextures (1, &cw->gl_texture);
        cw->gl_texture = 0;
    }
}

void
xfwmGLFreeWindowShadow (CWindow *cw)
{
    g_return_if_fail (cw != NULL);

    if (cw->screen_info->gl_data == NULL)
    {
        return;
    }

    if (cw->gl_shadow_texture != 0)
    {
        glDeleteTextures (1, &cw->gl_shadow_texture);
        cw->gl_shadow_texture = 0;
    }
    cw->gl_shadow_width = 0;
    cw->gl_shadow_height = 0;
}

gboolean
xfwmGLUpdateWindowShadow (CWindow *cw, gdouble opacity, gint width, gint height)
{
    XImage *image;

    g_return_val_if_fail (cw != NULL, FALSE);

    xfwmGLFreeWindowShadow (cw);

    image = compositorMakeShadowImage (cw->screen_info, opacity, width, height);
    if (image == NULL)
    {
        return FALSE;
    }

    glGenTextures (1, &cw->gl_shadow_texture);
    glBindTexture (GL_TEXTURE_2D, cw->gl_shadow_texture);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    /* One byte per pixel, so the stride in bytes is also the stride in pixels */
    glPixelStorei (GL_UNPACK_ROW_LENGTH, image->bytes_per_line);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_ALPHA,
                  image->width, image->height, 0,
                  GL_ALPHA, GL_UNSIGNED_BYTE, image->data);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture (GL_TEXTURE_2D, 0);

    cw->gl_shadow_width = image->width;
    cw->gl_shadow_height = image->height;
    XDestroyImage (image);

    return TRUE;
}

static gboolean
bind_window_texture (CWindow *cw)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);
    DisplayInfo *display_info = screen_info->display_info;
    Display *dpy = myScreenGetXDisplay (screen_info);
    gint slot;

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

    slot = (cw->attr.depth == 32) ? GL_DEPTH_RGBA : GL_DEPTH_RGB;
    if (!data->fbconfig_ok[slot])
    {
        return FALSE;
    }

    if (cw->gl_pixmap == None)
    {
        const gint attribs[] = {
            GLX_TEXTURE_TARGET_EXT, (gint) data->tex_target,
            GLX_TEXTURE_FORMAT_EXT, (slot == GL_DEPTH_RGBA)
                                     ? GLX_TEXTURE_FORMAT_RGBA_EXT
                                     : GLX_TEXTURE_FORMAT_RGB_EXT,
            None
        };

        myDisplayErrorTrapPush (display_info);
        cw->gl_pixmap = glXCreatePixmap (dpy, data->fbconfig[slot],
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
        glTexParameteri (data->tex_type, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri (data->tex_type, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri (data->tex_type, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (data->tex_type, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    else
    {
        glBindTexture (data->tex_type, cw->gl_texture);
    }

    /*
     * The contents behind the GLX pixmap change as the window draws, the
     * texture has to be released and bound again to see the new content.
     */
    if (cw->gl_texture_bound)
    {
        glXReleaseTexImageEXT (dpy, cw->gl_pixmap, GLX_FRONT_EXT);
        cw->gl_texture_bound = FALSE;
    }
    glXBindTexImageEXT (dpy, cw->gl_pixmap, GLX_FRONT_EXT, NULL);
    cw->gl_texture_bound = TRUE;

    return TRUE;
}

static void
get_window_pixmap_size (CWindow *cw, gint *width, gint *height)
{
    *width = cw->attr.width + 2 * cw->attr.border_width;
    *height = cw->attr.height + 2 * cw->attr.border_width;
}

/*
 * Draw one textured quad, clipped to every rectangle of the region.
 * Source and destination are in screen pixels, the texture coordinates are
 * worked out from the size of the texture.
 */
static void
draw_quad (ScreenInfo *screen_info, GLenum tex_type, gboolean y_inverted,
           gint sx, gint sy, gint tex_width, gint tex_height,
           gint dx, gint dy, gint width, gint height,
           XRectangle *rects, gint nrects)
{
    gfloat x1, y1, x2, y2;
    gfloat u1, v1, u2, v2;
    gint i;

    if (width <= 0 || height <= 0 || tex_width <= 0 || tex_height <= 0)
    {
        return;
    }

    x1 = 2.0f * (gfloat) dx / (gfloat) screen_info->width - 1.0f;
    x2 = 2.0f * (gfloat) (dx + width) / (gfloat) screen_info->width - 1.0f;
    y1 = 1.0f - 2.0f * (gfloat) dy / (gfloat) screen_info->height;
    y2 = 1.0f - 2.0f * (gfloat) (dy + height) / (gfloat) screen_info->height;

    if (tex_type == GL_TEXTURE_RECTANGLE_ARB)
    {
        u1 = (gfloat) sx;
        u2 = (gfloat) (sx + width);
        v1 = (gfloat) sy;
        v2 = (gfloat) (sy + height);
    }
    else
    {
        u1 = (gfloat) sx / (gfloat) tex_width;
        u2 = (gfloat) (sx + width) / (gfloat) tex_width;
        v1 = (gfloat) sy / (gfloat) tex_height;
        v2 = (gfloat) (sy + height) / (gfloat) tex_height;
    }

    if (y_inverted)
    {
        gfloat v = v1;
        v1 = (tex_type == GL_TEXTURE_RECTANGLE_ARB) ? (gfloat) tex_height - v2 : 1.0f - v2;
        v2 = (tex_type == GL_TEXTURE_RECTANGLE_ARB) ? (gfloat) tex_height - v : 1.0f - v;
    }

    for (i = 0; i < nrects; i++)
    {
        /* GL scissor counts from the bottom of the screen */
        glScissor (rects[i].x,
                   screen_info->height - (rects[i].y + rects[i].height),
                   rects[i].width, rects[i].height);

        glBegin (GL_QUADS);
        glTexCoord2f (u1, v1);
        glVertex2f (x1, y1);
        glTexCoord2f (u2, v1);
        glVertex2f (x2, y1);
        glTexCoord2f (u2, v2);
        glVertex2f (x2, y2);
        glTexCoord2f (u1, v2);
        glVertex2f (x1, y2);
        glEnd ();
    }
}

static void
draw_window_part (CWindow *cw, gint sx, gint sy, gint dx, gint dy,
                  gint width, gint height, gfloat opacity,
                  XRectangle *rects, gint nrects)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);
    gint tex_width, tex_height, slot;

    get_window_pixmap_size (cw, &tex_width, &tex_height);
    slot = (cw->attr.depth == 32) ? GL_DEPTH_RGBA : GL_DEPTH_RGB;

    glUniform1f (data->u_opacity_win, opacity);
    draw_quad (screen_info, data->tex_type, data->y_inverted[slot],
               sx, sy, tex_width, tex_height,
               dx, dy, width, height, rects, nrects);
}

/*
 * Paint a window, either its opaque part with blending off, or the whole
 * window blended. Mirrors paint_win() of the XRender path, including the
 * frame drawn separately when the title bar is translucent.
 */
static void
paint_window_gl (CWindow *cw, gboolean solid_part,
                 XRectangle *rects, gint nrects)
{
    ScreenInfo *screen_info = cw->screen_info;
    gfloat opacity;

    if (nrects == 0)
    {
        return;
    }

    if (!bind_window_texture (cw))
    {
        return;
    }

    opacity = (gfloat) cw->opacity / (gfloat) NET_WM_OPAQUE;

    if (WIN_HAS_FRAME(cw) && (screen_info->params->frame_opacity < 100))
    {
        gint frame_top, frame_bottom, frame_left, frame_right;
        gint frame_width, frame_height;
        gfloat frame_opacity;

        frame_width = cw->attr.width;
        frame_height = cw->attr.height;
        frame_top = frameTop (cw->c);
        frame_bottom = frameBottom (cw->c);
        frame_left = frameLeft (cw->c);
        frame_right = frameRight (cw->c);
        frame_opacity = opacity * (gfloat) screen_info->params->frame_opacity / 100.0f;

        if (!solid_part)
        {
            /* Top border, the title bar */
            draw_window_part (cw, 0, 0, cw->attr.x, cw->attr.y,
                              frame_width, frame_top, frame_opacity, rects, nrects);
            /* Bottom border */
            draw_window_part (cw, 0, frame_height - frame_bottom,
                              cw->attr.x, cw->attr.y + frame_height - frame_bottom,
                              frame_width, frame_bottom, frame_opacity, rects, nrects);
            /* Left border */
            draw_window_part (cw, 0, frame_top,
                              cw->attr.x, cw->attr.y + frame_top,
                              frame_left, frame_height - frame_top - frame_bottom,
                              frame_opacity, rects, nrects);
            /* Right border */
            draw_window_part (cw, frame_width - frame_right, frame_top,
                              cw->attr.x + frame_width - frame_right,
                              cw->attr.y + frame_top,
                              frame_right, frame_height - frame_top - frame_bottom,
                              frame_opacity, rects, nrects);
            /* Client area */
            draw_window_part (cw, frame_left, frame_top,
                              cw->attr.x + frame_left, cw->attr.y + frame_top,
                              frame_width - frame_left - frame_right,
                              frame_height - frame_top - frame_bottom,
                              opacity, rects, nrects);
        }
        else
        {
            /* Only the client area is opaque when the frame is translucent */
            draw_window_part (cw, frame_left, frame_top,
                              cw->attr.x + frame_left, cw->attr.y + frame_top,
                              frame_width - frame_left - frame_right,
                              frame_height - frame_top - frame_bottom,
                              1.0f, rects, nrects);
        }
    }
    else
    {
        gint width, height;

        get_window_pixmap_size (cw, &width, &height);
        draw_window_part (cw, 0, 0, cw->attr.x, cw->attr.y, width, height,
                          solid_part ? 1.0f : opacity, rects, nrects);
    }
}

static void
paint_shadow_gl (CWindow *cw, XRectangle *rects, gint nrects)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);

    if (cw->gl_shadow_texture == 0 || nrects == 0)
    {
        return;
    }

    glUseProgram (data->program_shadow);
    glUniform1i (data->u_tex_shadow, 0);
    glUniform1f (data->u_opacity_shadow, 1.0f);
    glBindTexture (GL_TEXTURE_2D, cw->gl_shadow_texture);
    glEnable (GL_BLEND);

    draw_quad (screen_info, GL_TEXTURE_2D, FALSE,
               0, 0, cw->gl_shadow_width, cw->gl_shadow_height,
               cw->attr.x + cw->shadow_dx, cw->attr.y + cw->shadow_dy,
               cw->gl_shadow_width, cw->gl_shadow_height, rects, nrects);

    glBindTexture (GL_TEXTURE_2D, 0);
    glUseProgram (data->program_win);
    glUniform1i (data->u_tex_win, 0);
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
    Atom backgroundProps[2];
    Pixmap pixmap = None;
    gint p;

    if (data->root_texture != 0)
    {
        glBindTexture (data->tex_type, data->root_texture);
        if (data->root_glx_pixmap != None)
        {
            glXReleaseTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT);
            glXBindTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT, NULL);
        }
        return TRUE;
    }

    if (!data->fbconfig_ok[GL_DEPTH_RGB])
    {
        return FALSE;
    }

    backgroundProps[0] = display_info->atoms[XROOTPMAP];
    backgroundProps[1] = display_info->atoms[XSETROOT];

    for (p = 0; p < 2; p++)
    {
        Atom actual_type;
        gint actual_format;
        unsigned long nitems;
        unsigned long bytes_after;
        guchar *prop;
        gint result;

        result = XGetWindowProperty (dpy, screen_info->xroot, backgroundProps[p],
                                     0, 4, False, AnyPropertyType,
                                     &actual_type, &actual_format, &nitems,
                                     &bytes_after, &prop);
        if ((result == Success) &&
            (actual_type == display_info->atoms[PIXMAP]) &&
            (actual_format == 32) &&
            (nitems == 1))
        {
            memcpy (&pixmap, prop, 4);
            XFree (prop);
            break;
        }
        if (result == Success && prop != NULL)
        {
            XFree (prop);
        }
    }

    if (pixmap == None)
    {
        return FALSE;
    }

    {
        Window root_ret;
        gint x_ret, y_ret;
        guint width_ret, height_ret, border_ret, depth_ret;
        const gint attribs[] = {
            GLX_TEXTURE_TARGET_EXT, (gint) data->tex_target,
            GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGB_EXT,
            None
        };

        myDisplayErrorTrapPush (display_info);
        if (!XGetGeometry (dpy, pixmap, &root_ret, &x_ret, &y_ret,
                           &width_ret, &height_ret, &border_ret, &depth_ret))
        {
            myDisplayErrorTrapPopIgnored (display_info);
            return FALSE;
        }
        data->root_width = (gint) width_ret;
        data->root_height = (gint) height_ret;

        data->root_glx_pixmap = glXCreatePixmap (dpy, data->fbconfig[GL_DEPTH_RGB],
                                                 pixmap, attribs);
        if (myDisplayErrorTrapPop (display_info) != Success)
        {
            data->root_glx_pixmap = None;
        }
        if (data->root_glx_pixmap == None)
        {
            return FALSE;
        }
    }

    glGenTextures (1, &data->root_texture);
    glBindTexture (data->tex_type, data->root_texture);
    glTexParameteri (data->tex_type, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (data->tex_type, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri (data->tex_type, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (data->tex_type, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glXBindTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT, NULL);

    return TRUE;
}

static void
paint_root_gl (ScreenInfo *screen_info, XRectangle *rects, gint nrects)
{
    XfwmGLData *data = gl_data (screen_info);

    if (nrects == 0)
    {
        return;
    }

    glDisable (GL_BLEND);

    if (bind_root_texture (screen_info))
    {
        gint tex_width = (data->root_width > 0) ? data->root_width : screen_info->width;
        gint tex_height = (data->root_height > 0) ? data->root_height : screen_info->height;

        glUniform1f (data->u_opacity_win, 1.0f);
        /*
         * The background pixmap can be smaller than the screen, it is tiled
         * by the X server. Drawing it once stretched would be wrong, so it is
         * repeated instead.
         */
        glTexParameteri (data->tex_type, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri (data->tex_type, GL_TEXTURE_WRAP_T, GL_REPEAT);
        draw_quad (screen_info, data->tex_type, data->y_inverted[GL_DEPTH_RGB],
                   0, 0, tex_width, tex_height,
                   0, 0, screen_info->width, screen_info->height, rects, nrects);
        glTexParameteri (data->tex_type, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (data->tex_type, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    else
    {
        gint i;

        /* No background pixmap, plain black like the XRender path */
        glUseProgram (0);
        glColor4f (0.0f, 0.0f, 0.0f, 1.0f);
        for (i = 0; i < nrects; i++)
        {
            gfloat x1 = 2.0f * (gfloat) rects[i].x / (gfloat) screen_info->width - 1.0f;
            gfloat x2 = 2.0f * (gfloat) (rects[i].x + rects[i].width) / (gfloat) screen_info->width - 1.0f;
            gfloat y1 = 1.0f - 2.0f * (gfloat) rects[i].y / (gfloat) screen_info->height;
            gfloat y2 = 1.0f - 2.0f * (gfloat) (rects[i].y + rects[i].height) / (gfloat) screen_info->height;

            glScissor (rects[i].x,
                       screen_info->height - (rects[i].y + rects[i].height),
                       rects[i].width, rects[i].height);
            glBegin (GL_QUADS);
            glVertex2f (x1, y1);
            glVertex2f (x2, y1);
            glVertex2f (x2, y2);
            glVertex2f (x1, y2);
            glEnd ();
        }
        glUseProgram (data->program_win);
        glUniform1i (data->u_tex_win, 0);
    }
}

static void
paint_cursor_gl (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    XRectangle rect;

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
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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

    glEnable (GL_BLEND);
    glUniform1f (data->u_opacity_win, 1.0f);
    draw_quad (screen_info, GL_TEXTURE_2D, FALSE,
               0, 0, data->cursor_width, data->cursor_height,
               screen_info->cursorLocation.x, screen_info->cursorLocation.y,
               screen_info->cursorLocation.width, screen_info->cursorLocation.height,
               &rect, 1);
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
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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
    glUniform1f (data->u_opacity_win, 1.0f);

    glScissor (0, 0, screen_info->width, screen_info->height);

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

static gint
fetch_region_rects (Display *dpy, XserverRegion region, XRectangle **rects)
{
    gint nrects = 0;

    *rects = XFixesFetchRegion (dpy, region, &nrects);
    if (*rects == NULL)
    {
        return 0;
    }

    return nrects;
}

/*
 * Work out what has to be repainted this frame. With GLX_EXT_buffer_age the
 * damage of the last frames is replayed, otherwise the whole screen is
 * redrawn because the content of the back buffer is undefined after a swap.
 */
static XserverRegion
get_paint_region (ScreenInfo *screen_info, XserverRegion damage)
{
    XfwmGLData *data = gl_data (screen_info);
    Display *dpy = myScreenGetXDisplay (screen_info);
    XserverRegion region;
    guint age = 0;
    guint i;

    region = XFixesCreateRegion (dpy, NULL, 0);

    if (data->has_buffer_age && !data->full_repaint)
    {
        glXQueryDrawable (dpy, screen_info->glx_window,
                          GLX_BACK_BUFFER_AGE_EXT, &age);
    }

    if (age == 0 || age > GL_DAMAGE_HISTORY || data->full_repaint)
    {
        XRectangle r;

        r.x = 0;
        r.y = 0;
        r.width = screen_info->width;
        r.height = screen_info->height;
        XFixesSetRegion (dpy, region, &r, 1);
        data->full_repaint = FALSE;
    }
    else
    {
        XFixesCopyRegion (dpy, region, damage);
        /* Add back what the older frames in the buffer never saw */
        for (i = 0; i < age - 1; i++)
        {
            guint slot = (data->damage_index + GL_DAMAGE_HISTORY - i - 1) % GL_DAMAGE_HISTORY;

            if (data->damage_history[slot] != None)
            {
                XFixesUnionRegion (dpy, region, region, data->damage_history[slot]);
            }
        }
    }

    /* Remember this frame's damage for the next rounds */
    if (data->damage_history[data->damage_index] == None)
    {
        data->damage_history[data->damage_index] = XFixesCreateRegion (dpy, NULL, 0);
    }
    XFixesCopyRegion (dpy, data->damage_history[data->damage_index], damage);
    data->damage_index = (data->damage_index + 1) % GL_DAMAGE_HISTORY;

    return region;
}

gboolean
xfwmGLPaintAll (ScreenInfo *screen_info, XserverRegion damage)
{
    XfwmGLData *data;
    DisplayInfo *display_info;
    Display *dpy;
    XserverRegion paint_region;
    XRectangle *rects;
    GList *list;
    CWindow *cw;
    gint nrects;
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

    paint_region = get_paint_region (screen_info, damage);
    nrects = fetch_region_rects (dpy, paint_region, &rects);
    if (nrects == 0)
    {
        XFixesDestroyRegion (dpy, paint_region);
        myDisplayErrorTrapPopIgnored (display_info);

        return TRUE;
    }

    zoomed = screen_info->zoomed;
    if (zoomed && !bind_zoom_fbo (screen_info))
    {
        zoomed = FALSE;
    }

    glViewport (0, 0, screen_info->width, screen_info->height);
    glEnable (GL_SCISSOR_TEST);
    glUseProgram (data->program_win);
    glUniform1i (data->u_tex_win, 0);
    glActiveTexture (GL_TEXTURE0);
    glEnable (data->tex_type);

    /*
     * First pass, top to bottom: draw the opaque windows and take what they
     * cover out of the region left to paint.
     */
    for (list = screen_info->cwindows; list; list = g_list_next (list))
    {
        XserverRegion visible;
        XRectangle *win_rects;
        gint win_nrects;

        cw = (CWindow *) list->data;

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

        /* Refreshes the shadow of the window as a side effect */
        compositorUpdateWinExtents (cw);

        if (cw->borderSize == None)
        {
            cw->borderSize = compositorBorderSize (cw);
        }
        if (cw->clientSize == None)
        {
            cw->clientSize = compositorClientSize (cw);
        }

        /* Keep the region still to paint for the pass below */
        if (cw->borderClip == None)
        {
            cw->borderClip = XFixesCreateRegion (dpy, NULL, 0);
            XFixesCopyRegion (dpy, cw->borderClip, paint_region);
        }

        if (WIN_IS_OPAQUE(cw))
        {
            visible = XFixesCreateRegion (dpy, NULL, 0);
            XFixesIntersectRegion (dpy, visible, paint_region, cw->borderSize);
            win_nrects = fetch_region_rects (dpy, visible, &win_rects);
            if (win_nrects > 0)
            {
                glDisable (GL_BLEND);
                paint_window_gl (cw, TRUE, win_rects, win_nrects);
                XFree (win_rects);
            }
            XFixesDestroyRegion (dpy, visible);

            /*
             * Nothing below shows through an opaque window. A window with a
             * translucent frame only covers its client area.
             */
            if (WIN_HAS_FRAME(cw) && (screen_info->params->frame_opacity < 100))
            {
                XFixesSubtractRegion (dpy, paint_region, paint_region, cw->clientSize);
            }
            else
            {
                XFixesSubtractRegion (dpy, paint_region, paint_region, cw->borderSize);
            }
        }
        else if ((cw->opacity == NET_WM_OPAQUE) && !WIN_IS_SHADED(cw) &&
                 (cw->opaque_region != None))
        {
            compositorClipOpaqueRegion (cw, paint_region);
        }

        cw->skipped = FALSE;
    }

    /* The background shows wherever no opaque window is left */
    XFree (rects);
    nrects = fetch_region_rects (dpy, paint_region, &rects);
    if (nrects > 0)
    {
        paint_root_gl (screen_info, rects, nrects);
    }

    /*
     * Second pass, bottom to top: shadows and everything that is blended.
     */
    for (list = g_list_last (screen_info->cwindows); list; list = g_list_previous (list))
    {
        XRectangle *win_rects;
        gint win_nrects;

        cw = (CWindow *) list->data;
        if (cw->skipped)
        {
            continue;
        }

        if (cw->gl_shadow_texture != 0)
        {
            XserverRegion shadow_clip;

            shadow_clip = XFixesCreateRegion (dpy, NULL, 0);
            XFixesSubtractRegion (dpy, shadow_clip, cw->borderClip, cw->borderSize);
            win_nrects = fetch_region_rects (dpy, shadow_clip, &win_rects);
            if (win_nrects > 0)
            {
                paint_shadow_gl (cw, win_rects, win_nrects);
                XFree (win_rects);
            }
            XFixesDestroyRegion (dpy, shadow_clip);
        }

        if (!WIN_IS_OPAQUE(cw) ||
            (WIN_HAS_FRAME(cw) && (screen_info->params->frame_opacity < 100)))
        {
            XFixesIntersectRegion (dpy, cw->borderClip, cw->borderClip, cw->borderSize);
            win_nrects = fetch_region_rects (dpy, cw->borderClip, &win_rects);
            if (win_nrects > 0)
            {
                glEnable (GL_BLEND);
                paint_window_gl (cw, FALSE, win_rects, win_nrects);
                XFree (win_rects);
            }
        }

        if (cw->borderClip != None)
        {
            XFixesDestroyRegion (dpy, cw->borderClip);
            cw->borderClip = None;
        }
    }

    if (zoomed)
    {
        if (screen_info->cursor_is_zoomed)
        {
            paint_cursor_gl (screen_info);
        }
        draw_zoomed_scene (screen_info);
    }

    glDisable (GL_SCISSOR_TEST);
    glUseProgram (0);
    glDisable (data->tex_type);
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

    XFree (rects);
    XFixesDestroyRegion (dpy, paint_region);
    myDisplayErrorTrapPopIgnored (display_info);

    return TRUE;
}

#endif /* HAVE_EPOXY */

#endif /* HAVE_COMPOSITOR */

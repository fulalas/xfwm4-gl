/*
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
        Foundation, Inc., Inc., 51 Franklin Street, Fifth Floor, Boston,
        MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <string.h>
#include <glib.h>
#include <gdk/gdk.h>
#include <librsvg/rsvg.h>

#include "mysvg.h"
#include "mysvgparse.h"
#include "mysvgsnap.h"

#define VAR_PREFIX "var(--"
#define VAR_PREFIX_LEN (sizeof (VAR_PREFIX) - 1)
#define VAR_OPEN_PAREN 3
#define VAR_SKIP 4
#define CHANNEL_MAX 255

typedef struct
{
    const gchar *start;
    const gchar *comma;
    const gchar *end;
}
VarRef;

typedef struct
{
    GString *out;
    xfwmColorSymbol *color_sym;
}
Colorizer;

typedef struct
{
    gdouble width;
    gdouble height;
    gdouble box[4];
    gboolean has_box;
}
SvgSize;

static gchar *apply_colors (const gchar *, xfwmColorSymbol *);
static const gchar *var_end (const gchar *, const gchar **);
static const gchar *replace_var (Colorizer *, const gchar *, const gchar *);
static void append_var (Colorizer *, const VarRef *);
static const gchar *var_value (const VarRef *, xfwmColorSymbol *);
static void append_rgb (GString *, const GdkRGBA *);
static guint channel_byte (gdouble);
static void append_stripped (GString *, const gchar *, const gchar *);
static GdkPixbuf *load_colored (const gchar *, gdouble);
static void set_scale (SvgAxisScale *, gdouble);
static gboolean get_size (const gchar *, SvgCanvas *, gboolean *);
static gboolean read_size (const gchar *, SvgSize *);
static gboolean size_from_tag (GArray *, SvgSize *);
static gboolean size_from_rsvg (const gchar *, SvgSize *);
static gboolean box_matches (const gdouble *, const SvgCanvas *);
static GdkPixbuf *render (const gchar *, const SvgCanvas *);
static gboolean render_document (RsvgHandle *, cairo_surface_t *, const SvgCanvas *);

GdkPixbuf *
xfwmSvgLoad (const gchar *filename, xfwmColorSymbol *color_sym, gdouble scale)
{
    GdkPixbuf *pixbuf;
    gchar *data, *colored;

    if (!g_file_get_contents (filename, &data, NULL, NULL))
    {
        return NULL;
    }
    colored = apply_colors (data, color_sym);
    g_free (data);
    pixbuf = load_colored (colored, scale);
    g_free (colored);

    return pixbuf;
}

const gchar *
xfwmColorSymbolFind (const gchar *name, xfwmColorSymbol *color_sym)
{
    xfwmColorSymbol *i;

    for (i = color_sym; i && i->name; ++i)
    {
        if (!g_ascii_strcasecmp (i->name, name))
        {
            return i->value;
        }
    }
    return NULL;
}

static gchar *
apply_colors (const gchar *data, xfwmColorSymbol *color_sym)
{
    Colorizer colorizer;
    const gchar *p, *start;

    colorizer.out = g_string_new (NULL);
    colorizer.color_sym = color_sym;
    p = data;
    while ((start = strstr (p, VAR_PREFIX)))
    {
        p = replace_var (&colorizer, p, start);
    }
    g_string_append (colorizer.out, p);

    return g_string_free (colorizer.out, FALSE);
}

static const gchar *
replace_var (Colorizer *colorizer, const gchar *p, const gchar *start)
{
    VarRef var;

    var.start = start;
    var.end = var_end (start + VAR_OPEN_PAREN, &var.comma);
    if (!var.end)
    {
        g_string_append_len (colorizer->out, p, start + VAR_SKIP - p);
        return start + VAR_SKIP;
    }
    g_string_append_len (colorizer->out, p, start - p);
    append_var (colorizer, &var);

    return var.end + 1;
}

static const gchar *
var_end (const gchar *p, const gchar **comma)
{
    gint depth;

    depth = 0;
    *comma = NULL;
    for (; *p; p++)
    {
        if (*p == '(')
        {
            depth++;
        }
        else if (*p == ')' && --depth == 0)
        {
            return p;
        }
        else if (*p == ',' && depth == 1 && !*comma)
        {
            *comma = p;
        }
    }
    return NULL;
}

static void
append_var (Colorizer *colorizer, const VarRef *var)
{
    const gchar *value;
    GdkRGBA rgba;

    value = var_value (var, colorizer->color_sym);
    if (value && gdk_rgba_parse (&rgba, value))
    {
        append_rgb (colorizer->out, &rgba);
    }
    else if (var->comma)
    {
        append_stripped (colorizer->out, var->comma + 1, var->end);
    }
    else
    {
        g_string_append_len (colorizer->out, var->start, var->end - var->start + 1);
    }
}

static const gchar *
var_value (const VarRef *var, xfwmColorSymbol *color_sym)
{
    const gchar *name_start, *name_end, *value;
    gchar *name;

    name_start = var->start + VAR_PREFIX_LEN;
    name_end = var->comma ? var->comma : var->end;
    name = g_strstrip (g_strndup (name_start, name_end - name_start));
    value = xfwmColorSymbolFind (name, color_sym);
    g_free (name);

    return value;
}

static void
append_rgb (GString *out, const GdkRGBA *rgba)
{
    g_string_append_printf (out, "#%02x%02x%02x",
                            channel_byte (rgba->red),
                            channel_byte (rgba->green),
                            channel_byte (rgba->blue));
}

static guint
channel_byte (gdouble channel)
{
    return (guint) lround (channel * CHANNEL_MAX);
}

static void
append_stripped (GString *out, const gchar *start, const gchar *end)
{
    gchar *text;

    text = g_strstrip (g_strndup (start, end - start));
    g_string_append (out, text);
    g_free (text);
}

static GdkPixbuf *
load_colored (const gchar *data, gdouble scale)
{
    SvgCanvas canvas;
    GdkPixbuf *pixbuf;
    gchar *snapped;
    gboolean snap;

    if (!get_size (data, &canvas, &snap))
    {
        return NULL;
    }
    set_scale (&canvas.x, scale);
    set_scale (&canvas.y, scale);
    if (!snap)
    {
        return render (data, &canvas);
    }
    snapped = svgSnap (data, &canvas);
    pixbuf = render (snapped, &canvas);
    g_free (snapped);

    return pixbuf;
}

static void
set_scale (SvgAxisScale *axis, gdouble scale)
{
    axis->scale = scale;
    axis->device_size = MAX (1, lround (axis->size * scale));
}

static gboolean
get_size (const gchar *data, SvgCanvas *canvas, gboolean *snap)
{
    SvgSize size = { 0 };

    if (!read_size (data, &size))
    {
        return FALSE;
    }
    canvas->x.size = lround (size.width);
    canvas->y.size = lround (size.height);
    *snap = !size.has_box || box_matches (size.box, canvas);

    return (canvas->x.size > 0 && canvas->y.size > 0);
}

static gboolean
read_size (const gchar *data, SvgSize *size)
{
    const gchar *tag, *end;
    GArray *attrs;
    gboolean ok;

    tag = svgNextTag (data, "<svg", &end);
    if (!tag)
    {
        return FALSE;
    }
    attrs = svgTagAttrs (tag, end);
    ok = size_from_tag (attrs, size);
    svgAttrsFree (attrs);

    return ok || size_from_rsvg (data, size);
}

static gboolean
size_from_tag (GArray *attrs, SvgSize *size)
{
    const gchar *w_attr, *h_attr;

    size->has_box = svgParseViewbox (svgAttrGet (attrs, "viewBox"), size->box);
    w_attr = svgAttrGet (attrs, "width");
    h_attr = svgAttrGet (attrs, "height");
    if (w_attr && h_attr)
    {
        return svgParseLength (w_attr, &size->width) && svgParseLength (h_attr, &size->height);
    }
    size->width = size->box[2];
    size->height = size->box[3];

    return !w_attr && !h_attr && size->has_box;
}

static gboolean
size_from_rsvg (const gchar *data, SvgSize *size)
{
    RsvgHandle *handle;
    gboolean ok;

    handle = rsvg_handle_new_from_data ((const guint8 *) data, strlen (data), NULL);
    if (!handle)
    {
        return FALSE;
    }
    ok = rsvg_handle_get_intrinsic_size_in_pixels (handle, &size->width, &size->height);
    g_object_unref (handle);

    return ok;
}

static gboolean
box_matches (const gdouble *box, const SvgCanvas *canvas)
{
    return box[0] == 0 && box[1] == 0 && box[2] == canvas->x.size && box[3] == canvas->y.size;
}

static GdkPixbuf *
render (const gchar *data, const SvgCanvas *canvas)
{
    RsvgHandle *handle;
    cairo_surface_t *surface;
    GdkPixbuf *pixbuf;

    handle = rsvg_handle_new_from_data ((const guint8 *) data, strlen (data), NULL);
    if (!handle)
    {
        return NULL;
    }
    surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, canvas->x.device_size, canvas->y.device_size);
    pixbuf = NULL;
    if (render_document (handle, surface, canvas))
    {
        pixbuf = gdk_pixbuf_get_from_surface (surface, 0, 0, canvas->x.device_size, canvas->y.device_size);
    }
    g_object_unref (handle);
    cairo_surface_destroy (surface);

    return pixbuf;
}

static gboolean
render_document (RsvgHandle *handle, cairo_surface_t *surface, const SvgCanvas *canvas)
{
    RsvgRectangle viewport = { 0, 0, canvas->x.size, canvas->y.size };
    cairo_t *cr;
    gboolean ok;

    cr = cairo_create (surface);
    cairo_scale (cr, (gdouble) canvas->x.device_size / canvas->x.size,
                 (gdouble) canvas->y.device_size / canvas->y.size);
    ok = rsvg_handle_render_document (handle, cr, &viewport, NULL);
    cairo_destroy (cr);

    return ok;
}

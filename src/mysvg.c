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

static gchar *
apply_colors (const gchar *data, xfwmColorSymbol *color_sym)
{
    GString *out;
    const gchar *p, *start, *end, *comma;
    const gchar *value;
    gchar *name, *fallback;
    GdkRGBA rgba;

    out = g_string_new (NULL);
    p = data;
    while ((start = strstr (p, "var(--")))
    {
        end = var_end (start + 3, &comma);
        if (!end)
        {
            g_string_append_len (out, p, start + 4 - p);
            p = start + 4;
            continue;
        }
        g_string_append_len (out, p, start - p);
        name = g_strstrip (g_strndup (start + 6, (comma ? comma : end) - start - 6));
        value = xfwmColorSymbolFind (name, color_sym);
        if (value && gdk_rgba_parse (&rgba, value))
        {
            g_string_append_printf (out, "#%02x%02x%02x",
                                    (guint) lround (rgba.red * 255),
                                    (guint) lround (rgba.green * 255),
                                    (guint) lround (rgba.blue * 255));
        }
        else if (comma)
        {
            fallback = g_strstrip (g_strndup (comma + 1, end - comma - 1));
            g_string_append (out, fallback);
            g_free (fallback);
        }
        else
        {
            g_string_append_len (out, start, end - start + 1);
        }
        g_free (name);
        p = end + 1;
    }
    g_string_append (out, p);

    return g_string_free (out, FALSE);
}

static gboolean
parse_length (const gchar *s, gdouble *v)
{
    gchar *end;

    *v = g_ascii_strtod (s, &end);
    if (end == s)
    {
        return FALSE;
    }
    if (g_str_has_prefix (end, "px"))
    {
        end += 2;
    }
    while (g_ascii_isspace (*end))
    {
        end++;
    }
    return (*end == '\0');
}

typedef struct
{
    gchar *name;
    gchar *value;
    gchar quote;
}
Attr;

static GArray *
parse_attrs (const gchar *p, const gchar *end)
{
    GArray *attrs;
    const gchar *name, *value;
    gchar quote;
    Attr attr;

    attrs = g_array_new (FALSE, FALSE, sizeof (Attr));
    while (p < end)
    {
        while (p < end && (g_ascii_isspace (*p) || *p == '/'))
        {
            p++;
        }
        name = p;
        while (p < end && *p != '=' && !g_ascii_isspace (*p))
        {
            p++;
        }
        if (p == name)
        {
            break;
        }
        attr.name = g_strndup (name, p - name);
        while (p < end && (g_ascii_isspace (*p) || *p == '='))
        {
            p++;
        }
        if (p >= end || (*p != '"' && *p != '\''))
        {
            g_free (attr.name);
            break;
        }
        quote = *p++;
        value = p;
        while (p < end && *p != quote)
        {
            p++;
        }
        attr.value = g_strndup (value, p - value);
        attr.quote = quote;
        g_array_append_val (attrs, attr);
        p++;
    }
    return attrs;
}

static void
free_attrs (GArray *attrs)
{
    guint i;

    for (i = 0; i < attrs->len; i++)
    {
        g_free (g_array_index (attrs, Attr, i).name);
        g_free (g_array_index (attrs, Attr, i).value);
    }
    g_array_free (attrs, TRUE);
}

static gchar *
attr_get (GArray *attrs, const gchar *name)
{
    guint i;

    for (i = 0; i < attrs->len; i++)
    {
        if (!strcmp (g_array_index (attrs, Attr, i).name, name))
        {
            return g_array_index (attrs, Attr, i).value;
        }
    }
    return NULL;
}

static gboolean
attr_number (GArray *attrs, const gchar *name, gdouble *v)
{
    const gchar *value;

    value = attr_get (attrs, name);
    if (!value)
    {
        *v = 0;
        return TRUE;
    }
    return parse_length (value, v);
}

typedef struct
{
    gdouble from;
    gdouble to;
}
Knot;

typedef struct
{
    gdouble pos;
    gdouble len;
}
Span;

typedef struct
{
    GArray *knots;
    gdouble k;
}
Axis;

typedef struct
{
    Axis axis[2];
}
Layout;

static const gchar *
next_tag (const gchar *p, const gchar *name, const gchar **end)
{
    const gchar *tag;
    gsize len;
    gchar quote;

    len = strlen (name);
    while ((tag = strstr (p, name)))
    {
        p = tag + len;
        if (!g_ascii_isspace (*p))
        {
            continue;
        }
        quote = 0;
        for (; *p && (quote || *p != '>'); p++)
        {
            if (quote && *p == quote)
            {
                quote = 0;
            }
            else if (!quote && (*p == '"' || *p == '\''))
            {
                quote = *p;
            }
        }
        if (!*p)
        {
            return NULL;
        }
        *end = p;
        return tag;
    }
    return NULL;
}

static gboolean
rect_geometry (GArray *attrs, gdouble *x, gdouble *y, gdouble *w, gdouble *h)
{
    const gchar *style;
    const gchar *render;

    render = attr_get (attrs, "shape-rendering");
    style = attr_get (attrs, "style");
    if (!(render && !strcmp (render, "crispEdges")) && !(style && strstr (style, "crispEdges")))
    {
        return FALSE;
    }
    return attr_number (attrs, "x", x) && attr_number (attrs, "y", y) &&
           attr_number (attrs, "width", w) && attr_number (attrs, "height", h) &&
           *w > 0 && *h > 0 && !attr_get (attrs, "transform");
}

static gint
knot_find (GArray *knots, gdouble from)
{
    guint i;

    for (i = 0; i < knots->len; i++)
    {
        if (ABS (g_array_index (knots, Knot, i).from - from) < 1e-6)
        {
            return i;
        }
    }
    return -1;
}

static gdouble
knot_get (GArray *knots, gdouble from, gdouble fallback)
{
    gint i;

    i = knot_find (knots, from);
    return (i < 0) ? fallback : g_array_index (knots, Knot, i).to;
}

static void
knot_set (GArray *knots, gdouble from, gdouble to)
{
    Knot knot;

    if (knot_find (knots, from) < 0)
    {
        knot.from = from;
        knot.to = to;
        g_array_append_val (knots, knot);
    }
}

static gint
compare_span_up (gconstpointer a, gconstpointer b)
{
    const Span *sa = a, *sb = b;

    return (sa->pos > sb->pos) - (sa->pos < sb->pos);
}

static gint
compare_span_down (gconstpointer a, gconstpointer b)
{
    const Span *sa = a, *sb = b;

    return (sa->pos + sa->len < sb->pos + sb->len) - (sa->pos + sa->len > sb->pos + sb->len);
}

static gint
compare_knot (gconstpointer a, gconstpointer b)
{
    const Knot *ka = a, *kb = b;

    return (ka->from > kb->from) - (ka->from < kb->from);
}

static gdouble
axis_snap (gdouble v, gint size, gint device_size, gdouble scale)
{
    if (v * 2 <= size)
    {
        return lround (v * scale);
    }
    return device_size - lround ((size - v) * scale);
}

static void
axis_build (Axis *axis, GArray *spans, gint size, gint device_size, gdouble scale)
{
    GArray *lines;
    Span *span;
    gdouble a, b;
    guint i;

    axis->k = (gdouble) device_size / size;
    axis->knots = g_array_new (FALSE, FALSE, sizeof (Knot));
    knot_set (axis->knots, 0, 0);
    knot_set (axis->knots, size, device_size);

    lines = g_array_new (FALSE, FALSE, sizeof (Span));
    for (i = 0; i < spans->len; i++)
    {
        span = &g_array_index (spans, Span, i);
        if (span->len <= 1.0 && span->pos * 2 + span->len < size)
        {
            g_array_append_val (lines, *span);
        }
    }
    g_array_sort (lines, compare_span_up);
    for (i = 0; i < lines->len; i++)
    {
        span = &g_array_index (lines, Span, i);
        a = knot_get (axis->knots, span->pos, axis_snap (span->pos, size, device_size, scale));
        knot_set (axis->knots, span->pos, a);
        knot_set (axis->knots, span->pos + span->len, a + 1);
    }
    g_array_set_size (lines, 0);

    for (i = 0; i < spans->len; i++)
    {
        span = &g_array_index (spans, Span, i);
        if (span->len <= 1.0 && span->pos * 2 + span->len >= size)
        {
            g_array_append_val (lines, *span);
        }
    }
    g_array_sort (lines, compare_span_down);
    for (i = 0; i < lines->len; i++)
    {
        span = &g_array_index (lines, Span, i);
        b = knot_get (axis->knots, span->pos + span->len,
                      axis_snap (span->pos + span->len, size, device_size, scale));
        knot_set (axis->knots, span->pos + span->len, b);
        knot_set (axis->knots, span->pos, b - 1);
    }
    g_array_free (lines, TRUE);

    for (i = 0; i < spans->len; i++)
    {
        span = &g_array_index (spans, Span, i);
        knot_set (axis->knots, span->pos, axis_snap (span->pos, size, device_size, scale));
        knot_set (axis->knots, span->pos + span->len,
                  axis_snap (span->pos + span->len, size, device_size, scale));
    }

    g_array_sort (axis->knots, compare_knot);
    for (i = 1; i < axis->knots->len; i++)
    {
        Knot *prev = &g_array_index (axis->knots, Knot, i - 1);
        Knot *knot = &g_array_index (axis->knots, Knot, i);

        knot->to = CLAMP (knot->to, prev->to, device_size);
    }
}

static gdouble
axis_map (Axis *axis, gdouble v)
{
    Knot *lo, *hi;
    guint i;

    lo = &g_array_index (axis->knots, Knot, 0);
    if (v <= lo->from)
    {
        return (lo->to + (v - lo->from) * axis->k) / axis->k;
    }
    for (i = 1; i < axis->knots->len; i++)
    {
        hi = &g_array_index (axis->knots, Knot, i);
        if (v <= hi->from)
        {
            return (lo->to + (v - lo->from) * (hi->to - lo->to) / (hi->from - lo->from)) / axis->k;
        }
        lo = hi;
    }
    return (lo->to + (v - lo->from) * axis->k) / axis->k;
}

static void
append_number (GString *out, gdouble v)
{
    gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

    g_string_append (out, g_ascii_dtostr (buf, sizeof (buf), v));
}

static gchar *
map_path (Layout *layout, const gchar *d)
{
    static const gchar *commands = "MLHVCSQTAZ";
    static const gchar *patterns[] = { "xy", "xy", "x", "y", "xyxyxy", "xyxy", "xyxy", "xy", "nnnnnxy", "" };
    GString *out;
    const gchar *p, *pattern, *arg, *c;
    gchar *end;
    gdouble v;

    out = g_string_new (NULL);
    p = d;
    pattern = arg = NULL;
    while (*p)
    {
        if (g_ascii_isspace (*p) || *p == ',')
        {
            p++;
            continue;
        }
        if (g_ascii_isalpha (*p))
        {
            c = strchr (commands, (*p == 'z') ? 'Z' : *p);
            if (!c)
            {
                g_string_free (out, TRUE);
                return NULL;
            }
            pattern = arg = patterns[c - commands];
            g_string_append_c (out, *p);
            p++;
            continue;
        }
        if (!pattern || !*pattern)
        {
            g_string_free (out, TRUE);
            return NULL;
        }
        v = g_ascii_strtod (p, &end);
        if (end == p)
        {
            g_string_free (out, TRUE);
            return NULL;
        }
        p = end;
        if (!*arg)
        {
            arg = pattern;
        }
        if (!g_ascii_isalpha (out->str[out->len - 1]))
        {
            g_string_append_c (out, ' ');
        }
        append_number (out, (*arg == 'n') ? v : axis_map (&layout->axis[*arg == 'y'], v));
        arg++;
    }
    return g_string_free (out, FALSE);
}

static void
append_attrs (GString *out, GArray *attrs, const gchar **skip)
{
    const gchar **name;
    guint i;

    for (i = 0; i < attrs->len; i++)
    {
        Attr *attr = &g_array_index (attrs, Attr, i);

        for (name = skip; *name && strcmp (*name, attr->name); name++)
        {
        }
        if (!*name)
        {
            g_string_append_printf (out, " %s=%c%s%c", attr->name, attr->quote, attr->value, attr->quote);
        }
    }
}

static gboolean
rewrite_rect (GString *out, Layout *layout, GArray *attrs)
{
    static const gchar *skip[] = { "x", "y", "width", "height", NULL };
    gdouble x, y, w, h, x0, y0;

    if (!rect_geometry (attrs, &x, &y, &w, &h))
    {
        return FALSE;
    }
    x0 = axis_map (&layout->axis[0], x);
    y0 = axis_map (&layout->axis[1], y);
    w = axis_map (&layout->axis[0], x + w) - x0;
    h = axis_map (&layout->axis[1], y + h) - y0;

    g_string_append (out, "<rect");
    append_attrs (out, attrs, skip);
    g_string_append (out, " x=\"");
    append_number (out, x0);
    g_string_append (out, "\" y=\"");
    append_number (out, y0);
    g_string_append (out, "\" width=\"");
    append_number (out, w);
    g_string_append (out, "\" height=\"");
    append_number (out, h);
    g_string_append (out, "\"/>");

    return TRUE;
}

static gboolean
rewrite_path (GString *out, Layout *layout, GArray *attrs, gboolean closed)
{
    static const gchar *skip[] = { "d", NULL };
    const gchar *d;
    gchar *mapped;

    d = attr_get (attrs, "d");
    if (!d || attr_get (attrs, "transform") || !(mapped = map_path (layout, d)))
    {
        return FALSE;
    }

    g_string_append (out, "<path");
    append_attrs (out, attrs, skip);
    g_string_append_printf (out, " d=\"%s\"%s>", mapped, closed ? "/" : "");
    g_free (mapped);

    return TRUE;
}

static void
layout_collect (Layout *layout, const gchar *data, gint width, gint height,
                gint device_width, gint device_height, gdouble scale)
{
    const gchar *p, *tag, *end;
    GArray *attrs;
    GArray *spans[2];
    gdouble x, y, w, h;
    Span span;

    spans[0] = g_array_new (FALSE, FALSE, sizeof (Span));
    spans[1] = g_array_new (FALSE, FALSE, sizeof (Span));
    p = data;
    while ((tag = next_tag (p, "<rect", &end)))
    {
        attrs = parse_attrs (tag + 5, end);
        if (*(end - 1) == '/' && rect_geometry (attrs, &x, &y, &w, &h))
        {
            span.pos = x;
            span.len = w;
            g_array_append_val (spans[0], span);
            span.pos = y;
            span.len = h;
            g_array_append_val (spans[1], span);
        }
        free_attrs (attrs);
        p = end + 1;
    }
    axis_build (&layout->axis[0], spans[0], width, device_width, scale);
    axis_build (&layout->axis[1], spans[1], height, device_height, scale);
    g_array_free (spans[0], TRUE);
    g_array_free (spans[1], TRUE);
}

static void
layout_free (Layout *layout)
{
    g_array_free (layout->axis[0].knots, TRUE);
    g_array_free (layout->axis[1].knots, TRUE);
}

static gchar *
layout_apply (Layout *layout, const gchar *data)
{
    GString *out;
    const gchar *p, *rect, *path, *tag, *end, *rect_end, *path_end;
    GArray *attrs;
    gboolean closed, done;

    out = g_string_new (NULL);
    p = data;
    for (;;)
    {
        rect = next_tag (p, "<rect", &rect_end);
        path = next_tag (p, "<path", &path_end);
        if (!rect && !path)
        {
            break;
        }
        if (rect && (!path || rect < path))
        {
            tag = rect;
            end = rect_end;
        }
        else
        {
            tag = path;
            end = path_end;
        }
        g_string_append_len (out, p, tag - p);
        closed = (*(end - 1) == '/');
        attrs = parse_attrs (tag + 5, closed ? end - 1 : end);
        if (tag == rect)
        {
            done = closed && rewrite_rect (out, layout, attrs);
        }
        else
        {
            done = rewrite_path (out, layout, attrs, closed);
        }
        if (!done)
        {
            g_string_append_len (out, tag, end - tag + 1);
        }
        free_attrs (attrs);
        p = end + 1;
    }
    g_string_append (out, p);

    return g_string_free (out, FALSE);
}

static gboolean
parse_viewbox (const gchar *viewbox, gdouble *box)
{
    gchar *next;
    gint i;

    if (!viewbox)
    {
        return FALSE;
    }
    for (i = 0; i < 4; i++)
    {
        while (g_ascii_isspace (*viewbox) || *viewbox == ',')
        {
            viewbox++;
        }
        box[i] = g_ascii_strtod (viewbox, &next);
        if (next == viewbox)
        {
            return FALSE;
        }
        viewbox = next;
    }
    return TRUE;
}

static gboolean
get_size (const gchar *data, gint *width, gint *height, gboolean *snap)
{
    const gchar *tag, *end, *w_attr, *h_attr;
    RsvgHandle *handle;
    GArray *attrs;
    gdouble box[4] = { 0 }, w, h;
    gboolean has_box, ok;

    tag = next_tag (data, "<svg", &end);
    if (!tag)
    {
        return FALSE;
    }
    attrs = parse_attrs (tag + 4, end);
    has_box = parse_viewbox (attr_get (attrs, "viewBox"), box);
    w_attr = attr_get (attrs, "width");
    h_attr = attr_get (attrs, "height");
    if (w_attr && h_attr)
    {
        ok = parse_length (w_attr, &w) && parse_length (h_attr, &h);
    }
    else
    {
        ok = !w_attr && !h_attr && has_box;
        w = box[2];
        h = box[3];
    }
    free_attrs (attrs);

    if (!ok)
    {
        handle = rsvg_handle_new_from_data ((const guint8 *) data, strlen (data), NULL);
        ok = handle && rsvg_handle_get_intrinsic_size_in_pixels (handle, &w, &h);
        if (handle)
        {
            g_object_unref (handle);
        }
        if (!ok)
        {
            return FALSE;
        }
    }
    *width = lround (w);
    *height = lround (h);
    *snap = !has_box || (box[0] == 0 && box[1] == 0 && box[2] == *width && box[3] == *height);

    return (*width > 0 && *height > 0);
}

GdkPixbuf *
xfwmSvgLoad (const gchar *filename, xfwmColorSymbol *color_sym, gdouble scale)
{
    RsvgHandle *handle;
    RsvgRectangle viewport;
    Layout layout;
    cairo_surface_t *surface;
    cairo_t *cr;
    GdkPixbuf *pixbuf;
    gchar *data, *colored, *snapped;
    gint width, height, device_width, device_height;
    gboolean snap, ok;

    if (!g_file_get_contents (filename, &data, NULL, NULL))
    {
        return NULL;
    }
    colored = apply_colors (data, color_sym);
    g_free (data);

    if (!get_size (colored, &width, &height, &snap))
    {
        g_free (colored);
        return NULL;
    }

    device_width = MAX (1, lround (width * scale));
    device_height = MAX (1, lround (height * scale));
    if (snap)
    {
        layout_collect (&layout, colored, width, height, device_width, device_height, scale);
        snapped = layout_apply (&layout, colored);
        layout_free (&layout);
        g_free (colored);
    }
    else
    {
        snapped = colored;
    }

    handle = rsvg_handle_new_from_data ((const guint8 *) snapped, strlen (snapped), NULL);
    g_free (snapped);
    if (!handle)
    {
        return NULL;
    }

    surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, device_width, device_height);
    cr = cairo_create (surface);
    cairo_scale (cr, (gdouble) device_width / width, (gdouble) device_height / height);
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = width;
    viewport.height = height;
    ok = rsvg_handle_render_document (handle, cr, &viewport, NULL);
    cairo_destroy (cr);
    g_object_unref (handle);

    pixbuf = NULL;
    if (ok)
    {
        pixbuf = gdk_pixbuf_get_from_surface (surface, 0, 0, device_width, device_height);
    }
    cairo_surface_destroy (surface);

    return pixbuf;
}

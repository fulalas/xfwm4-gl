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

#include "mysvgparse.h"
#include "mysvgsnap.h"

typedef struct
{
    SvgAxis axis[2];
}
Layout;

typedef struct
{
    gdouble x;
    gdouble y;
    gdouble width;
    gdouble height;
}
Box;

typedef struct
{
    const gchar *start;
    const gchar *end;
    gboolean is_rect;
}
Shape;

typedef struct
{
    const Layout *layout;
    const gchar *pattern;
    const gchar *arg;
    GString *out;
}
PathState;

static void layout_collect (Layout *, const gchar *, const SvgCanvas *);
static void collect_spans (const gchar *, GArray **);
static void add_box_spans (GArray **, const Box *);
static void layout_free (Layout *);
static gchar *layout_apply (const Layout *, const gchar *);
static gboolean next_shape (const gchar *, Shape *);
static void rewrite_shape (GString *, const Layout *, const Shape *);
static gboolean rewrite_rect (GString *, const Layout *, GArray *);
static gboolean rewrite_path (GString *, const Layout *, GArray *);
static gboolean rect_geometry (GArray *, Box *);
static gboolean is_crisp (GArray *);
static Box map_box (const Layout *, const Box *);
static gchar *map_path (const Layout *, const gchar *);
static const gchar *map_token (PathState *, const gchar *);
static const gchar *map_command (PathState *, const gchar *);
static const gchar *map_number (PathState *, const gchar *);
static gdouble map_next_arg (PathState *, gdouble);
static void append_number_attr (GString *, const gchar *, gdouble);
static void append_number (GString *, gdouble);

gchar *
svgSnap (const gchar *data, const SvgCanvas *canvas)
{
    Layout layout;
    gchar *snapped;

    layout_collect (&layout, data, canvas);
    snapped = layout_apply (&layout, data);
    layout_free (&layout);

    return snapped;
}

static void
layout_collect (Layout *layout, const gchar *data, const SvgCanvas *canvas)
{
    GArray *spans[2];

    spans[0] = g_array_new (FALSE, FALSE, sizeof (SvgSpan));
    spans[1] = g_array_new (FALSE, FALSE, sizeof (SvgSpan));
    collect_spans (data, spans);
    svgAxisBuild (&layout->axis[0], spans[0], &canvas->x);
    svgAxisBuild (&layout->axis[1], spans[1], &canvas->y);
    g_array_free (spans[0], TRUE);
    g_array_free (spans[1], TRUE);
}

static void
collect_spans (const gchar *data, GArray **spans)
{
    const gchar *p, *tag, *end;
    GArray *attrs;
    Box box;

    p = data;
    while ((tag = svgNextTag (p, "<rect", &end)))
    {
        attrs = svgTagAttrs (tag, end);
        if (*(end - 1) == '/' && rect_geometry (attrs, &box))
        {
            add_box_spans (spans, &box);
        }
        svgAttrsFree (attrs);
        p = end + 1;
    }
}

static void
add_box_spans (GArray **spans, const Box *box)
{
    SvgSpan span;

    span.pos = box->x;
    span.len = box->width;
    g_array_append_val (spans[0], span);
    span.pos = box->y;
    span.len = box->height;
    g_array_append_val (spans[1], span);
}

static void
layout_free (Layout *layout)
{
    svgAxisFree (&layout->axis[0]);
    svgAxisFree (&layout->axis[1]);
}

static gchar *
layout_apply (const Layout *layout, const gchar *data)
{
    GString *out;
    const gchar *p;
    Shape shape;

    out = g_string_new (NULL);
    p = data;
    while (next_shape (p, &shape))
    {
        g_string_append_len (out, p, shape.start - p);
        rewrite_shape (out, layout, &shape);
        p = shape.end + 1;
    }
    g_string_append (out, p);

    return g_string_free (out, FALSE);
}

static gboolean
next_shape (const gchar *p, Shape *shape)
{
    const gchar *rect, *path, *rect_end, *path_end;

    rect = svgNextTag (p, "<rect", &rect_end);
    path = svgNextTag (p, "<path", &path_end);
    if (!rect && !path)
    {
        return FALSE;
    }
    shape->is_rect = rect && (!path || rect < path);
    shape->start = shape->is_rect ? rect : path;
    shape->end = shape->is_rect ? rect_end : path_end;
    return TRUE;
}

static void
rewrite_shape (GString *out, const Layout *layout, const Shape *shape)
{
    GArray *attrs;
    gboolean closed, done;

    closed = (*(shape->end - 1) == '/');
    attrs = svgTagAttrs (shape->start, closed ? shape->end - 1 : shape->end);
    if (shape->is_rect)
    {
        done = closed && rewrite_rect (out, layout, attrs);
    }
    else
    {
        done = rewrite_path (out, layout, attrs);
    }
    svgAttrsFree (attrs);
    if (!done)
    {
        g_string_append_len (out, shape->start, shape->end - shape->start + 1);
        return;
    }
    g_string_append (out, closed ? "/>" : ">");
}

static gboolean
rewrite_rect (GString *out, const Layout *layout, GArray *attrs)
{
    static const gchar *const skip[] = { "x", "y", "width", "height", NULL };
    Box box;

    if (!rect_geometry (attrs, &box))
    {
        return FALSE;
    }
    box = map_box (layout, &box);
    g_string_append (out, "<rect");
    svgAppendAttrs (out, attrs, skip);
    append_number_attr (out, "x", box.x);
    append_number_attr (out, "y", box.y);
    append_number_attr (out, "width", box.width);
    append_number_attr (out, "height", box.height);

    return TRUE;
}

static gboolean
rewrite_path (GString *out, const Layout *layout, GArray *attrs)
{
    static const gchar *const skip[] = { "d", NULL };
    const gchar *d;
    gchar *mapped;

    d = svgAttrGet (attrs, "d");
    if (!d || svgAttrGet (attrs, "transform") || !(mapped = map_path (layout, d)))
    {
        return FALSE;
    }
    g_string_append (out, "<path");
    svgAppendAttrs (out, attrs, skip);
    g_string_append_printf (out, " d=\"%s\"", mapped);
    g_free (mapped);

    return TRUE;
}

static gboolean
rect_geometry (GArray *attrs, Box *box)
{
    if (!is_crisp (attrs))
    {
        return FALSE;
    }
    return svgAttrNumber (attrs, "x", &box->x) && svgAttrNumber (attrs, "y", &box->y) &&
           svgAttrNumber (attrs, "width", &box->width) && svgAttrNumber (attrs, "height", &box->height) &&
           box->width > 0 && box->height > 0 && !svgAttrGet (attrs, "transform");
}

static gboolean
is_crisp (GArray *attrs)
{
    const gchar *render, *style;

    render = svgAttrGet (attrs, "shape-rendering");
    style = svgAttrGet (attrs, "style");
    return (render && !strcmp (render, "crispEdges")) || (style && strstr (style, "crispEdges"));
}

static Box
map_box (const Layout *layout, const Box *box)
{
    Box mapped;

    mapped.x = svgAxisMap (&layout->axis[0], box->x);
    mapped.y = svgAxisMap (&layout->axis[1], box->y);
    mapped.width = svgAxisMap (&layout->axis[0], box->x + box->width) - mapped.x;
    mapped.height = svgAxisMap (&layout->axis[1], box->y + box->height) - mapped.y;

    return mapped;
}

static gchar *
map_path (const Layout *layout, const gchar *d)
{
    PathState state = { layout, NULL, NULL, NULL };
    const gchar *p;

    state.out = g_string_new (NULL);
    p = d;
    while (p && *p)
    {
        p = map_token (&state, p);
    }
    if (!p)
    {
        g_string_free (state.out, TRUE);
        return NULL;
    }
    return g_string_free (state.out, FALSE);
}

static const gchar *
map_token (PathState *state, const gchar *p)
{
    if (g_ascii_isspace (*p) || *p == ',')
    {
        return p + 1;
    }
    if (g_ascii_isalpha (*p))
    {
        return map_command (state, p);
    }
    return map_number (state, p);
}

static const gchar *
map_command (PathState *state, const gchar *p)
{
    static const gchar commands[] = "MLHVCSQTAZ";
    static const gchar *const patterns[] = { "xy", "xy", "x", "y", "xyxyxy", "xyxy", "xyxy", "xy", "nnnnnxy", "" };
    const gchar *c;

    c = strchr (commands, (*p == 'z') ? 'Z' : *p);
    if (!c)
    {
        return NULL;
    }
    state->pattern = state->arg = patterns[c - commands];
    g_string_append_c (state->out, *p);

    return p + 1;
}

static const gchar *
map_number (PathState *state, const gchar *p)
{
    gchar *end;
    gdouble v;

    if (!state->pattern || !*state->pattern)
    {
        return NULL;
    }
    v = g_ascii_strtod (p, &end);
    if (end == p)
    {
        return NULL;
    }
    if (!g_ascii_isalpha (state->out->str[state->out->len - 1]))
    {
        g_string_append_c (state->out, ' ');
    }
    append_number (state->out, map_next_arg (state, v));

    return end;
}

static gdouble
map_next_arg (PathState *state, gdouble v)
{
    gchar arg;

    if (!*state->arg)
    {
        state->arg = state->pattern;
    }
    arg = *state->arg++;
    if (arg == 'n')
    {
        return v;
    }
    return svgAxisMap (&state->layout->axis[arg == 'y'], v);
}

static void
append_number_attr (GString *out, const gchar *name, gdouble v)
{
    g_string_append_printf (out, " %s=\"", name);
    append_number (out, v);
    g_string_append_c (out, '"');
}

static void
append_number (GString *out, gdouble v)
{
    gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

    g_string_append (out, g_ascii_dtostr (buf, sizeof (buf), v));
}

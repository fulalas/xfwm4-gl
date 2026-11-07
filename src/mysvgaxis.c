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
#include <glib.h>

#include "mysvgaxis.h"

#define KNOT_EPSILON 1e-6
#define LINE_WIDTH 1.0

typedef struct
{
    gdouble from;
    gdouble to;
}
Knot;

typedef gboolean (*SpanFilter) (const SvgSpan *, gint);

static void pin_leading_lines (SvgAxis *, GArray *, const SvgAxisScale *);
static void pin_trailing_lines (SvgAxis *, GArray *, const SvgAxisScale *);
static void pin_edges (SvgAxis *, GArray *, const SvgAxisScale *);
static void clamp_knots (SvgAxis *, gint);
static GArray *select_lines (GArray *, gint, SpanFilter);
static gboolean is_leading_line (const SvgSpan *, gint);
static gboolean is_trailing_line (const SvgSpan *, gint);
static gdouble axis_snap (const SvgAxisScale *, gdouble);
static gdouble interpolate (const Knot *, const Knot *, gdouble);
static gint knot_find (GArray *, gdouble);
static gdouble knot_get (GArray *, gdouble, gdouble);
static void knot_set (GArray *, gdouble, gdouble);
static gint compare_span_up (gconstpointer, gconstpointer);
static gint compare_span_down (gconstpointer, gconstpointer);
static gint compare_knot (gconstpointer, gconstpointer);

void
svgAxisBuild (SvgAxis *axis, GArray *spans, const SvgAxisScale *scale)
{
    axis->k = (gdouble) scale->device_size / scale->size;
    axis->knots = g_array_new (FALSE, FALSE, sizeof (Knot));
    knot_set (axis->knots, 0, 0);
    knot_set (axis->knots, scale->size, scale->device_size);
    pin_leading_lines (axis, spans, scale);
    pin_trailing_lines (axis, spans, scale);
    pin_edges (axis, spans, scale);
    g_array_sort (axis->knots, compare_knot);
    clamp_knots (axis, scale->device_size);
}

static void
pin_leading_lines (SvgAxis *axis, GArray *spans, const SvgAxisScale *scale)
{
    GArray *lines;
    SvgSpan *span;
    gdouble a;
    guint i;

    lines = select_lines (spans, scale->size, is_leading_line);
    g_array_sort (lines, compare_span_up);
    for (i = 0; i < lines->len; i++)
    {
        span = &g_array_index (lines, SvgSpan, i);
        a = knot_get (axis->knots, span->pos, axis_snap (scale, span->pos));
        knot_set (axis->knots, span->pos, a);
        knot_set (axis->knots, span->pos + span->len, a + LINE_WIDTH);
    }
    g_array_free (lines, TRUE);
}

static void
pin_trailing_lines (SvgAxis *axis, GArray *spans, const SvgAxisScale *scale)
{
    GArray *lines;
    SvgSpan *span;
    gdouble b;
    guint i;

    lines = select_lines (spans, scale->size, is_trailing_line);
    g_array_sort (lines, compare_span_down);
    for (i = 0; i < lines->len; i++)
    {
        span = &g_array_index (lines, SvgSpan, i);
        b = knot_get (axis->knots, span->pos + span->len, axis_snap (scale, span->pos + span->len));
        knot_set (axis->knots, span->pos + span->len, b);
        knot_set (axis->knots, span->pos, b - LINE_WIDTH);
    }
    g_array_free (lines, TRUE);
}

static void
pin_edges (SvgAxis *axis, GArray *spans, const SvgAxisScale *scale)
{
    SvgSpan *span;
    guint i;

    for (i = 0; i < spans->len; i++)
    {
        span = &g_array_index (spans, SvgSpan, i);
        knot_set (axis->knots, span->pos, axis_snap (scale, span->pos));
        knot_set (axis->knots, span->pos + span->len, axis_snap (scale, span->pos + span->len));
    }
}

static void
clamp_knots (SvgAxis *axis, gint device_size)
{
    guint i;

    for (i = 1; i < axis->knots->len; i++)
    {
        Knot *prev = &g_array_index (axis->knots, Knot, i - 1);
        Knot *knot = &g_array_index (axis->knots, Knot, i);

        knot->to = CLAMP (knot->to, prev->to, device_size);
    }
}

static GArray *
select_lines (GArray *spans, gint size, SpanFilter keep)
{
    GArray *lines;
    SvgSpan *span;
    guint i;

    lines = g_array_new (FALSE, FALSE, sizeof (SvgSpan));
    for (i = 0; i < spans->len; i++)
    {
        span = &g_array_index (spans, SvgSpan, i);
        if (keep (span, size))
        {
            g_array_append_val (lines, *span);
        }
    }
    return lines;
}

static gboolean
is_leading_line (const SvgSpan *span, gint size)
{
    return span->len <= LINE_WIDTH && span->pos * 2 + span->len < size;
}

static gboolean
is_trailing_line (const SvgSpan *span, gint size)
{
    return span->len <= LINE_WIDTH && span->pos * 2 + span->len >= size;
}

static gdouble
axis_snap (const SvgAxisScale *scale, gdouble v)
{
    if (v * 2 <= scale->size)
    {
        return lround (v * scale->scale);
    }
    return scale->device_size - lround ((scale->size - v) * scale->scale);
}

gdouble
svgAxisMap (const SvgAxis *axis, gdouble v)
{
    const Knot *lo, *hi;
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
            return interpolate (lo, hi, v) / axis->k;
        }
        lo = hi;
    }
    return (lo->to + (v - lo->from) * axis->k) / axis->k;
}

static gdouble
interpolate (const Knot *lo, const Knot *hi, gdouble v)
{
    return lo->to + (v - lo->from) * (hi->to - lo->to) / (hi->from - lo->from);
}

void
svgAxisFree (SvgAxis *axis)
{
    g_array_free (axis->knots, TRUE);
}

static gint
knot_find (GArray *knots, gdouble from)
{
    guint i;

    for (i = 0; i < knots->len; i++)
    {
        if (ABS (g_array_index (knots, Knot, i).from - from) < KNOT_EPSILON)
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
    const SvgSpan *sa = a, *sb = b;

    return (sa->pos > sb->pos) - (sa->pos < sb->pos);
}

static gint
compare_span_down (gconstpointer a, gconstpointer b)
{
    const SvgSpan *sa = a, *sb = b;

    return (sa->pos + sa->len < sb->pos + sb->len) - (sa->pos + sa->len > sb->pos + sb->len);
}

static gint
compare_knot (gconstpointer a, gconstpointer b)
{
    const Knot *ka = a, *kb = b;

    return (ka->from > kb->from) - (ka->from < kb->from);
}

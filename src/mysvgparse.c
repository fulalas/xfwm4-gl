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

#include <string.h>
#include <glib.h>

#include "mysvgparse.h"

#define VIEWBOX_FIELDS 4

typedef struct
{
    gchar *name;
    gchar *value;
    gchar quote;
}
Attr;

static const gchar *skip_list_separators (const gchar *);
static const gchar *skip_separators (const gchar *, const gchar *, gchar);
static const gchar *skip_name (const gchar *, const gchar *);
static const gchar *skip_to (const gchar *, const gchar *, gchar);
static const gchar *parse_attr (const gchar *, const gchar *, Attr *);
static const gchar *tag_close (const gchar *);
static gboolean is_listed (const gchar *, const gchar *const *);

gboolean
svgParseLength (const gchar *s, gdouble *v)
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

gboolean
svgParseViewbox (const gchar *viewbox, gdouble *box)
{
    gchar *next;
    gint i;

    if (!viewbox)
    {
        return FALSE;
    }
    for (i = 0; i < VIEWBOX_FIELDS; i++)
    {
        viewbox = skip_list_separators (viewbox);
        box[i] = g_ascii_strtod (viewbox, &next);
        if (next == viewbox)
        {
            return FALSE;
        }
        viewbox = next;
    }
    return TRUE;
}

static const gchar *
skip_list_separators (const gchar *p)
{
    while (g_ascii_isspace (*p) || *p == ',')
    {
        p++;
    }
    return p;
}

const gchar *
svgNextTag (const gchar *p, const gchar *name, const gchar **end)
{
    const gchar *tag;
    gsize len;

    len = strlen (name);
    while ((tag = strstr (p, name)))
    {
        p = tag + len;
        if (g_ascii_isspace (*p))
        {
            *end = tag_close (p);
            return *end ? tag : NULL;
        }
    }
    return NULL;
}

static const gchar *
tag_close (const gchar *p)
{
    gchar quote;

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
    return *p ? p : NULL;
}

GArray *
svgTagAttrs (const gchar *tag, const gchar *end)
{
    GArray *attrs;
    const gchar *p;
    Attr attr;

    p = skip_name (tag, end);
    attrs = g_array_new (FALSE, FALSE, sizeof (Attr));
    while (p < end && (p = parse_attr (p, end, &attr)))
    {
        g_array_append_val (attrs, attr);
    }
    return attrs;
}

static const gchar *
parse_attr (const gchar *p, const gchar *end, Attr *attr)
{
    const gchar *name, *name_end, *value;

    name = skip_separators (p, end, '/');
    name_end = skip_name (name, end);
    if (name_end == name)
    {
        return NULL;
    }
    p = skip_separators (name_end, end, '=');
    if (p >= end || (*p != '"' && *p != '\''))
    {
        return NULL;
    }
    attr->quote = *p;
    value = p + 1;
    p = skip_to (value, end, attr->quote);
    attr->name = g_strndup (name, name_end - name);
    attr->value = g_strndup (value, p - value);
    return p + 1;
}

static const gchar *
skip_separators (const gchar *p, const gchar *end, gchar separator)
{
    while (p < end && (g_ascii_isspace (*p) || *p == separator))
    {
        p++;
    }
    return p;
}

static const gchar *
skip_name (const gchar *p, const gchar *end)
{
    while (p < end && *p != '=' && !g_ascii_isspace (*p))
    {
        p++;
    }
    return p;
}

static const gchar *
skip_to (const gchar *p, const gchar *end, gchar c)
{
    while (p < end && *p != c)
    {
        p++;
    }
    return p;
}

void
svgAttrsFree (GArray *attrs)
{
    guint i;

    for (i = 0; i < attrs->len; i++)
    {
        g_free (g_array_index (attrs, Attr, i).name);
        g_free (g_array_index (attrs, Attr, i).value);
    }
    g_array_free (attrs, TRUE);
}

const gchar *
svgAttrGet (GArray *attrs, const gchar *name)
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

gboolean
svgAttrNumber (GArray *attrs, const gchar *name, gdouble *v)
{
    const gchar *value;

    value = svgAttrGet (attrs, name);
    if (!value)
    {
        *v = 0;
        return TRUE;
    }
    return svgParseLength (value, v);
}

void
svgAppendAttrs (GString *out, GArray *attrs, const gchar *const *skip)
{
    guint i;

    for (i = 0; i < attrs->len; i++)
    {
        Attr *attr = &g_array_index (attrs, Attr, i);

        if (!is_listed (attr->name, skip))
        {
            g_string_append_printf (out, " %s=%c%s%c", attr->name, attr->quote, attr->value, attr->quote);
        }
    }
}

static gboolean
is_listed (const gchar *name, const gchar *const *list)
{
    for (; *list; list++)
    {
        if (!strcmp (*list, name))
        {
            return TRUE;
        }
    }
    return FALSE;
}

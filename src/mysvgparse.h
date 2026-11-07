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

#ifndef INC_MYSVGPARSE_H
#define INC_MYSVGPARSE_H

#include <glib.h>

gboolean                 svgParseLength                         (const gchar *,
                                                                 gdouble *);
gboolean                 svgParseViewbox                        (const gchar *,
                                                                 gdouble *);
const gchar             *svgNextTag                             (const gchar *,
                                                                 const gchar *,
                                                                 const gchar **);
GArray                  *svgTagAttrs                            (const gchar *,
                                                                 const gchar *);
void                     svgAttrsFree                           (GArray *);
const gchar             *svgAttrGet                             (GArray *,
                                                                 const gchar *);
gboolean                 svgAttrNumber                          (GArray *,
                                                                 const gchar *,
                                                                 gdouble *);
void                     svgAppendAttrs                         (GString *,
                                                                 GArray *,
                                                                 const gchar *const *);

#endif

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

#ifndef INC_MYSVG_H
#define INC_MYSVG_H

#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

typedef struct
{
    gchar *name;
    const gchar *value;
}
xfwmColorSymbol;

const gchar             *xfwmColorSymbolFind                    (const gchar *,
                                                                 xfwmColorSymbol *);
GdkPixbuf               *xfwmSvgLoad                            (const gchar *,
                                                                 xfwmColorSymbol *,
                                                                 gdouble);

#endif

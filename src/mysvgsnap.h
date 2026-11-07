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

#ifndef INC_MYSVGSNAP_H
#define INC_MYSVGSNAP_H

#include <glib.h>

#include "mysvgaxis.h"

typedef struct
{
    SvgAxisScale x;
    SvgAxisScale y;
}
SvgCanvas;

gchar                   *svgSnap                                (const gchar *,
                                                                 const SvgCanvas *);

#endif

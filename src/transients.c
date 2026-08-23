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
        Foundation, Inc., Inc., 51 Franklin Street, Fifth Floor, Boston,
        MA 02110-1301, USA.


        xfwm4    - (c) 2002-2011 Olivier Fourdan

 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <libxfce4util/libxfce4util.h>

#include "screen.h"
#include "client.h"
#include "stacking.h"
#include "transients.h"

/*
 * The parent/child relation is cached in c->parent, so that asking about
 * transients is a pointer compare instead of a walk over all the clients.
 * Only direct transients have a parent: a transient for group is related
 * to its group members, which is a cheap test on its own.
 */

static void
clientUpdateTransientRelation (Client * c)
{
    g_return_if_fail (c != NULL);

    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    c->parent = NULL;
    if (!clientIsDirectTransient (c))
    {
        return;
    }

    /*
     * An application may point WM_TRANSIENT_FOR at its own child, refuse
     * the link or we would end up with a loop. This is the only place that
     * sets parent, and refusing here is what keeps every walk up the chain
     * finite. Two windows naming each other keep the first link only, so
     * the family is seen from the child but not from the parent.
     *
     * The relation compares client windows, so look for the window alone,
     * not the frame like clientGetTransient() does.
     */
    if (clientCheckTransientWindow (c, c->transient_for))
    {
        c->parent = myScreenGetClientFromWindow (c->screen_info,
                                                 c->transient_for, SEARCH_WINDOW);
    }
}

/* Set WM_TRANSIENT_FOR, keeping the cached relation in step with it */
void
clientSetTransientFor (Client * c, Window w)
{
    g_return_if_fail (c != NULL);

    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    c->transient_for = w;
    clientUpdateTransientRelation (c);
}

/* The client is now managed, link it and adopt the transients waiting for
   it. Must run once the client is in the ring, that is what makes a parent
   findable.
 */
void
clientAttachTransients (Client * c)
{
    ScreenInfo *screen_info;
    Client *c2;
    guint i;

    g_return_if_fail (c != NULL);

    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    clientUpdateTransientRelation (c);

    screen_info = c->screen_info;
    for (c2 = screen_info->clients, i = 0; i < screen_info->client_count; c2 = c2->next, i++)
    {
        if ((c2 != c) && (c2->parent == NULL) && (c2->transient_for == c->window))
        {
            clientUpdateTransientRelation (c2);
        }
    }
}

/* The client is going away, orphan its children. Must run while it is
   still in the ring, but after it can no longer be looked up, or the
   orphans below would just find it again.
 */
void
clientDetachTransients (Client * c)
{
    ScreenInfo *screen_info;
    Client *c2;
    guint i;

    g_return_if_fail (c != NULL);

    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    screen_info = c->screen_info;
    for (c2 = screen_info->clients, i = 0; i < screen_info->client_count; c2 = c2->next, i++)
    {
        if (c2->parent == c)
        {
            c2->parent = NULL;
        }
    }
    c->parent = NULL;

    /* A link refused earlier because it would have closed a loop may be
       fine now that this client is out of the way, so let every transient
       still without a parent have another go
     */
    for (c2 = screen_info->clients, i = 0; i < screen_info->client_count; c2 = c2->next, i++)
    {
        if ((c2 != c) && (c2->parent == NULL))
        {
            clientUpdateTransientRelation (c2);
        }
    }
}

Client *
clientGetTransient (Client * c)
{
    g_return_val_if_fail (c != NULL, NULL);

    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    if ((c->transient_for) && (c->transient_for != c->screen_info->xroot))
    {
        return myScreenGetClientFromWindow (c->screen_info, c->transient_for, SEARCH_WINDOW|SEARCH_FRAME);
    }
    return NULL;
}

gboolean
clientIsDirectTransient (Client * c)
{
    g_return_val_if_fail (c != NULL, FALSE);

    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    return ((c->transient_for != c->screen_info->xroot) && (c->transient_for != None) && (c->transient_for != c->window));
}

gboolean
clientIsTransientForGroup (Client * c)
{
    g_return_val_if_fail (c != NULL, FALSE);

    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    return ((c->transient_for == c->screen_info->xroot) && (c->group_leader != None) && (c->group_leader != c->window));
}

gboolean
clientIsTransient (Client * c)
{
    g_return_val_if_fail (c != NULL, FALSE);

    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    return (clientIsDirectTransient(c) || clientIsTransientForGroup (c));
}

gboolean
clientIsModal (Client * c)
{
    g_return_val_if_fail (c != NULL, FALSE);

    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);
    /*
       If the WM_TRANSIENT_FOR hint is set to another toplevel window, the dialog is modal for that window;
       if WM_TRANSIENT_FOR is not set or set to the root window the dialog is modal for its window group.
     */
    return (FLAG_TEST (c->flags, CLIENT_FLAG_STATE_MODAL) && (c->type & WINDOW_REGULAR_FOCUSABLE) &&
            clientIsTransient (c));
}

gboolean
clientIsModalForGroup (Client * c)
{
    g_return_val_if_fail (c != NULL, FALSE);

    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    return (FLAG_TEST (c->flags, CLIENT_FLAG_STATE_MODAL) && (c->type & WINDOW_REGULAR_FOCUSABLE) &&
            !clientIsTransient(c) && (c->group_leader != None));
}

gboolean
clientIsTransientOrModalForGroup (Client * c)
{
    g_return_val_if_fail (c != NULL, FALSE);

    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    return (clientIsTransientForGroup(c) || clientIsModalForGroup(c));
}

gboolean
clientIsTransientOrModal (Client * c)
{
    g_return_val_if_fail (c != NULL, FALSE);

    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    return (clientIsTransient(c) || clientIsModal(c));
}

gboolean
clientSameGroup (Client * c1, Client * c2)
{
    g_return_val_if_fail (c1 != NULL, FALSE);
    g_return_val_if_fail (c2 != NULL, FALSE);

    TRACE ("client 1 \"%s\" (0x%lx), client 2 \"%s\" (0x%lx)",
           c1->name, c1->window, c2->name, c2->window);

    return ((c1 != c2) &&
            (((c1->group_leader != None) &&
              (c1->group_leader == c2->group_leader)) ||
             (c1->group_leader == c2->window) ||
             (c2->group_leader == c1->window)));
}

gboolean
clientSameLeader (Client * c1, Client * c2)
{
    g_return_val_if_fail (c1 != NULL, FALSE);
    g_return_val_if_fail (c2 != NULL, FALSE);

    TRACE ("client 1 \"%s\" (0x%lx), client 2 \"%s\" (0x%lx)",
           c1->name, c1->window, c2->name, c2->window);

    return ((c1 != c2) &&
            (((c1->client_leader != None) &&
              (c1->client_leader == c2->client_leader)) ||
             (c1->client_leader == c2->window) ||
             (c2->client_leader == c1->window)));
}

gboolean
clientSameName (Client * c1, Client * c2)
{
    g_return_val_if_fail (c1 != NULL, FALSE);
    g_return_val_if_fail (c2 != NULL, FALSE);

    TRACE ("client 1 \"%s\" (0x%lx), client 2 \"%s\" (0x%lx)",
           c1->name, c1->window, c2->name, c2->window);

    return ((c1 != c2) &&
            (c1->class.res_class != NULL) &&
            (c2->class.res_class != NULL) &&
            (strcmp (c1->class.res_name, c2->class.res_name) == 0));
}

static gboolean
clientIsTransientFor (Client * c1, Client * c2)
{
    g_return_val_if_fail (c1 != NULL, FALSE);
    g_return_val_if_fail (c2 != NULL, FALSE);

    TRACE ("client 1 \"%s\" (0x%lx), client 2 \"%s\" (0x%lx)",
           c1->name, c1->window, c2->name, c2->window);

    if (c1->transient_for)
    {
        if (c1->transient_for != c1->screen_info->xroot)
        {
            return (c1->parent == c2);
        }
        /*
         * Transients for group shouldn't apply to other transients, or
         * it breaks stacking for some apps, noticeably mozilla "save as"
         * dialog...
         */
        else if (c2->transient_for == None)
        {
            return (clientSameGroup (c1, c2));
        }
    }
    return FALSE;
}

gboolean
clientIsModalFor (Client * c1, Client * c2)
{
    g_return_val_if_fail (c1 != NULL, FALSE);
    g_return_val_if_fail (c2 != NULL, FALSE);

    TRACE ("client 1 \"%s\" (0x%lx), client 2 \"%s\" (0x%lx)",
           c1->name, c1->window, c2->name, c2->window);

    if (FLAG_TEST (c1->flags, CLIENT_FLAG_STATE_MODAL) && (c1->type & WINDOW_REGULAR_FOCUSABLE) && (c1->serial >= c2->serial))
    {
        /*
         * We used to consider all windows of the same group here
         * (ie. "clientSameGroup (c1, c2)") but that seems too
         * wide so we restric modality to transient relationship
         * for now.
         */
        return (clientIsTransientFor (c1, c2));
    }
    return FALSE;
}

gboolean
clientIsTransientOrModalFor (Client * c1, Client * c2)
{
    g_return_val_if_fail (c1 != NULL, FALSE);
    g_return_val_if_fail (c2 != NULL, FALSE);

    TRACE ("client 1 \"%s\" (0x%lx), client 2 \"%s\" (0x%lx)",
           c1->name, c1->window, c2->name, c2->window);

    return (clientIsTransientFor(c1, c2) || clientIsModalFor(c1, c2));
}

gboolean
clientIsValidTransientOrModal (Client * c)
{
    g_return_val_if_fail (c != NULL, FALSE);

    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);
    if (clientIsTransientOrModalForGroup (c))
    {
        ScreenInfo *screen_info = c->screen_info;
        GList *list;

        /* Look for a valid transient or modal for the same group */
        for (list = screen_info->windows_stack; list; list = g_list_next (list))
        {
            Client *c2 = (Client *) list->data;
            if (c2 != c)
            {
                if (clientIsTransientOrModalFor (c, c2))
                {
                    /* We found one, look no further */
                    return TRUE;
                }
            }
        }
    }
    else if (clientIsTransientOrModal (c))
    {
        return (clientGetTransient (c) != NULL);
    }

    return FALSE;
}

gboolean
clientTransientOrModalHasAncestor (Client * c, guint ws)
{
    Client *c2;
    GList *list;
    ScreenInfo *screen_info;

    g_return_val_if_fail (c != NULL, FALSE);

    TRACE ("client \"%s\" (0x%lx), workspace %u", c->name, c->window, ws);

    if (!clientIsTransientOrModal (c))
    {
        return FALSE;
    }

    screen_info = c->screen_info;
    for (list = screen_info->windows_stack; list; list = g_list_next (list))
    {
        c2 = (Client *) list->data;
        if ((c2 != c)
            && !clientIsTransientOrModal (c2)
            && clientIsTransientOrModalFor (c, c2)
            && FLAG_TEST (c2->xfwm_flags, XFWM_FLAG_VISIBLE)
            && (c2->win_workspace == ws)
            && (((ws == screen_info->current_ws) && FLAG_TEST (c2->xfwm_flags, XFWM_FLAG_VISIBLE))
                || !FLAG_TEST (c2->flags, CLIENT_FLAG_ICONIFIED)))
        {
            return TRUE;
        }
    }
    return FALSE;

}

Client *
clientGetModalFor (Client * c)
{
    ScreenInfo *screen_info;
    Client *c2;
    GList *list;

    g_return_val_if_fail (c != NULL, NULL);
    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    screen_info = c->screen_info;
    for (list = g_list_last(screen_info->windows_stack); list; list = g_list_previous (list))
    {
        c2 = (Client *) list->data;
        if (c2)
        {
            if ((c2 != c) && clientIsModalFor (c2, c))
            {
                return c2;
            }
        }
    }
    return NULL;
}

/* Find the deepest direct parent of that window */
Client *
clientGetTransientFor (Client * c)
{
    Client *first_parent;
    Client *c2;

    g_return_val_if_fail (c != NULL, NULL);
    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    first_parent = c;
    for (c2 = c->parent; c2 != NULL; c2 = c2->parent)
    {
        /* Do not cross a layer boundary */
        if (c->win_layer > c2->win_layer)
        {
            break;
        }
        first_parent = c2;
    }

    return first_parent;
}

/* The stamp marks the clients already collected in the list being built */
static unsigned long transient_stamp = 0;

static gboolean
clientIsStamped (Client * c)
{
    return (c->transient_stamp == transient_stamp);
}

/* Same test as clientIsTransientFor(), against the whole list at once.
   "head" is the client the list was asked for, the first of it.
 */
static gboolean
clientIsTransientForListed (Client * c, Client * head)
{
    if (clientIsDirectTransient (c))
    {
        return ((c->parent != NULL) && clientIsStamped (c->parent));
    }

    /*
     * A transient for group can only be transient for a client with no
     * transient_for of its own, and head is the only one of those that
     * ever gets in the list: every other member got in because it has a
     * transient_for. So one test against head is enough.
     */
    return clientIsTransientFor (c, head);
}

/* Build a GList of clients that have a transient or modal relationship.
   A modal is always a transient too, see clientIsModalFor(), so there is
   one list for both.
 */
GList *
clientListTransientOrModal (Client * c)
{
    ScreenInfo *screen_info;
    Client *c2;
    GList *transients;
    GList *list;

    g_return_val_if_fail (c != NULL, NULL);
    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    transients = g_list_prepend (NULL, c);
    transient_stamp++;
    c->transient_stamp = transient_stamp;

    /* Walk the stack bottom up, so that a client is stamped before the
       transients sitting above it are tested.
     */
    screen_info = c->screen_info;
    for (list = screen_info->windows_stack; list; list = g_list_next (list))
    {
        c2 = (Client *) list->data;
        if ((c2 != c) && clientIsTransientForListed (c2, c))
        {
            c2->transient_stamp = transient_stamp;
            transients = g_list_prepend (transients, c2);
        }
    }

    return g_list_reverse (transients);
}

/* Check if a window is not already a transient of a client.
   That's to avoid potential self transient relationship...
 */
gboolean
clientCheckTransientWindow (Client *c, Window w)
{
    Client *c2;

    g_return_val_if_fail (c != NULL, FALSE);
    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    if (w == None)
    {
        return TRUE;
    }

    /* Walk up from that window: if we meet c on the way, or a client that
       is already transient for c, the window is one of c's own transients
     */
    for (c2 = myScreenGetClientFromWindow (c->screen_info, w, SEARCH_WINDOW); c2; c2 = c2->parent)
    {
        if ((c2 == c) || clientIsTransientFor (c2, c))
        {
            return FALSE;
        }
    }

    return TRUE;
}

gboolean
clientSameApplication (Client *c1, Client *c2)
{
    g_return_val_if_fail (c1 != NULL, FALSE);
    g_return_val_if_fail (c2 != NULL, FALSE);

    TRACE ("client 1 \"%s\" (0x%lx), client 2 \"%s\" (0x%lx)",
           c1->name, c1->window, c2->name, c2->window);

    return (clientIsTransientOrModalFor (c1, c2) ||
            clientIsTransientOrModalFor (c2, c1) ||
            clientSameGroup (c1, c2) ||
            clientSameLeader (c1, c2) ||
            clientSameName (c1, c2) ||
            (c1->pid != 0 && c1->pid == c2->pid &&
             c1->hostname && c2->hostname &&
             !g_ascii_strcasecmp (c1->hostname, c2->hostname)));
}

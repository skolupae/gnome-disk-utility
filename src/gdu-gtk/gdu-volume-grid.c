/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/* gdu-volume-grid.c
 *
 * Copyright (C) 2009 David Zeuthen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <math.h>
#include <glib/gi18n.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <X11/XKBlib.h>

#include <gdu-gtk/gdu-gtk.h>

#include "gdu-volume-grid.h"

/* ---------------------------------------------------------------------------------------------------- */

typedef enum
{
        GRID_EDGE_NONE    = 0,
        GRID_EDGE_TOP    = (1<<0),
        GRID_EDGE_BOTTOM = (1<<1),
        GRID_EDGE_LEFT   = (1<<2),
        GRID_EDGE_RIGHT  = (1<<3)
} GridEdgeFlags;

typedef struct GridElement GridElement;

struct GridElement
{
        /* these values are set in recompute_grid() */
        gdouble size_ratio;
        GduPresentable *presentable; /* if NULL, it means no media is available */
        GList *embedded_elements;
        GridElement *parent;
        GridElement *prev;
        GridElement *next;

        /* these values are set in recompute_size() */
        gint x;
        gint y;
        gint width;
        gint height;
        GridEdgeFlags edge_flags;
};

static void
grid_element_free (GridElement *element)
{
        if (element->presentable != NULL)
                g_object_unref (element->presentable);

        g_list_foreach (element->embedded_elements, (GFunc) grid_element_free, NULL);
        g_list_free (element->embedded_elements);

        g_free (element);
}

/* ---------------------------------------------------------------------------------------------------- */

struct GduVolumeGridPrivate
{
        GduPool *pool;
        GduDrive *drive;
        GduDevice *device;

        GList *elements;

        GridElement *selected;
        GridElement *focused;
};

enum
{
        PROP_0,
        PROP_DRIVE,
};

enum
{
        CHANGED_SIGNAL,
        LAST_SIGNAL,
};

static guint signals[LAST_SIGNAL] = {0};

G_DEFINE_TYPE (GduVolumeGrid, gdu_volume_grid, GTK_TYPE_DRAWING_AREA)

static void recompute_grid (GduVolumeGrid *grid);

static void recompute_size (GduVolumeGrid *grid,
                            gint           width,
                            gint           height);

static GridElement *find_element_for_presentable (GduVolumeGrid *grid,
                                                  GduPresentable *presentable);

static GridElement *find_element_for_position (GduVolumeGrid *grid,
                                               gint x,
                                               gint y);

static gboolean gdu_volume_grid_expose_event (GtkWidget           *widget,
                                              GdkEventExpose      *event);

static void on_presentable_added        (GduPool        *pool,
                                         GduPresentable *p,
                                         gpointer        user_data);
static void on_presentable_removed      (GduPool        *pool,
                                         GduPresentable *p,
                                         gpointer        user_data);
static void on_presentable_changed      (GduPool        *pool,
                                         GduPresentable *p,
                                         gpointer        user_data);
static void on_presentable_job_changed (GduPool        *pool,
                                        GduPresentable *p,
                                        gpointer        user_data);

static void
gdu_volume_grid_finalize (GObject *object)
{
        GduVolumeGrid *grid = GDU_VOLUME_GRID (object);

        g_list_foreach (grid->priv->elements, (GFunc) grid_element_free, NULL);
        g_list_free (grid->priv->elements);

        g_object_unref (grid->priv->drive);
        if (grid->priv->device != NULL)
                g_object_unref (grid->priv->device);

        g_signal_handlers_disconnect_by_func (grid->priv->pool,
                                              on_presentable_added,
                                              grid);
        g_signal_handlers_disconnect_by_func (grid->priv->pool,
                                              on_presentable_removed,
                                              grid);
        g_signal_handlers_disconnect_by_func (grid->priv->pool,
                                              on_presentable_changed,
                                              grid);
        g_signal_handlers_disconnect_by_func (grid->priv->pool,
                                              on_presentable_job_changed,
                                              grid);
        g_object_unref (grid->priv->pool);

        if (G_OBJECT_CLASS (gdu_volume_grid_parent_class)->finalize != NULL)
                G_OBJECT_CLASS (gdu_volume_grid_parent_class)->finalize (object);
}

static void
gdu_volume_grid_get_property (GObject    *object,
                              guint       property_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
        GduVolumeGrid *grid = GDU_VOLUME_GRID (object);

        switch (property_id) {
        case PROP_DRIVE:
                g_value_set_object (value, grid->priv->drive);
                break;

        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
                break;
        }
}

static void
gdu_volume_grid_set_property (GObject      *object,
                              guint         property_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
        GduVolumeGrid *grid = GDU_VOLUME_GRID (object);

        switch (property_id) {
        case PROP_DRIVE:
                grid->priv->drive = GDU_DRIVE (g_value_dup_object (value));
                grid->priv->pool = gdu_presentable_get_pool (GDU_PRESENTABLE (grid->priv->drive));
                grid->priv->device = gdu_presentable_get_device (GDU_PRESENTABLE (grid->priv->drive));
                break;

        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
                break;
        }
}

static void
gdu_volume_grid_constructed (GObject *object)
{
        GduVolumeGrid *grid = GDU_VOLUME_GRID (object);

        gtk_widget_set_size_request (GTK_WIDGET (grid),
                                     -1,
                                     100);

        g_signal_connect (grid->priv->pool,
                          "presentable-added",
                          G_CALLBACK (on_presentable_added),
                          grid);
        g_signal_connect (grid->priv->pool,
                          "presentable-removed",
                          G_CALLBACK (on_presentable_removed),
                          grid);
        g_signal_connect (grid->priv->pool,
                          "presentable-changed",
                          G_CALLBACK (on_presentable_changed),
                          grid);
        g_signal_connect (grid->priv->pool,
                          "presentable-job-changed",
                          G_CALLBACK (on_presentable_job_changed),
                          grid);

        recompute_grid (grid);

        /* select the first element */
        if (grid->priv->elements != NULL) {
                GridElement *element = grid->priv->elements->data;
                grid->priv->selected = element;
                grid->priv->focused = element;
        }

        if (G_OBJECT_CLASS (gdu_volume_grid_parent_class)->constructed != NULL)
                G_OBJECT_CLASS (gdu_volume_grid_parent_class)->constructed (object);
}

static gboolean
is_ctrl_pressed (void)
{
        gboolean ret;
        XkbStateRec state;
        Bool status;

        ret = FALSE;

        gdk_error_trap_push ();
        status = XkbGetState (GDK_DISPLAY (), XkbUseCoreKbd, &state);
        gdk_error_trap_pop ();

        if (status == Success) {
                ret = ((state.mods & ControlMask) != 0);
        }

        return ret;
}

static gboolean
gdu_volume_grid_key_press_event (GtkWidget      *widget,
                                 GdkEventKey    *event)
{
        GduVolumeGrid *grid = GDU_VOLUME_GRID (widget);
        gboolean handled;
        GridElement *target;

        handled = FALSE;

        if (event->type != GDK_KEY_PRESS)
                goto out;

        switch (event->keyval) {
        case GDK_Left:
        case GDK_Right:
        case GDK_Up:
        case GDK_Down:
                target = NULL;

                if (grid->priv->focused == NULL) {
                        g_warning ("TODO: handle nothing being selected/focused");
                } else {
                        GridElement *element;

                        element = grid->priv->focused;
                        if (element != NULL) {
                                if (event->keyval == GDK_Left) {
                                        if (element->prev != NULL) {
                                                target = element->prev;
                                        } else {
                                                if (element->parent && element->parent->prev != NULL)
                                                        target = element->parent->prev;
                                        }
                                } else if (event->keyval == GDK_Right) {
                                        if (element->next != NULL) {
                                                target = element->next;
                                        } else {
                                                if (element->parent && element->parent->next != NULL)
                                                        target = element->parent->next;
                                        }
                                } else if (event->keyval == GDK_Up) {
                                        if (element->parent != NULL) {
                                                target = element->parent;
                                        }
                                } else if (event->keyval == GDK_Down) {
                                        if (element->embedded_elements != NULL) {
                                                target = (GridElement *) element->embedded_elements->data;
                                        }
                                }
                        }
                }

                if (target != NULL) {
                        if (is_ctrl_pressed ()) {
                                grid->priv->focused = target;
                        } else {
                                grid->priv->selected = target;
                                grid->priv->focused = target;
                                g_signal_emit (grid,
                                               signals[CHANGED_SIGNAL],
                                               0);
                        }
                        gtk_widget_queue_draw (GTK_WIDGET (grid));
                }
                handled = TRUE;
                break;

        case GDK_Return:
        case GDK_space:
                if (grid->priv->focused != grid->priv->selected &&
                    grid->priv->focused != NULL) {
                        grid->priv->selected = grid->priv->focused;
                        g_signal_emit (grid,
                                       signals[CHANGED_SIGNAL],
                                       0);
                        gtk_widget_queue_draw (GTK_WIDGET (grid));
                }

                handled = TRUE;
                break;

        default:
                break;
        }

 out:
        return handled;
}

static gboolean
gdu_volume_grid_button_press_event (GtkWidget      *widget,
                                    GdkEventButton *event)
{
        GduVolumeGrid *grid = GDU_VOLUME_GRID (widget);
        gboolean handled;

        handled = FALSE;

        if (event->type != GDK_BUTTON_PRESS)
                goto out;

        if (event->button == 1) {
                GridElement *element;

                element = find_element_for_position (grid, event->x, event->y);
                if (element != NULL) {
                        grid->priv->selected = element;
                        grid->priv->focused = element;

                        g_signal_emit (grid,
                                       signals[CHANGED_SIGNAL],
                                       0);

                        gtk_widget_grab_focus (GTK_WIDGET (grid));
                        gtk_widget_queue_draw (GTK_WIDGET (grid));
                }

                handled = TRUE;
        }

 out:
        return handled;
}

static void
gdu_volume_grid_realize (GtkWidget *widget)
{
        GduVolumeGrid *grid = GDU_VOLUME_GRID (widget);
        GdkWindowAttr attributes;
        gint attributes_mask;

        GTK_WIDGET_SET_FLAGS (widget, GTK_REALIZED);

        attributes.x = widget->allocation.x;
        attributes.y = widget->allocation.y;
        attributes.width = widget->allocation.width;
        attributes.height = widget->allocation.height;
        attributes.wclass = GDK_INPUT_OUTPUT;
        attributes.window_type = GDK_WINDOW_CHILD;
        attributes.event_mask = gtk_widget_get_events (widget) |
                GDK_KEY_PRESS_MASK |
                GDK_EXPOSURE_MASK |
                GDK_BUTTON_PRESS_MASK |
                GDK_BUTTON_RELEASE_MASK |
                GDK_ENTER_NOTIFY_MASK |
                GDK_LEAVE_NOTIFY_MASK;
        attributes.visual = gtk_widget_get_visual (widget);
        attributes.colormap = gtk_widget_get_colormap (widget);

        attributes_mask = GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL | GDK_WA_COLORMAP;

        widget->window = gtk_widget_get_parent_window (widget);
        g_object_ref (widget->window);

        widget->window = gdk_window_new (gtk_widget_get_parent_window (widget),
                                         &attributes,
                                         attributes_mask);
        gdk_window_set_user_data (widget->window, grid);

        widget->style = gtk_style_attach (widget->style, widget->window);

        gtk_style_set_background (widget->style, widget->window, GTK_STATE_NORMAL);
}


static void
gdu_volume_grid_class_init (GduVolumeGridClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);
        GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

        g_type_class_add_private (klass, sizeof (GduVolumeGridPrivate));

        object_class->get_property = gdu_volume_grid_get_property;
        object_class->set_property = gdu_volume_grid_set_property;
        object_class->constructed  = gdu_volume_grid_constructed;
        object_class->finalize     = gdu_volume_grid_finalize;

        widget_class->realize            = gdu_volume_grid_realize;
        widget_class->key_press_event    = gdu_volume_grid_key_press_event;
        widget_class->button_press_event = gdu_volume_grid_button_press_event;
        widget_class->expose_event       = gdu_volume_grid_expose_event;

        g_object_class_install_property (object_class,
                                         PROP_DRIVE,
                                         g_param_spec_object ("drive",
                                                              _("Drive"),
                                                              _("Drive to show volumes for"),
                                                              GDU_TYPE_DRIVE,
                                                              G_PARAM_READABLE |
                                                              G_PARAM_WRITABLE |
                                                              G_PARAM_CONSTRUCT_ONLY));

        signals[CHANGED_SIGNAL] = g_signal_new ("changed",
                                                GDU_TYPE_VOLUME_GRID,
                                                G_SIGNAL_RUN_LAST,
                                                G_STRUCT_OFFSET (GduVolumeGridClass, changed),
                                                NULL,
                                                NULL,
                                                g_cclosure_marshal_VOID__VOID,
                                                G_TYPE_NONE,
                                                0);
}

static void
gdu_volume_grid_init (GduVolumeGrid *grid)
{
        grid->priv = G_TYPE_INSTANCE_GET_PRIVATE (grid, GDU_TYPE_VOLUME_GRID, GduVolumeGridPrivate);

        GTK_WIDGET_SET_FLAGS (grid, GTK_CAN_FOCUS);
}

GtkWidget *
gdu_volume_grid_new (GduDrive *drive)
{
        return GTK_WIDGET (g_object_new (GDU_TYPE_VOLUME_GRID,
                                         "drive", drive,
                                         NULL));
}

static gint
presentable_sort_offset (GduPresentable *a, GduPresentable *b)
{
        guint64 oa, ob;

        oa = gdu_presentable_get_offset (a);
        ob = gdu_presentable_get_offset (b);

        if (oa < ob)
                return -1;
        else if (oa > ob)
                return 1;
        else
                return 0;
}

static void
recompute_size (GduVolumeGrid *grid,
                gint           width,
                gint           height)
{
        GList *l;
        gint x;
        gint pixels_left;

        x = 0;
        pixels_left = width;
        for (l = grid->priv->elements; l != NULL; l = l->next) {
                GridElement *element = l->data;
                gint element_width;
                gboolean is_first;
                gboolean is_last;

                is_first = (l == grid->priv->elements);
                is_last  = (l->next == NULL);

                if (is_last) {
                        element_width = pixels_left;
                        pixels_left = 0;
                } else {
                        element_width = element->size_ratio * width;
                        if (element_width > pixels_left)
                                element_width = pixels_left;
                        pixels_left -= element_width;
                }

                element->x = x;
                element->y = 0;
                element->width = element_width;
                element->height = height;
                element->edge_flags = GRID_EDGE_TOP | GRID_EDGE_BOTTOM;
                if (is_first)
                        element->edge_flags |= GRID_EDGE_LEFT;
                if (is_last)
                        element->edge_flags |= GRID_EDGE_RIGHT;

                x += element_width;

                /* for now we don't recurse - we only handle embedded element for toplevel elements */
                if (element->embedded_elements != NULL) {
                        gint e_x;
                        gint e_width;
                        gint e_pixels_left;
                        GList *ll;

                        element->height = height/3;
                        element->edge_flags &= ~(GRID_EDGE_BOTTOM);

                        e_x = element->x;
                        e_width = element->width;
                        e_pixels_left = e_width;
                        for (ll = element->embedded_elements; ll != NULL; ll = ll->next) {
                                GridElement *e_element = ll->data;
                                gint e_element_width;
                                gboolean e_is_first;
                                gboolean e_is_last;

                                e_is_first = (ll == element->embedded_elements);
                                e_is_last  = (ll->next == NULL);

                                if (e_is_last) {
                                        e_element_width = e_pixels_left;
                                        e_pixels_left = 0;
                                } else {
                                        e_element_width = e_element->size_ratio * e_width;
                                        if (e_element_width > e_pixels_left)
                                                element_width = e_pixels_left;
                                        e_pixels_left -= e_element_width;
                                }

                                e_element->x = e_x;
                                e_element->y = element->height;
                                e_element->width = e_element_width;
                                e_element->height = height - e_element->y;
                                e_element->edge_flags = GRID_EDGE_BOTTOM;
                                if (is_first && e_is_first)
                                        e_element->edge_flags |= GRID_EDGE_LEFT;
                                if (is_last && e_is_last)
                                        e_element->edge_flags |= GRID_EDGE_RIGHT;

                                e_x += e_element_width;

                        }
                }
        }
}

static void
recompute_grid (GduVolumeGrid *grid)
{
        GduPresentable *cur_selected_presentable;
        GduPresentable *cur_focused_presentable;
        GList *enclosed_partitions;
        GList *l;
        guint64 size;
        GridElement *element;
        GridElement *prev_element;

        cur_selected_presentable = NULL;
        cur_focused_presentable = NULL;

        if (grid->priv->selected != NULL && grid->priv->selected->presentable != NULL)
                cur_selected_presentable = g_object_ref (grid->priv->selected->presentable);

        if (grid->priv->focused != NULL && grid->priv->focused->presentable != NULL)
                cur_focused_presentable = g_object_ref (grid->priv->focused->presentable);

        /* first delete old elements */
        g_list_foreach (grid->priv->elements, (GFunc) grid_element_free, NULL);
        g_list_free (grid->priv->elements);
        grid->priv->elements = NULL;

        /* then add new elements */
        size = gdu_presentable_get_size (GDU_PRESENTABLE (grid->priv->drive));
        enclosed_partitions = gdu_pool_get_enclosed_presentables (grid->priv->pool,
                                                                  GDU_PRESENTABLE (grid->priv->drive));
        enclosed_partitions = g_list_sort (enclosed_partitions, (GCompareFunc) presentable_sort_offset);
        prev_element = NULL;
        for (l = enclosed_partitions; l != NULL; l = l->next) {
                GduPresentable *ep = GDU_PRESENTABLE (l->data);
                GduDevice *ed;
                guint64 psize;

                ed = gdu_presentable_get_device (ep);

                psize = gdu_presentable_get_size (ep);

                element = g_new0 (GridElement, 1);
                element->size_ratio = ((gdouble) psize) / size;
                element->presentable = g_object_ref (ep);
                element->prev = prev_element;
                if (prev_element != NULL)
                        prev_element->next = element;
                prev_element = element;

                grid->priv->elements = g_list_append (grid->priv->elements, element);

                if (ed != NULL && gdu_device_is_partition (ed) &&
                    (g_strcmp0 (gdu_device_partition_get_type (ed), "0x05") == 0 ||
                     g_strcmp0 (gdu_device_partition_get_type (ed), "0x0f") == 0 ||
                     g_strcmp0 (gdu_device_partition_get_type (ed), "0x85") == 0)) {
                        GList *enclosed_logical_partitions;
                        GList *ll;
                        GridElement *logical_element;
                        GridElement *logical_prev_element;

                        enclosed_logical_partitions = gdu_pool_get_enclosed_presentables (grid->priv->pool, ep);
                        logical_prev_element = NULL;
                        for (ll = enclosed_logical_partitions; ll != NULL; ll = ll->next) {
                                GduPresentable *logical_ep = GDU_PRESENTABLE (ll->data);
                                guint64 lsize;

                                lsize = gdu_presentable_get_size (logical_ep);

                                logical_element = g_new0 (GridElement, 1);
                                logical_element->size_ratio = ((gdouble) lsize) / psize;
                                logical_element->presentable = g_object_ref (logical_ep);
                                logical_element->parent = element;
                                logical_element->prev = logical_prev_element;
                                if (logical_prev_element != NULL)
                                        logical_prev_element->next = logical_element;
                                logical_prev_element = logical_element;

                                element->embedded_elements = g_list_append (element->embedded_elements,
                                                                            logical_element);
                        }
                        g_list_foreach (enclosed_logical_partitions, (GFunc) g_object_unref, NULL);
                        g_list_free (enclosed_logical_partitions);
                        }
                if (ed != NULL)
                        g_object_unref (ed);

        }
        g_list_foreach (enclosed_partitions, (GFunc) g_object_unref, NULL);
        g_list_free (enclosed_partitions);

        /* If we have no elements, then make up an element with a NULL presentable - this is to handle
         * the "No Media Inserted" case.
         */
        if (grid->priv->elements == NULL) {
                element = g_new0 (GridElement, 1);
                element->size_ratio = 1.0;
                grid->priv->elements = g_list_append (grid->priv->elements, element);
        }

        /* reselect focused and selected elements */
        grid->priv->focused = NULL;
        if (cur_focused_presentable != NULL) {
                grid->priv->focused = find_element_for_presentable (grid, cur_focused_presentable);
                g_object_unref (cur_focused_presentable);
        }
        grid->priv->selected = NULL;
        if (cur_selected_presentable != NULL) {
                grid->priv->selected = find_element_for_presentable (grid, cur_selected_presentable);
                g_object_unref (cur_selected_presentable);
        }

        /* ensure something is always focused/selected */
        if ( grid->priv->focused == NULL) {
                grid->priv->focused = grid->priv->elements->data;
        }
        if (grid->priv->selected == NULL) {
                grid->priv->selected = grid->priv->focused;
        }

        /* queue a redraw */
        gtk_widget_queue_draw (GTK_WIDGET (grid));
}

static void
round_rect (cairo_t *cr,
            gdouble x, gdouble y,
            gdouble w, gdouble h,
            gdouble r,
            GridEdgeFlags edge_flags)
{
        gboolean top_left_round;
        gboolean top_right_round;
        gboolean bottom_right_round;
        gboolean bottom_left_round;

        top_left_round     = ((edge_flags & GRID_EDGE_TOP)    && (edge_flags & GRID_EDGE_LEFT));
        top_right_round    = ((edge_flags & GRID_EDGE_TOP)    && (edge_flags & GRID_EDGE_RIGHT));
        bottom_right_round = ((edge_flags & GRID_EDGE_BOTTOM) && (edge_flags & GRID_EDGE_RIGHT));
        bottom_left_round  = ((edge_flags & GRID_EDGE_BOTTOM) && (edge_flags & GRID_EDGE_LEFT));

        if (top_left_round) {
                cairo_move_to  (cr,
                                x + r, y);
        } else {
                cairo_move_to  (cr,
                                x, y);
        }

        if (top_right_round) {
                cairo_line_to  (cr,
                                x + w - r, y);
                cairo_curve_to (cr,
                                x + w, y,
                                x + w, y,
                                x + w, y + r);
        } else {
                cairo_line_to  (cr,
                                x + w, y);
        }

        if (bottom_right_round) {
                cairo_line_to  (cr,
                                x + w, y + h - r);
                cairo_curve_to (cr,
                                x + w, y + h,
                                x + w, y + h,
                                x + w - r, y + h);
        } else {
                cairo_line_to  (cr,
                                x + w, y + h);
        }

        if (bottom_left_round) {
                cairo_line_to  (cr,
                                x + r, y + h);
                cairo_curve_to (cr,
                                x, y + h,
                                x, y + h,
                                x, y + h - r);
        } else {
                cairo_line_to  (cr,
                                x, y + h);
        }

        if (top_left_round) {
                cairo_line_to  (cr,
                                x, y + r);
                cairo_curve_to (cr,
                                x, y,
                                x, y,
                                x + r, y);
        } else {
                cairo_line_to  (cr,
                                x, y);
        }
}

static void
render_element (GduVolumeGrid *grid,
                cairo_t       *cr,
                GridElement   *element,
                gboolean       is_selected,
                gboolean       is_focused,
                gboolean       is_grid_focused)
{
        gdouble fill_red;
        gdouble fill_green;
        gdouble fill_blue;
        gdouble fill_selected_red;
        gdouble fill_selected_green;
        gdouble fill_selected_blue;
        gdouble fill_selected_not_focused_red;
        gdouble fill_selected_not_focused_green;
        gdouble fill_selected_not_focused_blue;
        gdouble focus_rect_red;
        gdouble focus_rect_green;
        gdouble focus_rect_blue;
        gdouble focus_rect_selected_red;
        gdouble focus_rect_selected_green;
        gdouble focus_rect_selected_blue;
        gdouble focus_rect_selected_not_focused_red;
        gdouble focus_rect_selected_not_focused_green;
        gdouble focus_rect_selected_not_focused_blue;
        gdouble stroke_red;
        gdouble stroke_green;
        gdouble stroke_blue;
        gdouble stroke_selected_red;
        gdouble stroke_selected_green;
        gdouble stroke_selected_blue;
        gdouble stroke_selected_not_focused_red;
        gdouble stroke_selected_not_focused_green;
        gdouble stroke_selected_not_focused_blue;
        gdouble text_red;
        gdouble text_green;
        gdouble text_blue;
        gdouble text_selected_red;
        gdouble text_selected_green;
        gdouble text_selected_blue;
        gdouble text_selected_not_focused_red;
        gdouble text_selected_not_focused_green;
        gdouble text_selected_not_focused_blue;

        fill_red     = 1;
        fill_green   = 1;
        fill_blue    = 1;
        fill_selected_red     = 0.40;
        fill_selected_green   = 0.60;
        fill_selected_blue    = 0.80;
        fill_selected_not_focused_red     = 0.60;
        fill_selected_not_focused_green   = 0.60;
        fill_selected_not_focused_blue    = 0.60;
        focus_rect_red     = 0.75;
        focus_rect_green   = 0.75;
        focus_rect_blue    = 0.75;
        focus_rect_selected_red     = 0.70;
        focus_rect_selected_green   = 0.70;
        focus_rect_selected_blue    = 0.80;
        focus_rect_selected_not_focused_red     = 0.70;
        focus_rect_selected_not_focused_green   = 0.70;
        focus_rect_selected_not_focused_blue    = 0.70;
        stroke_red   = 0.75;
        stroke_green = 0.75;
        stroke_blue  = 0.75;
        stroke_selected_red   = 0.3;
        stroke_selected_green = 0.45;
        stroke_selected_blue  = 0.6;
        stroke_selected_not_focused_red   = 0.45;
        stroke_selected_not_focused_green = 0.45;
        stroke_selected_not_focused_blue  = 0.45;
        text_red     = 0;
        text_green   = 0;
        text_blue    = 0;
        text_selected_red     = 1;
        text_selected_green   = 1;
        text_selected_blue    = 1;
        text_selected_not_focused_red     = 1;
        text_selected_not_focused_green   = 1;
        text_selected_not_focused_blue    = 1;

#if 0
        g_debug ("rendering element: x=%d w=%d",
                 element->x,
                 element->width);
#endif

        cairo_save (cr);
        cairo_rectangle (cr,
                         element->x + 0.5,
                         element->y + 0.5,
                         element->width,
                         element->height);
        cairo_clip (cr);

        round_rect (cr,
                    element->x + 0.5,
                    element->y + 0.5,
                    element->width,
                    element->height,
                    10,
                    element->edge_flags);

        if (is_selected) {
                cairo_pattern_t *gradient;
                gradient = cairo_pattern_create_radial (element->x + element->width / 2,
                                                        element->y + element->height / 2,
                                                        0.0,
                                                        element->x + element->width / 2,
                                                        element->y + element->height / 2,
                                                        element->width/2.0);
                if (is_grid_focused) {
                        cairo_pattern_add_color_stop_rgb (gradient,
                                                          0.0,
                                                          1.0 * fill_selected_red,
                                                          1.0 * fill_selected_green,
                                                          1.0 * fill_selected_blue);
                        cairo_pattern_add_color_stop_rgb (gradient,
                                                          1.0,
                                                          0.8 * fill_selected_red,
                                                          0.8 * fill_selected_green,
                                                          0.8 * fill_selected_blue);
                } else {
                        cairo_pattern_add_color_stop_rgb (gradient,
                                                          0.0,
                                                          1.0 * fill_selected_not_focused_red,
                                                          1.0 * fill_selected_not_focused_green,
                                                          1.0 * fill_selected_not_focused_blue);
                        cairo_pattern_add_color_stop_rgb (gradient,
                                                          1.0,
                                                          0.8 * fill_selected_not_focused_red,
                                                          0.8 * fill_selected_not_focused_green,
                                                          0.8 * fill_selected_not_focused_blue);
                }
                cairo_set_source (cr, gradient);
                cairo_pattern_destroy (gradient);
        } else {
                cairo_set_source_rgb (cr,
                                      fill_red,
                                      fill_green,
                                      fill_blue);
        }
        cairo_fill_preserve (cr);
        if (is_selected) {
                if (is_grid_focused) {
                        cairo_set_source_rgb (cr,
                                              stroke_selected_red,
                                              stroke_selected_green,
                                              stroke_selected_blue);
                } else {
                        cairo_set_source_rgb (cr,
                                              stroke_selected_not_focused_red,
                                              stroke_selected_not_focused_green,
                                              stroke_selected_not_focused_blue);
                }
        } else {
                cairo_set_source_rgb (cr,
                                      stroke_red,
                                      stroke_green,
                                      stroke_blue);
        }
        cairo_set_dash (cr, NULL, 0, 0.0);
        cairo_set_line_width (cr, 1.0);
        cairo_stroke (cr);

        /* focus indicator */
        if (is_focused && is_grid_focused) {
                gdouble dashes[] = {2.0};
                round_rect (cr,
                            element->x + 0.5 + 3,
                            element->y + 0.5 + 3,
                            element->width - 3 * 2,
                            element->height - 3 * 2,
                            20,
                            element->edge_flags);
                cairo_set_source_rgb (cr, focus_rect_red, focus_rect_green, focus_rect_blue);
                cairo_set_dash (cr, dashes, 1, 0.0);
                cairo_set_line_width (cr, 1.0);
                cairo_stroke (cr);
        }

        if (element->presentable == NULL) { /* no media available */
                cairo_text_extents_t te;
                const gchar *text;

                text = _("No Media Detected");

                if (is_selected) {
                        if (is_grid_focused) {
                                cairo_set_source_rgb (cr,
                                                      text_selected_red,
                                                      text_selected_green,
                                                      text_selected_blue);
                        } else {
                                cairo_set_source_rgb (cr,
                                                      text_selected_not_focused_red,
                                                      text_selected_not_focused_green,
                                                      text_selected_not_focused_blue);
                        }
                } else {
                        cairo_set_source_rgb (cr, text_red, text_green, text_blue);
                }
                cairo_select_font_face (cr, "sans",
                                        CAIRO_FONT_SLANT_NORMAL,
                                        CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size (cr, 8.0);

                cairo_text_extents (cr, text, &te);
                cairo_move_to (cr,
                               ceil (element->x + element->width / 2 - te.width/2  - te.x_bearing),
                               ceil (element->y + element->height / 2 - 2 - te.height/2 - te.y_bearing));
                cairo_show_text (cr, text);

        } else { /* render descriptive text for the presentable */
                gchar *s;
                gchar *s1;
                cairo_text_extents_t te;
                cairo_text_extents_t te1;
                GduDevice *d;
                gdouble text_height;

                d = gdu_presentable_get_device (element->presentable);

                s = NULL;
                s1 = NULL;
                if (d != NULL && g_strcmp0 (gdu_device_id_get_usage (d), "filesystem") == 0) {
                        gchar *fstype_str;
                        gchar *size_str;

                        s = g_strdup (gdu_device_id_get_label (d));
                        fstype_str = gdu_util_get_fstype_for_display (gdu_device_id_get_type (d),
                                                                      gdu_device_id_get_version (d),
                                                                      FALSE);
                        size_str = gdu_util_get_size_for_display (gdu_device_get_size (d),
                                                                  FALSE,
                                                                  FALSE);
                        s1 = g_strdup_printf ("%s %s", size_str, fstype_str);
                        g_free (fstype_str);
                        g_free (size_str);
                } else if (d != NULL && gdu_device_is_partition (d) &&
                           (g_strcmp0 (gdu_device_partition_get_type (d), "0x05") == 0 ||
                            g_strcmp0 (gdu_device_partition_get_type (d), "0x0f") == 0 ||
                            g_strcmp0 (gdu_device_partition_get_type (d), "0x85") == 0)) {
                        s = g_strdup (_("Extended"));
                        s1 = gdu_util_get_size_for_display (gdu_presentable_get_size (element->presentable),
                                                            FALSE,
                                                            FALSE);
                } else if (d != NULL && g_strcmp0 (gdu_device_id_get_usage (d), "crypto") == 0) {
                        s = g_strdup (_("Encrypted"));
                        s1 = gdu_util_get_size_for_display (gdu_presentable_get_size (element->presentable),
                                                            FALSE,
                                                            FALSE);
                } else if (!gdu_presentable_is_allocated (element->presentable)) {
                        s = g_strdup (_("Free"));
                        s1 = gdu_util_get_size_for_display (gdu_presentable_get_size (element->presentable),
                                                            FALSE,
                                                            FALSE);
                } else if (!gdu_presentable_is_recognized (element->presentable)) {
                        s = g_strdup (_("Unknown"));
                        s1 = gdu_util_get_size_for_display (gdu_presentable_get_size (element->presentable),
                                                            FALSE,
                                                            FALSE);
                }

                if (s == NULL)
                        s = gdu_presentable_get_name (element->presentable);
                if (s1 == NULL)
                        s1 = g_strdup ("");

                if (is_selected) {
                        if (is_grid_focused) {
                                cairo_set_source_rgb (cr,
                                                      text_selected_red,
                                                      text_selected_green,
                                                      text_selected_blue);
                        } else {
                                cairo_set_source_rgb (cr,
                                                      text_selected_not_focused_red,
                                                      text_selected_not_focused_green,
                                                      text_selected_not_focused_blue);
                        }
                } else {
                        cairo_set_source_rgb (cr, text_red, text_green, text_blue);
                }
                cairo_select_font_face (cr, "sans",
                                        CAIRO_FONT_SLANT_NORMAL,
                                        CAIRO_FONT_WEIGHT_NORMAL);
                cairo_set_font_size (cr, 8.0);

                cairo_text_extents (cr, s, &te);
                cairo_text_extents (cr, s1, &te1);

                text_height = te.height + te1.height;

                cairo_move_to (cr,
                               ceil (element->x + element->width / 2 - te.width/2  - te.x_bearing),
                               ceil (element->y + element->height / 2 - 2 - text_height/2 - te.y_bearing));
                cairo_show_text (cr, s);
                cairo_move_to (cr,
                               ceil (element->x + element->width / 2 - te1.width/2  - te1.x_bearing),
                               ceil (element->y + element->height / 2 + 2 - te1.y_bearing));
                cairo_show_text (cr, s1);
                g_free (s);
                g_free (s1);

                if (d != NULL)
                        g_object_unref (d);
        }

        cairo_restore (cr);
}

static gboolean
gdu_volume_grid_expose_event (GtkWidget           *widget,
                              GdkEventExpose      *event)
{
        GduVolumeGrid *grid = GDU_VOLUME_GRID (widget);
        GList *l;
        cairo_t *cr;
        gdouble width;
        gdouble height;

        width = widget->allocation.width;
        height = widget->allocation.height;

        recompute_size (grid,
                        width - 1,
                        height -1);

        cr = gdk_cairo_create (widget->window);
        cairo_rectangle (cr,
                         event->area.x, event->area.y,
                         event->area.width, event->area.height);
        cairo_clip (cr);

        for (l = grid->priv->elements; l != NULL; l = l->next) {
                GridElement *element = l->data;
                gboolean is_selected;
                gboolean is_focused;
                gboolean is_grid_focused;
                GList *ll;

                is_selected = FALSE;
                is_focused = FALSE;
                is_grid_focused = GTK_WIDGET_HAS_FOCUS (grid);

                if (element == grid->priv->selected)
                        is_selected = TRUE;

                if (element == grid->priv->focused) {
                        if (grid->priv->focused != grid->priv->selected && is_grid_focused)
                                is_focused = TRUE;
                }

                render_element (grid,
                                cr,
                                element,
                                is_selected,
                                is_focused,
                                is_grid_focused);

                for (ll = element->embedded_elements; ll != NULL; ll = ll->next) {
                        GridElement *element = ll->data;

                        is_selected = FALSE;
                        is_focused = FALSE;
                        is_grid_focused = GTK_WIDGET_HAS_FOCUS (grid);

                        if (element == grid->priv->selected)
                                is_selected = TRUE;

                        if (element == grid->priv->focused) {
                                if (grid->priv->focused != grid->priv->selected && is_grid_focused)
                                        is_focused = TRUE;
                        }

                        render_element (grid,
                                        cr,
                                        element,
                                        is_selected,
                                        is_focused,
                                        is_grid_focused);
                }
        }

        cairo_destroy (cr);

        return FALSE;
}

static GridElement *
do_find_element_for_presentable (GList *elements,
                                 GduPresentable *presentable)
{
        GList *l;
        GridElement *ret;

        ret = NULL;

        for (l = elements; l != NULL; l = l->next) {
                GridElement *e = l->data;
                if (e->presentable == presentable) {
                        ret = e;
                        goto out;
                }

                ret = do_find_element_for_presentable (e->embedded_elements, presentable);
                if (ret != NULL)
                        goto out;

        }

 out:
        return ret;
}

static GridElement *
find_element_for_presentable (GduVolumeGrid *grid,
                              GduPresentable *presentable)
{
        return do_find_element_for_presentable (grid->priv->elements, presentable);
}

static GridElement *
do_find_element_for_position (GList *elements,
                              gint   x,
                              gint   y)
{
        GList *l;
        GridElement *ret;

        ret = NULL;

        for (l = elements; l != NULL; l = l->next) {
                GridElement *e = l->data;

                if ((x >= e->x) &&
                    (x  < e->x + e->width) &&
                    (y >= e->y) &&
                    (y  < e->y + e->height)) {
                        ret = e;
                        goto out;
                }

                ret = do_find_element_for_position (e->embedded_elements, x, y);
                if (ret != NULL)
                        goto out;

        }

 out:
        return ret;
}

static GridElement *
find_element_for_position (GduVolumeGrid *grid,
                           gint x,
                           gint y)
{
        return do_find_element_for_position (grid->priv->elements, x, y);
}

GduPresentable *
gdu_volume_grid_get_selected (GduVolumeGrid *grid)
{
        GduPresentable *ret;

        g_return_val_if_fail (GDU_IS_VOLUME_GRID (grid), NULL);

                ret = NULL;
        if (grid->priv->selected != NULL) {
                if (grid->priv->selected->presentable != NULL)
                        ret = g_object_ref (grid->priv->selected->presentable);
        }

        return ret;
}

static void
maybe_recompute (GduVolumeGrid  *grid,
                 GduPresentable *p)
{
        gboolean recompute;

        recompute = FALSE;
        if (p == GDU_PRESENTABLE (grid->priv->drive) ||
            gdu_presentable_encloses (GDU_PRESENTABLE (grid->priv->drive), p) ||
            find_element_for_presentable (grid, p) != NULL) {
                recompute = TRUE;
        }

        if (recompute) {
                recompute_grid (grid);
                g_signal_emit (grid,
                               signals[CHANGED_SIGNAL],
                               0);
        }
}

static void
on_presentable_added (GduPool        *pool,
                      GduPresentable *p,
                      gpointer        user_data)
{
        GduVolumeGrid *grid = GDU_VOLUME_GRID (user_data);
        maybe_recompute (grid, p);
}

static void
on_presentable_removed (GduPool        *pool,
                        GduPresentable *p,
                        gpointer        user_data)
{
        GduVolumeGrid *grid = GDU_VOLUME_GRID (user_data);
        maybe_recompute (grid, p);
}

static void
on_presentable_changed (GduPool        *pool,
                        GduPresentable *p,
                        gpointer        user_data)
{
        GduVolumeGrid *grid = GDU_VOLUME_GRID (user_data);
        maybe_recompute (grid, p);
}

static void
on_presentable_job_changed (GduPool        *pool,
                            GduPresentable *p,
                            gpointer        user_data)
{
        GduVolumeGrid *grid = GDU_VOLUME_GRID (user_data);
        maybe_recompute (grid, p);
}
/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2008-2011 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include "config.h"
#include <string.h>
#include <stdlib.h>

#include <glib/gi18n.h>

#include "gduapplication.h"
#include "gduwindow.h"
#include "gdudevicetreemodel.h"
#include "gduutils.h"
#include "gduvolumegrid.h"
#include "gduiscsipathmodel.h"

/* Keep in sync with tabs in palimpsest.ui file */
typedef enum
{
  DETAILS_PAGE_NOT_SELECTED,
  DETAILS_PAGE_NOT_IMPLEMENTED,
  DETAILS_PAGE_DEVICE,
  DETAILS_PAGE_ISCSI_TARGET,
  DETAILS_PAGE_ISCSI_SENDTARGETS_COLLECTION,
} DetailsPage;

struct _GduWindow
{
  GtkWindow parent_instance;

  GduApplication *application;
  UDisksClient *client;

  GtkBuilder *builder;
  GduDeviceTreeModel *model;

  DetailsPage current_page;
  UDisksObject *current_object;

  GtkWidget *volume_grid;
  GtkWidget *write_cache_switch;
  GtkWidget *iscsi_connection_switch;

  GHashTable *label_connections;
};

typedef struct
{
  GtkWindowClass parent_class;
} GduWindowClass;

enum
{
  PROP_0,
  PROP_APPLICATION,
  PROP_CLIENT
};

static void gdu_window_show_error (GduWindow   *window,
                                   const gchar *message,
                                   GError      *orig_error);

static gboolean on_activate_link (GtkLabel    *label,
                                  const gchar *uri,
                                  gpointer     user_data);

static void setup_device_page (GduWindow *window, UDisksObject *object);
static void update_device_page (GduWindow *window);
static void teardown_device_page (GduWindow *window);

static void setup_iscsi_target_page (GduWindow *window, UDisksObject *object);
static void update_iscsi_target_page (GduWindow *window);
static void teardown_iscsi_target_page (GduWindow *window);

//static void setup_iscsi_sendtargets_collection_page (GduWindow *window, UDisksObject *object);
//static void update_iscsi_sendtargets_collection_page (GduWindow *window);
//static void teardown_iscsi_sendtargets_collection_page (GduWindow *window);

static void on_volume_grid_changed (GduVolumeGrid  *grid,
                                    gpointer        user_data);

static void on_devtab_action_generic_activated (GtkAction *action, gpointer user_data);
static void on_devtab_action_partition_create_activated (GtkAction *action, gpointer user_data);
static void on_devtab_action_mount_activated (GtkAction *action, gpointer user_data);
static void on_devtab_action_unmount_activated (GtkAction *action, gpointer user_data);
static void on_devtab_action_eject_activated (GtkAction *action, gpointer user_data);
static void on_devtab_action_unlock_activated (GtkAction *action, gpointer user_data);
static void on_devtab_action_lock_activated (GtkAction *action, gpointer user_data);
static void on_devtab_action_activate_swap_activated (GtkAction *action, gpointer user_data);
static void on_devtab_action_deactivate_swap_activated (GtkAction *action, gpointer user_data);

static void iscsi_connection_switch_on_notify_active (GObject     *object,
                                                      GParamSpec  *pspec,
                                                      gpointer     user_data);

G_DEFINE_TYPE (GduWindow, gdu_window, GTK_TYPE_WINDOW);

static void
gdu_window_init (GduWindow *window)
{
  window->label_connections = g_hash_table_new_full (g_str_hash,
                                                     g_str_equal,
                                                     g_free,
                                                     NULL);
}

static void on_object_added (GDBusObjectManager  *manager,
                             GDBusObject         *object,
                             gpointer             user_data);

static void on_object_removed (GDBusObjectManager  *manager,
                               GDBusObject         *object,
                               gpointer             user_data);

static void on_interface_added (GDBusObjectManager  *manager,
                                GDBusObject         *object,
                                GDBusInterface      *interface,
                                gpointer             user_data);

static void on_interface_removed (GDBusObjectManager  *manager,
                                  GDBusObject         *object,
                                  GDBusInterface      *interface,
                                  gpointer             user_data);

static void on_interface_proxy_properties_changed (GDBusObjectManagerClient   *manager,
                                                   GDBusObjectProxy           *object_proxy,
                                                   GDBusProxy                 *interface_proxy,
                                                   GVariant                   *changed_properties,
                                                   const gchar *const         *invalidated_properties,
                                                   gpointer                    user_data);

static void
gdu_window_finalize (GObject *object)
{
  GduWindow *window = GDU_WINDOW (object);
  GDBusObjectManager *object_manager;

  object_manager = udisks_client_get_object_manager (window->client);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_object_added),
                                        window);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_object_removed),
                                        window);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_interface_added),
                                        window);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_interface_removed),
                                        window);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_interface_proxy_properties_changed),
                                        window);

  if (window->current_object != NULL)
    g_object_unref (window->current_object);

  g_object_unref (window->builder);
  g_object_unref (window->model);
  g_object_unref (window->client);
  g_object_unref (window->application);

  G_OBJECT_CLASS (gdu_window_parent_class)->finalize (object);
}

static gboolean
dont_select_headings (GtkTreeSelection *selection,
                      GtkTreeModel     *model,
                      GtkTreePath      *path,
                      gboolean          selected,
                      gpointer          data)
{
  GtkTreeIter iter;
  gboolean is_heading;

  gtk_tree_model_get_iter (model,
                           &iter,
                           path);
  gtk_tree_model_get (model,
                      &iter,
                      GDU_DEVICE_TREE_MODEL_COLUMN_IS_HEADING,
                      &is_heading,
                      -1);

  return !is_heading;
}

static void
on_row_inserted (GtkTreeModel *tree_model,
                 GtkTreePath  *path,
                 GtkTreeIter  *iter,
                 gpointer      user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  gtk_tree_view_expand_all (GTK_TREE_VIEW (gdu_window_get_widget (window, "device-tree-treeview")));
}

static void select_details_page (GduWindow    *window,
                                 UDisksObject *object,
                                 DetailsPage   page);

static void
set_selected_object (GduWindow    *window,
                     UDisksObject *object)
{
  UDisksIScsiCollection *collection;

  if (object != NULL)
    {
      if (udisks_object_peek_lun (object) != NULL ||
          udisks_object_peek_block_device (object) != NULL)
        {
          select_details_page (window, object, DETAILS_PAGE_DEVICE);
        }
      else if (udisks_object_peek_iscsi_target (object) != NULL)
        {
          select_details_page (window, object, DETAILS_PAGE_ISCSI_TARGET);
        }
      else if ((collection = udisks_object_peek_iscsi_collection (object)) != NULL &&
               g_strcmp0 (udisks_iscsi_collection_get_mechanism (collection), "sendtargets") == 0)
        {
          select_details_page (window, object, DETAILS_PAGE_ISCSI_SENDTARGETS_COLLECTION);
        }
      else
        {
          g_warning ("no page for object %s", g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
          select_details_page (window, NULL, DETAILS_PAGE_NOT_IMPLEMENTED);
        }
    }
  else
    {
      select_details_page (window, NULL, DETAILS_PAGE_NOT_SELECTED);
    }
}

static void
on_tree_selection_changed (GtkTreeSelection *tree_selection,
                           gpointer          user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  GtkTreeIter iter;
  GtkTreeModel *model;

  if (gtk_tree_selection_get_selected (tree_selection, &model, &iter))
    {
      UDisksObject *object;
      gtk_tree_model_get (model,
                          &iter,
                          GDU_DEVICE_TREE_MODEL_COLUMN_OBJECT,
                          &object,
                          -1);
      set_selected_object (window, object);
      g_object_unref (object);
    }
  else
    {
      set_selected_object (window, NULL);
    }
}

gboolean _gdu_application_get_running_from_source_tree (GduApplication *app);

static void
init_css (GduWindow *window)
{
  GtkCssProvider *provider;
  GError *error;
  const gchar *css =
"#devtab-grid-toolbar.toolbar {\n"
"    border-width: 1;\n"
"    border-radius: 3;\n"
"    border-style: solid;\n"
"    background-color: @theme_base_color;\n"
"}\n"
;

  provider = gtk_css_provider_new ();
  error = NULL;
  if (!gtk_css_provider_load_from_data (provider,
                                        css,
                                        -1,
                                        &error))
    {
      g_warning ("Can't parse custom CSS: %s\n", error->message);
      g_error_free (error);
      goto out;
    }

  gtk_style_context_add_provider_for_screen (gtk_widget_get_screen (GTK_WIDGET (window)),
                                             GTK_STYLE_PROVIDER (provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);

 out:
  ;
}

static void
gdu_window_constructed (GObject *object)
{
  GduWindow *window = GDU_WINDOW (object);
  GError *error;
  GtkNotebook *notebook;
  GtkTreeView *tree_view;
  GtkTreeViewColumn *column;
  GtkCellRenderer *renderer;
  GtkTreeSelection *selection;
  const gchar *path;
  GtkWidget *w;
  GtkWidget *label;
  GtkStyleContext *context;
  GDBusObjectManager *object_manager;
  GList *children, *l;

  init_css (window);

  /* chain up */
  if (G_OBJECT_CLASS (gdu_window_parent_class)->constructed != NULL)
    G_OBJECT_CLASS (gdu_window_parent_class)->constructed (object);

  window->builder = gtk_builder_new ();
  error = NULL;
  path = _gdu_application_get_running_from_source_tree (window->application)
    ? "../../data/ui/palimpsest.ui" :
      PACKAGE_DATA_DIR "/gnome-disk-utility/palimpsest.ui";
  if (gtk_builder_add_from_file (window->builder,
                                 path,
                                 &error) == 0)
    {
      g_error ("Error loading %s: %s", path, error->message);
      g_error_free (error);
    }

  w = gdu_window_get_widget (window, "palimpsest-hbox");
  gtk_widget_reparent (w, GTK_WIDGET (window));
  gtk_window_set_title (GTK_WINDOW (window), _("Disk Utility"));
  gtk_window_set_default_size (GTK_WINDOW (window), 800, 600);
  gtk_container_set_border_width (GTK_CONTAINER (window), 12);

  /* hide all children in the devtab list, otherwise the dialog is going to be huge by default */
  children = gtk_container_get_children (GTK_CONTAINER (gdu_window_get_widget (window, "devtab-drive-table")));
  for (l = children; l != NULL; l = l->next)
    {
      gtk_widget_hide (GTK_WIDGET (l->data));
      gtk_widget_set_no_show_all (GTK_WIDGET (l->data), TRUE);
    }
  g_list_free (children);
  children = gtk_container_get_children (GTK_CONTAINER (gdu_window_get_widget (window, "devtab-table")));
  for (l = children; l != NULL; l = l->next)
    {
      gtk_widget_hide (GTK_WIDGET (l->data));
      gtk_widget_set_no_show_all (GTK_WIDGET (l->data), TRUE);
    }

  notebook = GTK_NOTEBOOK (gdu_window_get_widget (window, "palimpsest-notebook"));
  gtk_notebook_set_show_tabs (notebook, FALSE);
  gtk_notebook_set_show_border (notebook, FALSE);

  context = gtk_widget_get_style_context (gdu_window_get_widget (window, "device-tree-scrolledwindow"));
  gtk_style_context_set_junction_sides (context, GTK_JUNCTION_BOTTOM);
  context = gtk_widget_get_style_context (gdu_window_get_widget (window, "device-tree-add-remove-toolbar"));
  gtk_style_context_add_class (context, GTK_STYLE_CLASS_INLINE_TOOLBAR);
  gtk_style_context_set_junction_sides (context, GTK_JUNCTION_TOP);

  context = gtk_widget_get_style_context (gdu_window_get_widget (window, "iscsitab-scrolledwindow"));
  gtk_style_context_set_junction_sides (context, GTK_JUNCTION_BOTTOM);
  context = gtk_widget_get_style_context (gdu_window_get_widget (window, "iscsitab-toolbar"));
  gtk_style_context_add_class (context, GTK_STYLE_CLASS_INLINE_TOOLBAR);
  gtk_style_context_set_junction_sides (context, GTK_JUNCTION_TOP);

  window->model = gdu_device_tree_model_new (window->client);

  tree_view = GTK_TREE_VIEW (gdu_window_get_widget (window, "device-tree-treeview"));

  label = gtk_label_new (NULL);
  gtk_label_set_markup_with_mnemonic (GTK_LABEL (label), _("_Storage Devices"));
  gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), GTK_WIDGET (tree_view));
  gtk_widget_show_all (label);

  gtk_tree_view_set_model (tree_view, GTK_TREE_MODEL (window->model));
  gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (window->model),
                                        GDU_DEVICE_TREE_MODEL_COLUMN_SORT_KEY,
                                        GTK_SORT_ASCENDING);

  selection = gtk_tree_view_get_selection (tree_view);
  gtk_tree_selection_set_select_function (selection, dont_select_headings, NULL, NULL);
  g_signal_connect (selection,
                    "changed",
                    G_CALLBACK (on_tree_selection_changed),
                    window);

  column = gtk_tree_view_column_new ();
  gtk_tree_view_column_set_widget (column, label);
  gtk_tree_view_append_column (tree_view, column);

  renderer = gtk_cell_renderer_text_new ();
  gtk_tree_view_column_pack_start (column, renderer, FALSE);
  gtk_tree_view_column_set_attributes (column,
                                       renderer,
                                       "markup", GDU_DEVICE_TREE_MODEL_COLUMN_HEADING_TEXT,
                                       "visible", GDU_DEVICE_TREE_MODEL_COLUMN_IS_HEADING,
                                       NULL);

  renderer = gtk_cell_renderer_pixbuf_new ();
  g_object_set (G_OBJECT (renderer),
                "stock-size", GTK_ICON_SIZE_DND,
                NULL);
  gtk_tree_view_column_pack_start (column, renderer, FALSE);
  gtk_tree_view_column_set_attributes (column,
                                       renderer,
                                       "gicon", GDU_DEVICE_TREE_MODEL_COLUMN_ICON,
                                       NULL);
  renderer = gtk_cell_renderer_text_new ();
  g_object_set (G_OBJECT (renderer),
                "ellipsize", PANGO_ELLIPSIZE_MIDDLE,
                NULL);
  gtk_tree_view_column_pack_start (column, renderer, FALSE);
  gtk_tree_view_column_set_attributes (column,
                                       renderer,
                                       "markup", GDU_DEVICE_TREE_MODEL_COLUMN_NAME,
                                       NULL);

  /* expand on insertion - hmm, I wonder if there's an easier way to do this */
  g_signal_connect (window->model,
                    "row-inserted",
                    G_CALLBACK (on_row_inserted),
                    window);
  gtk_tree_view_expand_all (tree_view);

  object_manager = udisks_client_get_object_manager (window->client);
  g_signal_connect (object_manager,
                    "object-added",
                    G_CALLBACK (on_object_added),
                    window);
  g_signal_connect (object_manager,
                    "object-removed",
                    G_CALLBACK (on_object_removed),
                    window);
  g_signal_connect (object_manager,
                    "interface-added",
                    G_CALLBACK (on_interface_added),
                    window);
  g_signal_connect (object_manager,
                    "interface-removed",
                    G_CALLBACK (on_interface_removed),
                    window);
  g_signal_connect (object_manager,
                    "interface-proxy-properties-changed",
                    G_CALLBACK (on_interface_proxy_properties_changed),
                    window);

  /* set up non-standard widgets that isn't in the .ui file */

  window->volume_grid = gdu_volume_grid_new (window->client);
  gtk_box_pack_start (GTK_BOX (gdu_window_get_widget (window, "devtab-grid-hbox")),
                      window->volume_grid,
                      TRUE, TRUE, 0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (gdu_window_get_widget (window, "devtab-volumes-label")),
                                 window->volume_grid);
  g_signal_connect (window->volume_grid,
                    "changed",
                    G_CALLBACK (on_volume_grid_changed),
                    window);

  context = gtk_widget_get_style_context (gdu_window_get_widget (window, "devtab-grid-toolbar"));
  gtk_widget_set_name (gdu_window_get_widget (window, "devtab-grid-toolbar"), "devtab-grid-toolbar");
  gtk_style_context_set_junction_sides (context, GTK_JUNCTION_TOP);

  /* devtab's Write Cache switch */
  window->write_cache_switch = gtk_switch_new ();
  gtk_box_pack_start (GTK_BOX (gdu_window_get_widget (window, "devtab-write-cache-hbox")),
                      window->write_cache_switch,
                      FALSE, TRUE, 0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (gdu_window_get_widget (window, "devtab-write-cache-label")),
                                 window->write_cache_switch);

  /* iSCSI tab's Connection switch */
  window->iscsi_connection_switch = gtk_switch_new ();
  g_signal_connect (window->iscsi_connection_switch,
                    "notify::active",
                    G_CALLBACK (iscsi_connection_switch_on_notify_active),
                    window);
  gtk_box_pack_start (GTK_BOX (gdu_window_get_widget (window, "iscsitab-connection-hbox")),
                      window->iscsi_connection_switch,
                      FALSE, TRUE, 0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (gdu_window_get_widget (window, "iscsitab-connection-label")),
                                 window->iscsi_connection_switch);

  /* actions */
  g_signal_connect (gtk_builder_get_object (window->builder, "devtab-action-generic"),
                    "activate",
                    G_CALLBACK (on_devtab_action_generic_activated),
                    window);
  g_signal_connect (gtk_builder_get_object (window->builder, "devtab-action-partition-create"),
                    "activate",
                    G_CALLBACK (on_devtab_action_partition_create_activated),
                    window);
  g_signal_connect (gtk_builder_get_object (window->builder, "devtab-action-mount"),
                    "activate",
                    G_CALLBACK (on_devtab_action_mount_activated),
                    window);
  g_signal_connect (gtk_builder_get_object (window->builder, "devtab-action-unmount"),
                    "activate",
                    G_CALLBACK (on_devtab_action_unmount_activated),
                    window);
  g_signal_connect (gtk_builder_get_object (window->builder, "devtab-action-eject"),
                    "activate",
                    G_CALLBACK (on_devtab_action_eject_activated),
                    window);
  g_signal_connect (gtk_builder_get_object (window->builder, "devtab-action-unlock"),
                    "activate",
                    G_CALLBACK (on_devtab_action_unlock_activated),
                    window);
  g_signal_connect (gtk_builder_get_object (window->builder, "devtab-action-lock"),
                    "activate",
                    G_CALLBACK (on_devtab_action_lock_activated),
                    window);
  g_signal_connect (gtk_builder_get_object (window->builder, "devtab-action-activate-swap"),
                    "activate",
                    G_CALLBACK (on_devtab_action_activate_swap_activated),
                    window);
  g_signal_connect (gtk_builder_get_object (window->builder, "devtab-action-deactivate-swap"),
                    "activate",
                    G_CALLBACK (on_devtab_action_deactivate_swap_activated),
                    window);
}

static void
gdu_window_get_property (GObject    *object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  GduWindow *window = GDU_WINDOW (object);

  switch (prop_id)
    {
    case PROP_APPLICATION:
      g_value_set_object (value, gdu_window_get_application (window));
      break;

    case PROP_CLIENT:
      g_value_set_object (value, gdu_window_get_client (window));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gdu_window_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  GduWindow *window = GDU_WINDOW (object);

  switch (prop_id)
    {
    case PROP_APPLICATION:
      window->application = g_value_dup_object (value);
      break;

    case PROP_CLIENT:
      window->client = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gdu_window_class_init (GduWindowClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->constructed  = gdu_window_constructed;
  gobject_class->finalize     = gdu_window_finalize;
  gobject_class->get_property = gdu_window_get_property;
  gobject_class->set_property = gdu_window_set_property;

  /**
   * GduWindow:application:
   *
   * The #GduApplication for the #GduWindow.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_APPLICATION,
                                   g_param_spec_object ("application",
                                                        "Application",
                                                        "The application for the window",
                                                        GDU_TYPE_APPLICATION,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * GduWindow:client:
   *
   * The #UDisksClient used by the #GduWindow.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_CLIENT,
                                   g_param_spec_object ("client",
                                                        "Client",
                                                        "The client used by the window",
                                                        UDISKS_TYPE_CLIENT,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}

GduWindow *
gdu_window_new (GduApplication *application,
                UDisksClient   *client)
{
  return GDU_WINDOW (g_object_new (GDU_TYPE_WINDOW,
                                   "application", application,
                                   "client", client,
                                   NULL));
}

GduApplication *
gdu_window_get_application   (GduWindow *window)
{
  g_return_val_if_fail (GDU_IS_WINDOW (window), NULL);
  return window->application;
}

UDisksClient *
gdu_window_get_client (GduWindow *window)
{
  g_return_val_if_fail (GDU_IS_WINDOW (window), NULL);
  return window->client;
}

GtkWidget *
gdu_window_get_widget (GduWindow    *window,
                       const gchar  *name)
{
  g_return_val_if_fail (GDU_IS_WINDOW (window), NULL);
  g_return_val_if_fail (name != NULL, NULL);
  return GTK_WIDGET (gtk_builder_get_object (window->builder, name));
}

static void
teardown_details_page (GduWindow    *window,
                       UDisksObject *object,
                       DetailsPage   page)
{
  //g_debug ("teardown for %s, page %d",
  //       object != NULL ? g_dbus_object_get_object_path (object) : "<none>",
  //         page);
  switch (page)
    {
    case DETAILS_PAGE_NOT_SELECTED:
      break;

    case DETAILS_PAGE_NOT_IMPLEMENTED:
      break;

    case DETAILS_PAGE_DEVICE:
      teardown_device_page (window);
      break;

    case DETAILS_PAGE_ISCSI_TARGET:
      teardown_iscsi_target_page (window);
      break;

    case DETAILS_PAGE_ISCSI_SENDTARGETS_COLLECTION:
      //TODO: teardown_iscsi_sendtargets_collection_page (window);
      break;
    }
}

typedef enum
{
  SET_MARKUP_FLAGS_NONE = 0,
  SET_MARKUP_FLAGS_HYPHEN_IF_EMPTY = (1<<0),
  SET_MARKUP_FLAGS_CHANGE_LINK = (1<<1)
} SetMarkupFlags;

static void
set_markup (GduWindow      *window,
            const gchar    *key_label_id,
            const gchar    *label_id,
            const gchar    *markup,
            SetMarkupFlags  flags)
{
  GtkWidget *key_label;
  GtkWidget *label;
  gchar *s;

  if (markup == NULL || strlen (markup) == 0)
    {
      if (flags & SET_MARKUP_FLAGS_HYPHEN_IF_EMPTY)
        markup = "—";
      else
        goto out;
    }

  key_label = gdu_window_get_widget (window, key_label_id);
  label = gdu_window_get_widget (window, label_id);

  /* TODO: utf-8 validate */

  if (flags & SET_MARKUP_FLAGS_CHANGE_LINK)
    {
      s = g_strdup_printf ("%s <small>— <a href=\"palimpsest://change/%s\">Change</a></small>", markup, label_id);
      if (g_hash_table_lookup (window->label_connections, label_id) == NULL)
        {
          g_signal_connect (label,
                            "activate-link",
                            G_CALLBACK (on_activate_link),
                            window);
          g_hash_table_insert (window->label_connections,
                               g_strdup (label_id),
                               label);
        }
    }
  else
    {
      s = g_strdup (markup);
    }
  gtk_label_set_markup (GTK_LABEL (label), s);
  g_free (s);
  gtk_widget_show (key_label);
  gtk_widget_show (label);

 out:
  ;
}

static void
set_size (GduWindow   *window,
          const gchar *key_label_id,
          const gchar *label_id,
          guint64      size)
{
  gchar *s;
  s = udisks_util_get_size_for_display (size, FALSE, TRUE);
  set_markup (window, key_label_id, label_id, s, SET_MARKUP_FLAGS_NONE);
  g_free (s);
}

static GList *
get_top_level_block_devices_for_lun (GduWindow   *window,
                                     const gchar *lun_object_path)
{
  GList *ret;
  GList *l;
  GList *object_proxies;
  GDBusObjectManager *object_manager;

  object_manager = udisks_client_get_object_manager (window->client);
  object_proxies = g_dbus_object_manager_get_objects (object_manager);

  ret = NULL;
  for (l = object_proxies; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlockDevice *block;

      block = udisks_object_get_block_device (object);
      if (block == NULL)
        continue;

      if (g_strcmp0 (udisks_block_device_get_lun (block), lun_object_path) == 0 &&
          !udisks_block_device_get_part_entry (block))
        {
          ret = g_list_append (ret, g_object_ref (object));
        }
      g_object_unref (block);
    }
  g_list_foreach (object_proxies, (GFunc) g_object_unref, NULL);
  g_list_free (object_proxies);
  return ret;
}

static gint
block_device_compare_on_preferred (UDisksObject *a,
                                   UDisksObject *b)
{
  return g_strcmp0 (udisks_block_device_get_preferred_device (udisks_object_peek_block_device (a)),
                    udisks_block_device_get_preferred_device (udisks_object_peek_block_device (b)));
}

/* ---------------------------------------------------------------------------------------------------- */

static void
setup_details_page (GduWindow     *window,
                    UDisksObject  *object,
                    DetailsPage    page)
{
  //g_debug ("setup for %s, page %d",
  //         object != NULL ? g_dbus_object_get_object_path (object) : "<none>",
  //         page);

  switch (page)
    {
    case DETAILS_PAGE_NOT_SELECTED:
      break;

    case DETAILS_PAGE_NOT_IMPLEMENTED:
      break;

    case DETAILS_PAGE_DEVICE:
      setup_device_page (window, object);
      break;

    case DETAILS_PAGE_ISCSI_TARGET:
      setup_iscsi_target_page (window, object);
      break;

    case DETAILS_PAGE_ISCSI_SENDTARGETS_COLLECTION:
      //TODO: setup_iscsi_sendtargets_collection_page (window, object);
      break;
    }
}

static void
update_details_page (GduWindow   *window,
                     DetailsPage  page)
{
  //g_debug ("update for %s, page %d",
  //         object != NULL ? g_dbus_object_get_object_path (object) : "<none>",
  //         page);

  switch (page)
    {
    case DETAILS_PAGE_NOT_SELECTED:
      break;

    case DETAILS_PAGE_NOT_IMPLEMENTED:
      break;

    case DETAILS_PAGE_DEVICE:
      update_device_page (window);
      break;

    case DETAILS_PAGE_ISCSI_TARGET:
      update_iscsi_target_page (window);
      break;

    case DETAILS_PAGE_ISCSI_SENDTARGETS_COLLECTION:
      //TODO: update_iscsi_sendtargets_collection_page (window);
      break;
    }
}

static void
select_details_page (GduWindow     *window,
                     UDisksObject  *object,
                     DetailsPage    page)
{
  GtkNotebook *notebook;

  notebook = GTK_NOTEBOOK (gdu_window_get_widget (window, "palimpsest-notebook"));

  teardown_details_page (window,
                         window->current_object,
                         window->current_page);

  window->current_page = page;
  if (window->current_object != NULL)
    g_object_unref (window->current_object);
  window->current_object = object != NULL ? g_object_ref (object) : NULL;

  gtk_notebook_set_current_page (notebook, page);

  setup_details_page (window,
                      window->current_object,
                      window->current_page);

  update_details_page (window, window->current_page);
}

static void
update_all (GduWindow     *window,
            UDisksObject  *object)
{
  switch (window->current_page)
    {
    case DETAILS_PAGE_NOT_SELECTED:
      /* Nothing to update */
      break;

    case DETAILS_PAGE_NOT_IMPLEMENTED:
      /* Nothing to update */
      break;

    case DETAILS_PAGE_DEVICE:
      /* this is a little too inclusive.. */
      if (gdu_volume_grid_includes_object (GDU_VOLUME_GRID (window->volume_grid), object))
        {
          update_details_page (window, window->current_page);
        }
      break;

    case DETAILS_PAGE_ISCSI_TARGET:
      if (object == window->current_object)
        {
          update_details_page (window, window->current_page);
        }
      /* Nothing to update */
      break;

    case DETAILS_PAGE_ISCSI_SENDTARGETS_COLLECTION:
      if (object == window->current_object)
        {
          update_details_page (window, window->current_page);
        }
      /* Nothing to update */
      break;
    }
}

static void
on_object_added (GDBusObjectManager  *manager,
                 GDBusObject         *object,
                 gpointer             user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  update_all (window, UDISKS_OBJECT (object));
}

static void
on_object_removed (GDBusObjectManager  *manager,
                   GDBusObject         *object,
                   gpointer             user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  update_all (window, UDISKS_OBJECT (object));
}

static void
on_interface_added (GDBusObjectManager  *manager,
                    GDBusObject         *object,
                    GDBusInterface      *interface,
                    gpointer             user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  update_all (window, UDISKS_OBJECT (object));
}

static void
on_interface_removed (GDBusObjectManager  *manager,
                      GDBusObject         *object,
                      GDBusInterface      *interface,
                      gpointer             user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  update_all (window, UDISKS_OBJECT (object));
}

static void
on_interface_proxy_properties_changed (GDBusObjectManagerClient   *manager,
                                       GDBusObjectProxy           *object_proxy,
                                       GDBusProxy                 *interface_proxy,
                                       GVariant                   *changed_properties,
                                       const gchar *const         *invalidated_properties,
                                       gpointer                    user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  update_all (window, UDISKS_OBJECT (object_proxy));
}

/* ---------------------------------------------------------------------------------------------------- */

static void
setup_device_page (GduWindow     *window,
                   UDisksObject  *object)
{
  UDisksLun *lun;
  UDisksBlockDevice *block;

  lun = udisks_object_peek_lun (object);
  block = udisks_object_peek_block_device (object);

  gdu_volume_grid_set_container_visible (GDU_VOLUME_GRID (window->volume_grid), FALSE);
  if (lun != NULL)
    {
      GList *block_devices;
      gchar *lun_name;
      gchar *lun_desc;
      GIcon *lun_icon;
      gchar *lun_media_desc;
      GIcon *lun_media_icon;

      /* TODO: for multipath, ensure e.g. mpathk is before sda, sdb */
      block_devices = get_top_level_block_devices_for_lun (window, g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
      block_devices = g_list_sort (block_devices, (GCompareFunc) block_device_compare_on_preferred);

      udisks_util_get_lun_info (lun,
                                &lun_name,
                                &lun_desc,
                                &lun_icon,
                                &lun_media_desc,
                                &lun_media_icon);
      if (block_devices != NULL)
        gdu_volume_grid_set_block_device (GDU_VOLUME_GRID (window->volume_grid), block_devices->data);
      else
        gdu_volume_grid_set_block_device (GDU_VOLUME_GRID (window->volume_grid), NULL);

      g_free (lun_name);
      g_free (lun_desc);
      g_object_unref (lun_icon);
      g_free (lun_media_desc);
      if (lun_media_icon != NULL)
        g_object_unref (lun_media_icon);

      g_list_foreach (block_devices, (GFunc) g_object_unref, NULL);
      g_list_free (block_devices);
    }
  else if (block != NULL)
    {
      gdu_volume_grid_set_block_device (GDU_VOLUME_GRID (window->volume_grid), object);
    }
  else
    {
      g_assert_not_reached ();
    }
}

static void
update_device_page_for_lun (GduWindow     *window,
                            UDisksObject  *object,
                            UDisksLun     *lun)
{
  gchar *s;
  GList *block_devices;
  GList *l;
  GString *str;
  const gchar *lun_vendor;
  const gchar *lun_model;
  gchar *name;
  gchar *description;
  gchar *media_description;
  GIcon *drive_icon;
  GIcon *media_icon;
  GtkWidget *w;
  guint64 size;

  //g_debug ("In update_device_page_for_lun() - selected=%s",
  //         object != NULL ? g_dbus_object_get_object_path (object) : "<nothing>");

  /* TODO: for multipath, ensure e.g. mpathk is before sda, sdb */
  block_devices = get_top_level_block_devices_for_lun (window, g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
  block_devices = g_list_sort (block_devices, (GCompareFunc) block_device_compare_on_preferred);

  udisks_util_get_lun_info (lun, &name, &description, &drive_icon, &media_description, &media_icon);

  lun_vendor = udisks_lun_get_vendor (lun);
  lun_model = udisks_lun_get_model (lun);

  str = g_string_new (NULL);
  for (l = block_devices; l != NULL; l = l->next)
    {
      UDisksObject *block_object = UDISKS_OBJECT (l->data);
      if (str->len > 0)
        g_string_append_c (str, ' ');
      g_string_append (str, udisks_block_device_get_preferred_device (udisks_object_peek_block_device (block_object)));
    }
  s = g_strdup_printf ("<big><b>%s</b></big>\n"
                       "<small><span foreground=\"#555555\">%s</span></small>",
                       description,
                       str->str);
  g_string_free (str, TRUE);
  w = gdu_window_get_widget (window, "devtab-drive-value-label");
  gtk_label_set_markup (GTK_LABEL (w), s);
  gtk_widget_show (w);
  g_free (s);
  w = gdu_window_get_widget (window, "devtab-drive-image");
  if (media_icon != NULL)
    gtk_image_set_from_gicon (GTK_IMAGE (w), media_icon, GTK_ICON_SIZE_DIALOG);
  else
    gtk_image_set_from_gicon (GTK_IMAGE (w), drive_icon, GTK_ICON_SIZE_DIALOG);
  gtk_widget_show (w);

  if (strlen (lun_vendor) == 0)
    s = g_strdup (lun_model);
  else if (strlen (lun_model) == 0)
    s = g_strdup (lun_vendor);
  else
    s = g_strconcat (lun_vendor, " ", lun_model, NULL);
  set_markup (window,
              "devtab-model-label",
              "devtab-model-value-label", s, SET_MARKUP_FLAGS_HYPHEN_IF_EMPTY);
  g_free (s);
  set_markup (window,
              "devtab-serial-number-label",
              "devtab-serial-number-value-label",
              udisks_lun_get_serial (lun), SET_MARKUP_FLAGS_NONE);
  set_markup (window,
              "devtab-firmware-version-label",
              "devtab-firmware-version-value-label",
              udisks_lun_get_revision (lun), SET_MARKUP_FLAGS_NONE);
  set_markup (window,
              "devtab-wwn-label",
              "devtab-wwn-value-label",
              udisks_lun_get_wwn (lun), SET_MARKUP_FLAGS_NONE);
  /* TODO: get this from udisks */
  gtk_switch_set_active (GTK_SWITCH (window->write_cache_switch), TRUE);
  gtk_widget_show (gdu_window_get_widget (window, "devtab-write-cache-label"));
  gtk_widget_set_no_show_all (gdu_window_get_widget (window, "devtab-write-cache-hbox"), FALSE);
  gtk_widget_show_all (gdu_window_get_widget (window, "devtab-write-cache-hbox"));

  size = udisks_lun_get_size (lun);
  if (size > 0)
    {
      s = udisks_util_get_size_for_display (size, FALSE, TRUE);
      set_markup (window,
                  "devtab-drive-size-label",
                  "devtab-drive-size-value-label",
                  s, SET_MARKUP_FLAGS_HYPHEN_IF_EMPTY);
      g_free (s);
      set_markup (window,
                  "devtab-media-label",
                  "devtab-media-value-label",
                  media_description, SET_MARKUP_FLAGS_NONE);
    }
  else
    {
      set_markup (window,
                  "devtab-drive-size-label",
                  "devtab-drive-size-value-label",
                  "",
                  SET_MARKUP_FLAGS_HYPHEN_IF_EMPTY);
      set_markup (window,
                  "devtab-media-label",
                  "devtab-media-value-label",
                  "",
                  SET_MARKUP_FLAGS_HYPHEN_IF_EMPTY);
    }

  if (udisks_lun_get_media_removable (lun))
    {
      gtk_action_set_visible (GTK_ACTION (gtk_builder_get_object (window->builder,
                                                                  "devtab-action-eject")), TRUE);
    }

  g_list_foreach (block_devices, (GFunc) g_object_unref, NULL);
  g_list_free (block_devices);
  if (media_icon != NULL)
    g_object_unref (media_icon);
  g_object_unref (drive_icon);
  g_free (description);
  g_free (media_description);
  g_free (name);
}

static UDisksObject *
lookup_cleartext_device_for_crypto_device (UDisksClient  *client,
                                           const gchar   *object_path)
{
  GDBusObjectManager *object_manager;
  UDisksObject *ret;
  GList *objects;
  GList *l;

  ret = NULL;

  object_manager = udisks_client_get_object_manager (client);
  objects = g_dbus_object_manager_get_objects (object_manager);
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlockDevice *block;

      block = udisks_object_peek_block_device (object);
      if (block == NULL)
        continue;

      if (g_strcmp0 (udisks_block_device_get_crypto_backing_device (block),
                     object_path) == 0)
        {
          ret = g_object_ref (object);
          goto out;
        }
    }

 out:
  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
  return ret;
}

static void
update_device_page_for_block (GduWindow          *window,
                              UDisksObject       *object,
                              UDisksBlockDevice  *block,
                              guint64             size)
{
  const gchar *backing_file;
  const gchar *usage;
  const gchar *type;
  const gchar *version;
  gint partition_type;
  gchar *type_for_display;

  //g_debug ("In update_device_page_for_block() - size=%" G_GUINT64_FORMAT " selected=%s",
  //         size,
  //         object != NULL ? g_dbus_object_get_object_path (object) : "<nothing>");

  set_markup (window,
              "devtab-device-label",
              "devtab-device-value-label",
              udisks_block_device_get_preferred_device (block), SET_MARKUP_FLAGS_NONE);
  set_size (window,
            "devtab-size-label",
            "devtab-size-value-label",
            size);
  backing_file = udisks_block_device_get_loop_backing_file (block);
  if (strlen (backing_file) > 0)
    {
      set_markup (window,
                  "devtab-backing-file-label",
                  "devtab-backing-file-value-label",
                  backing_file, SET_MARKUP_FLAGS_NONE);
    }

  usage = udisks_block_device_get_id_usage (block);
  type = udisks_block_device_get_id_type (block);
  version = udisks_block_device_get_id_version (block);
  partition_type = strtol (udisks_block_device_get_part_entry_type (block), NULL, 0);

  if (udisks_block_device_get_part_entry (block) &&
      g_strcmp0 (udisks_block_device_get_part_entry_scheme (block), "mbr") == 0 &&
      (partition_type == 0x05 || partition_type == 0x0f || partition_type == 0x85))
    {
      type_for_display = g_strdup (_("Extended Partition"));
    }
  else
    {
      type_for_display = udisks_util_get_id_for_display (usage, type, version, TRUE);
    }
  set_markup (window,
              "devtab-volume-type-label",
              "devtab-volume-type-value-label",
              type_for_display, SET_MARKUP_FLAGS_NONE);
  g_free (type_for_display);

  set_markup (window,
              "devtab-volume-label-label",
              "devtab-volume-label-value-label",
              udisks_block_device_get_id_label (block),
              SET_MARKUP_FLAGS_CHANGE_LINK);

  set_markup (window,
              "devtab-volume-uuid-label",
              "devtab-volume-uuid-value-label",
              udisks_block_device_get_id_uuid (block),
              SET_MARKUP_FLAGS_NONE);

  if (udisks_block_device_get_part_entry (block))
    {
      const gchar *partition_label;
      gchar *type_for_display;

      type_for_display = udisks_util_get_part_type_for_display (udisks_block_device_get_part_entry_scheme (block),
                                                                udisks_block_device_get_part_entry_type (block));

      partition_label = udisks_block_device_get_part_entry_label (block);
      set_markup (window,
                  "devtab-volume-partition-type-label",
                  "devtab-volume-partition-type-value-label",
                  type_for_display,
                  SET_MARKUP_FLAGS_CHANGE_LINK);
      set_markup (window,
                  "devtab-volume-partition-label-label",
                  "devtab-volume-partition-label-value-label",
                  partition_label,
                  SET_MARKUP_FLAGS_CHANGE_LINK);
      g_free (type_for_display);
    }
  else
    {
      UDisksObject *lun_object;
      lun_object = (UDisksObject *) g_dbus_object_manager_get_object (udisks_client_get_object_manager (window->client),
                                                                      udisks_block_device_get_lun (block));
      if (lun_object != NULL)
        {
          UDisksLun *lun;
          lun = udisks_object_peek_lun (lun_object);
          if (udisks_lun_get_media_removable (lun))
            {
              gtk_action_set_visible (GTK_ACTION (gtk_builder_get_object (window->builder,
                                                                          "devtab-action-eject")), TRUE);
            }
          g_object_unref (lun_object);
        }
    }

  if (g_strcmp0 (udisks_block_device_get_id_usage (block), "filesystem") == 0)
    {
      UDisksFilesystem *filesystem;
      filesystem = udisks_object_peek_filesystem (object);
      if (filesystem != NULL)
        {
          const gchar *const *mount_points;
          mount_points = udisks_filesystem_get_mount_points (filesystem);
          if (g_strv_length ((gchar **) mount_points) > 0)
            {
              gchar *mount_points_for_display;
              /* TODO: right now we only display the first mount point; could be we need to display
               * more than just that...
               */

              if (g_strcmp0 (mount_points[0], "/") == 0)
                {
                  /* Translators: This is shown for a device mounted at the filesystem root / - we show
                   * this text instead of '/', because '/' is too small to hit as a hyperlink
                   */
                  mount_points_for_display = g_strdup_printf ("<a href=\"file:///\">%s</a>", _("Root Filesystem (/)"));
                }
              else
                {
                  mount_points_for_display = g_strdup_printf ("<a href=\"file://%s\">%s</a>",
                                                              mount_points[0], mount_points[0]);
                }
              set_markup (window,
                          "devtab-volume-filesystem-mount-point-label",
                          "devtab-volume-filesystem-mount-point-value-label",
                          mount_points_for_display,
                          SET_MARKUP_FLAGS_NONE);
              g_free (mount_points_for_display);

              gtk_action_set_visible (GTK_ACTION (gtk_builder_get_object (window->builder,
                                                                          "devtab-action-unmount")), TRUE);
            }
          else
            {
              gtk_action_set_visible (GTK_ACTION (gtk_builder_get_object (window->builder,
                                                                          "devtab-action-mount")), TRUE);
            }
        }
    }
  else if (g_strcmp0 (udisks_block_device_get_id_usage (block), "other") == 0 &&
           g_strcmp0 (udisks_block_device_get_id_type (block), "swap") == 0)
    {
      UDisksSwapspace *swapspace;
      swapspace = udisks_object_peek_swapspace (object);
      if (swapspace != NULL)
        {
          if (udisks_swapspace_get_active (swapspace))
            {
              gtk_action_set_visible (GTK_ACTION (gtk_builder_get_object (window->builder,
                                                                          "devtab-action-deactivate-swap")), TRUE);
            }
          else
            {
              gtk_action_set_visible (GTK_ACTION (gtk_builder_get_object (window->builder,
                                                                          "devtab-action-activate-swap")), TRUE);
            }
        }
    }
  else if (g_strcmp0 (udisks_block_device_get_id_usage (block), "crypto") == 0)
    {
      UDisksObject *cleartext_device;

      cleartext_device = lookup_cleartext_device_for_crypto_device (window->client,
                                                                    g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
      if (cleartext_device != NULL)
        {
          gtk_action_set_visible (GTK_ACTION (gtk_builder_get_object (window->builder,
                                                                      "devtab-action-lock")), TRUE);
          g_object_unref (cleartext_device);
        }
      else
        {
          gtk_action_set_visible (GTK_ACTION (gtk_builder_get_object (window->builder,
                                                                      "devtab-action-unlock")), TRUE);
        }
    }
}

static void
update_device_page_for_no_media (GduWindow          *window,
                                 UDisksObject       *object,
                                 UDisksBlockDevice  *block)
{
  //g_debug ("In update_device_page_for_no_media() - selected=%s",
  //         object != NULL ? g_dbus_object_get_object_path (object) : "<nothing>");
}

static void
update_device_page_for_free_space (GduWindow          *window,
                                   UDisksObject       *object,
                                   UDisksBlockDevice  *block,
                                   guint64             size)
{
  //g_debug ("In update_device_page_for_free_space() - size=%" G_GUINT64_FORMAT " selected=%s",
  //         size,
  //         object != NULL ? g_dbus_object_get_object_path (object) : "<nothing>");

  set_markup (window,
              "devtab-device-label",
              "devtab-device-value-label",
              udisks_block_device_get_preferred_device (block), SET_MARKUP_FLAGS_NONE);
  set_size (window,
            "devtab-size-label",
            "devtab-size-value-label",
            size);
  set_markup (window,
              "devtab-volume-type-label",
              "devtab-volume-type-value-label",
              _("Unallocated Space"),
              SET_MARKUP_FLAGS_NONE);
  gtk_action_set_visible (GTK_ACTION (gtk_builder_get_object (window->builder,
                                                              "devtab-action-partition-create")), TRUE);
}

static void
update_device_page (GduWindow *window)
{
  UDisksObject *object;
  GduVolumeGridElementType type;
  UDisksBlockDevice *block;
  UDisksLun *lun;
  guint64 size;
  GList *children;
  GList *l;

  /* first hide everything */
  gtk_container_foreach (GTK_CONTAINER (gdu_window_get_widget (window, "devtab-drive-table")),
                         (GtkCallback) gtk_widget_hide, NULL);
  gtk_container_foreach (GTK_CONTAINER (gdu_window_get_widget (window, "devtab-table")),
                         (GtkCallback) gtk_widget_hide, NULL);
  children = gtk_action_group_list_actions (GTK_ACTION_GROUP (gtk_builder_get_object (window->builder, "devtab-actions")));
  for (l = children; l != NULL; l = l->next)
    gtk_action_set_visible (GTK_ACTION (l->data), FALSE);
  g_list_free (children);

  /* always show the generic toolbar item */
  gtk_action_set_visible (GTK_ACTION (gtk_builder_get_object (window->builder,
                                                              "devtab-action-generic")), TRUE);


  object = window->current_object;
  lun = udisks_object_peek_lun (window->current_object);
  block = udisks_object_peek_block_device (window->current_object);
  type = gdu_volume_grid_get_selected_type (GDU_VOLUME_GRID (window->volume_grid));
  size = gdu_volume_grid_get_selected_size (GDU_VOLUME_GRID (window->volume_grid));

  if (lun != NULL)
    update_device_page_for_lun (window, object, lun);

  if (type == GDU_VOLUME_GRID_ELEMENT_TYPE_CONTAINER)
    {
      if (block != NULL)
        update_device_page_for_block (window, object, block, size);
    }
  else
    {
      object = gdu_volume_grid_get_selected_device (GDU_VOLUME_GRID (window->volume_grid));
      if (object == NULL)
        object = gdu_volume_grid_get_block_device (GDU_VOLUME_GRID (window->volume_grid));
      if (object != NULL)
        {
          block = udisks_object_peek_block_device (object);
          switch (type)
            {
            case GDU_VOLUME_GRID_ELEMENT_TYPE_CONTAINER:
              g_assert_not_reached (); /* already handled above */
              break;

            case GDU_VOLUME_GRID_ELEMENT_TYPE_DEVICE:
              update_device_page_for_block (window, object, block, size);
              break;

            case GDU_VOLUME_GRID_ELEMENT_TYPE_NO_MEDIA:
              update_device_page_for_no_media (window, object, block);
              break;

            case GDU_VOLUME_GRID_ELEMENT_TYPE_FREE_SPACE:
              update_device_page_for_free_space (window, object, block, size);
              break;
            }
        }
    }
}

static void
teardown_device_page (GduWindow *window)
{
  gdu_volume_grid_set_block_device (GDU_VOLUME_GRID (window->volume_grid), NULL);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
iscsi_target_format_portal_address (GtkCellLayout   *cell_layout,
                                    GtkCellRenderer *renderer,
                                    GtkTreeModel    *tree_model,
                                    GtkTreeIter     *iter,
                                    gpointer         user_data)
{
  /* GduWindow *window = GDU_WINDOW (user_data); */
  gchar *portal_address;
  gint portal_port;
  gchar *markup;

  gtk_tree_model_get (tree_model,
                      iter,
                      GDU_ISCSI_PATH_MODEL_COLUMN_PORTAL_ADDRESS, &portal_address,
                      GDU_ISCSI_PATH_MODEL_COLUMN_PORTAL_PORT, &portal_port,
                      -1);

  /* only show port if it is non-standard */
  if (portal_port != 3260)
    markup = g_strdup_printf ("%s:%d", portal_address, portal_port);
  else
    markup = g_strdup (portal_address);

  g_object_set (renderer,
                "markup", markup,
                NULL);

  g_free (markup);
  g_free (portal_address);
}

static void
iscsi_target_login_cb (UDisksIScsiTarget *target,
                       GAsyncResult      *res,
                       gpointer           user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  GError *error;
  error = NULL;
  if (!udisks_iscsi_target_call_login_finish (target, res, &error))
    {
      gdu_window_show_error (window,
                             _("Error logging in to iSCSI target"),
                             error);
      g_error_free (error);
    }
  g_object_unref (window);
}

static void
iscsi_target_logout_cb (UDisksIScsiTarget *target,
                        GAsyncResult      *res,
                        gpointer           user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  GError *error;
  error = NULL;
  if (!udisks_iscsi_target_call_logout_finish (target, res, &error))
    {
      gdu_window_show_error (window,
                             _("Error logging out of iSCSI target"),
                             error);
      g_error_free (error);
    }
  g_object_unref (window);
}

static void
on_iscsi_active_toggled (GtkCellRendererToggle *renderer,
                         gchar                 *path,
                         gpointer               user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  gboolean is_active;
  GtkTreeView *tree_view;
  GtkTreeModel *tree_model;
  GtkTreeIter iter;
  gchar *portal_address;
  gchar *iface_name;
  gint portal_port;
  UDisksIScsiTarget *target;

  portal_address = NULL;
  iface_name = NULL;

  tree_view = GTK_TREE_VIEW (gdu_window_get_widget (window, "iscsi-connections-treeview"));
  tree_model = gtk_tree_view_get_model (tree_view);

  target = udisks_object_peek_iscsi_target (window->current_object);
  if (target == NULL)
    {
      g_warning ("Expected selected object to be an iSCSI target");
      goto out;
    }

  if (!gtk_tree_model_get_iter_from_string (tree_model,
                                            &iter,
                                            path))
    {
      g_warning ("Unable to get tree iter");
      goto out;
    }

  gtk_tree_model_get (tree_model,
                      &iter,
                      GDU_ISCSI_PATH_MODEL_COLUMN_PORTAL_ADDRESS, &portal_address,
                      GDU_ISCSI_PATH_MODEL_COLUMN_PORTAL_PORT, &portal_port,
                      GDU_ISCSI_PATH_MODEL_COLUMN_INTERFACE, &iface_name,
                      -1);

  is_active = gtk_cell_renderer_toggle_get_active (renderer);
  if (is_active)
    {
          const gchar *options[] = {NULL};
          udisks_iscsi_target_call_logout (target,
                                           options,
                                           portal_address,
                                           portal_port,
                                           iface_name,
                                           NULL,  /* GCancellable* */
                                           (GAsyncReadyCallback) iscsi_target_login_cb,
                                           g_object_ref (window));
    }
  else
    {
          const gchar *options[] = {NULL};
          udisks_iscsi_target_call_login (target,
                                          options,
                                          portal_address,
                                          portal_port,
                                          iface_name,
                                          NULL,  /* GCancellable* */
                                          (GAsyncReadyCallback) iscsi_target_login_cb,
                                          g_object_ref (window));
    }

 out:
  g_free (portal_address);
  g_free (iface_name);
}

static void
init_iscsi_target_page (GduWindow   *window)
{
  static volatile gsize init_val = 0;
  GtkTreeView *tree_view;
  /* GtkTreeSelection *selection; */
  GtkTreeViewColumn *column;
  GtkCellRenderer *renderer;

  if (!g_once_init_enter (&init_val))
    goto out;

  tree_view = GTK_TREE_VIEW (gdu_window_get_widget (window, "iscsi-connections-treeview"));
  gtk_tree_view_set_rules_hint (tree_view, TRUE);
#if 0
  selection = gtk_tree_view_get_selection (tree_view);
  gtk_tree_selection_set_select_function (selection, dont_select_headings, NULL, NULL);
  g_signal_connect (selection,
                    "changed",
                    G_CALLBACK (on_tree_selection_changed),
                    window);
#endif

  column = gtk_tree_view_column_new ();
  gtk_tree_view_append_column (tree_view, column);
  renderer = gtk_cell_renderer_toggle_new ();
  gtk_tree_view_column_pack_end (column, renderer, FALSE);
  gtk_tree_view_column_set_attributes (column, renderer,
                                       "active", GDU_ISCSI_PATH_MODEL_COLUMN_ACTIVE, NULL);
  g_signal_connect (renderer,
                    "toggled",
                    G_CALLBACK (on_iscsi_active_toggled),
                    window);

  column = gtk_tree_view_column_new ();
  gtk_tree_view_column_set_title (column, _("Portal"));
  gtk_tree_view_append_column (tree_view, column);
  renderer = gtk_cell_renderer_text_new ();
  gtk_tree_view_column_pack_start (column, renderer, TRUE);
  gtk_tree_view_column_set_alignment (column, 0.0);
  gtk_cell_layout_set_cell_data_func (GTK_CELL_LAYOUT (column),
                                      renderer,
                                      iscsi_target_format_portal_address,
                                      window,
                                      NULL);

  column = gtk_tree_view_column_new ();
  gtk_tree_view_column_set_title (column, _("Network Interface"));
  gtk_tree_view_append_column (tree_view, column);
  renderer = gtk_cell_renderer_text_new ();
  gtk_tree_view_column_pack_start (column, renderer, FALSE);
  gtk_tree_view_column_set_attributes (column, renderer,
                                       "markup", GDU_ISCSI_PATH_MODEL_COLUMN_INTERFACE, NULL);

  column = gtk_tree_view_column_new ();
  gtk_tree_view_column_set_title (column, _("TPGT"));
  gtk_tree_view_append_column (tree_view, column);
  renderer = gtk_cell_renderer_text_new ();
  gtk_tree_view_column_pack_start (column, renderer, FALSE);
  gtk_tree_view_column_set_attributes (column, renderer,
                                       "markup", GDU_ISCSI_PATH_MODEL_COLUMN_TPGT, NULL);

  column = gtk_tree_view_column_new ();
  gtk_tree_view_column_set_title (column, _("Status"));
  gtk_tree_view_append_column (tree_view, column);
  renderer = gtk_cell_renderer_text_new ();
  gtk_tree_view_column_pack_start (column, renderer, FALSE);
  gtk_tree_view_column_set_attributes (column, renderer,
                                       "markup", GDU_ISCSI_PATH_MODEL_COLUMN_STATUS, NULL);

  g_once_init_leave (&init_val, 1);
 out:
  ;
}

static gboolean
iscsi_target_has_active_connections (UDisksIScsiTarget *target)
{
  GVariant *portals_and_interfaces;
  GVariantIter portal_iter;
  GVariantIter *iface_iter;
  gboolean ret;

  ret = FALSE;
  portals_and_interfaces = udisks_iscsi_target_get_portals_and_interfaces (target);
  g_variant_iter_init (&portal_iter, portals_and_interfaces);
  while (g_variant_iter_next (&portal_iter,
                              "(^&ayiia(ays))",
                              NULL, /* &portal_adress */
                              NULL, /* &port */
                              NULL, /* &tpgt */
                              &iface_iter))
    {
      const gchar *state;
      while (g_variant_iter_next (iface_iter,
                                  "(^&ays)",
                                  NULL, /* &iface_name */
                                  &state))
        {
          if (g_strcmp0 (state, "LOGGED_IN") == 0)
            {
              ret = TRUE;
              goto out;
            }
        }
    }
 out:
  return ret;
}


static void
update_iscsi_target_page (GduWindow   *window)
{
  GList *children;
  GList *l;
  UDisksIScsiTarget *target;

  /* first hide everything */
  children = gtk_container_get_children (GTK_CONTAINER (gdu_window_get_widget (window, "iscsitab-table")));
  for (l = children; l != NULL; l = l->next)
    {
      GtkWidget *child = GTK_WIDGET (l->data);
      gtk_widget_hide (child);
    }
  g_list_free (children);

  target = udisks_object_peek_iscsi_target (window->current_object);
  /* TODO: get Alias from somewhere */
  set_markup (window,
              "iscsitab-alias-label",
              "iscsitab-alias-value-label",
              "", SET_MARKUP_FLAGS_HYPHEN_IF_EMPTY);
  set_markup (window,
              "iscsitab-name-label",
              "iscsitab-name-value-label",
              udisks_iscsi_target_get_name (target), SET_MARKUP_FLAGS_NONE);

  gtk_switch_set_active (GTK_SWITCH (window->iscsi_connection_switch),
                         iscsi_target_has_active_connections (target));
  gtk_widget_show (gdu_window_get_widget (window, "iscsitab-connection-label"));
  gtk_widget_show_all (gdu_window_get_widget (window, "iscsitab-connection-hbox"));

}

static void
iscsi_connection_switch_on_notify_active (GObject     *object,
                                          GParamSpec  *pspec,
                                          gpointer     user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  gboolean active;
  gboolean has_connections;
  UDisksIScsiTarget *target;

  target = udisks_object_peek_iscsi_target (window->current_object);
  if (target == NULL)
    {
      g_warning ("Expected selected object to be an iSCSI target");
      goto out;
    }

  active = !! gtk_switch_get_active (GTK_SWITCH (window->iscsi_connection_switch));
  has_connections = !! iscsi_target_has_active_connections (target);
  if (active != has_connections)
    {
      if (!has_connections)
        {
          const gchar *options[] = {NULL};
          udisks_iscsi_target_call_login (target,
                                          options,
                                          "", /* portal_address */
                                          0, /* portal_port */
                                          "", /* interface_name */
                                          NULL,  /* GCancellable* */
                                          (GAsyncReadyCallback) iscsi_target_login_cb,
                                          g_object_ref (window));
        }
      else
        {
          const gchar *options[] = {NULL};
          udisks_iscsi_target_call_logout (target,
                                           options,
                                           "", /* portal_address */
                                           0, /* portal_port */
                                           "", /* interface_name */
                                           NULL,  /* GCancellable* */
                                           (GAsyncReadyCallback) iscsi_target_logout_cb,
                                           g_object_ref (window));
        }
    }
  gtk_switch_set_active (GTK_SWITCH (window->iscsi_connection_switch), has_connections);

 out:
  ;
}

static void
setup_iscsi_target_page (GduWindow    *window,
                         UDisksObject *object)
{
  GtkTreeView *tree_view;
  GduIScsiPathModel *model;
  GtkTreeIter first_iter;

  init_iscsi_target_page (window);

  tree_view = GTK_TREE_VIEW (gdu_window_get_widget (window, "iscsi-connections-treeview"));
  model = gdu_iscsi_path_model_new (window->client, object);
  gtk_tree_view_set_model (tree_view, GTK_TREE_MODEL (model));
  /* select the first row */
  if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (model), &first_iter))
    gtk_tree_selection_select_iter (gtk_tree_view_get_selection (tree_view), &first_iter);
  g_object_unref (model);
}

static void
teardown_iscsi_target_page (GduWindow *window)
{
  GtkTreeView *tree_view;

  tree_view = GTK_TREE_VIEW (gdu_window_get_widget (window, "iscsi-connections-treeview"));
  gtk_tree_view_set_model (tree_view, NULL);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_volume_grid_changed (GduVolumeGrid  *grid,
                        gpointer        user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  update_device_page (window);
}

/* ---------------------------------------------------------------------------------------------------- */

/* TODO: right now we show a MessageDialog but we could do things like an InfoBar etc */
static void
gdu_window_show_error (GduWindow   *window,
                       const gchar *message,
                       GError      *orig_error)
{
  GtkWidget *dialog;
  GError *error;

  /* Never show an error if it's because the user dismissed the
   * authentication dialog himself
   */
  if (orig_error->domain == UDISKS_ERROR &&
      orig_error->code == UDISKS_ERROR_NOT_AUTHORIZED_DISMISSED)
    goto no_dialog;

  error = g_error_copy (orig_error);
  if (g_dbus_error_is_remote_error (error))
    g_dbus_error_strip_remote_error (error);

  dialog = gtk_message_dialog_new_with_markup (GTK_WINDOW (window),
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_ERROR,
                                               GTK_BUTTONS_CLOSE,
                                               "<big><b>%s</b></big>",
                                               message);
  gtk_message_dialog_format_secondary_markup (GTK_MESSAGE_DIALOG (dialog),
                                              "%s",
                                              error->message);
  g_error_free (error);
  gtk_dialog_run (GTK_DIALOG (dialog));
  gtk_widget_destroy (dialog);

 no_dialog:
  ;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  GtkWidget *dialog;
  gchar *orig_label;
} ChangeFilesystemLabelData;

static void
on_change_filesystem_label_entry_changed (GtkEditable *editable,
                                          gpointer     user_data)
{
  ChangeFilesystemLabelData *data = user_data;
  gboolean sensitive;

  sensitive = FALSE;
  if (g_strcmp0 (gtk_entry_get_text (GTK_ENTRY (editable)), data->orig_label) != 0)
    {
      sensitive = TRUE;
    }

  gtk_dialog_set_response_sensitive (GTK_DIALOG (data->dialog),
                                     GTK_RESPONSE_OK,
                                     sensitive);
}

static void
change_filesystem_label_cb (UDisksBlockDevice *block,
                            GAsyncResult      *res,
                            gpointer           user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  GError *error;

  error = NULL;
  if (!udisks_block_device_call_set_label_finish (block,
                                                  res,
                                                  &error))
    {
      gdu_window_show_error (window,
                             _("Error setting label"),
                             error);
      g_error_free (error);
    }
  g_object_unref (window);
}

static void
on_change_filesystem_label (GduWindow *window)
{
  gint response;
  GtkWidget *dialog;
  GtkWidget *entry;
  UDisksObject *object;
  UDisksBlockDevice *block;
  const gchar *label;
  ChangeFilesystemLabelData data;
  const gchar *label_to_set;
  const gchar *options[] = {NULL};

  object = gdu_volume_grid_get_selected_device (GDU_VOLUME_GRID (window->volume_grid));
  g_assert (object != NULL);
  block = udisks_object_peek_block_device (object);
  g_assert (block != NULL);

  dialog = gdu_window_get_widget (window, "change-filesystem-label-dialog");
  entry = gdu_window_get_widget (window, "change-filesystem-label-entry");
  gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (window));
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

  label = udisks_block_device_get_id_label (block);
  g_signal_connect (entry,
                    "changed",
                    G_CALLBACK (on_change_filesystem_label_entry_changed),
                    &data);
  memset (&data, '\0', sizeof (ChangeFilesystemLabelData));
  data.dialog = dialog;
  data.orig_label = g_strdup (label);

  gtk_entry_set_text (GTK_ENTRY (entry), label);
  gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);

  gtk_widget_show_all (dialog);
  response = gtk_dialog_run (GTK_DIALOG (dialog));
  if (response != GTK_RESPONSE_OK)
    goto out;

  label_to_set = gtk_entry_get_text (GTK_ENTRY (entry));

  udisks_block_device_call_set_label (block,
                                      label_to_set,
                                      options, /* options */
                                      NULL, /* cancellable */
                                      (GAsyncReadyCallback) change_filesystem_label_cb,
                                      g_object_ref (window));

 out:
  g_signal_handlers_disconnect_by_func (entry,
                                        G_CALLBACK (on_change_filesystem_label_entry_changed),
                                        &data);
  gtk_widget_hide (dialog);
  g_free (data.orig_label);
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  GtkWidget *dialog;
  gchar *orig_type;
  const gchar **part_types;
} ChangePartitionTypeData;

static void
on_change_partition_type_combo_box_changed (GtkComboBox *combo_box,
                                            gpointer     user_data)
{
  ChangePartitionTypeData *data = user_data;
  gint active;
  gboolean sensitive;

  sensitive = FALSE;
  active = gtk_combo_box_get_active (combo_box);
  if (active > 0)
    {
      if (g_strcmp0 (data->part_types[active], data->orig_type) != 0)
        {
          sensitive = TRUE;
        }
    }

  gtk_dialog_set_response_sensitive (GTK_DIALOG (data->dialog),
                                     GTK_RESPONSE_OK,
                                     sensitive);
}

static void
on_change_partition_type (GduWindow *window)
{
  gint response;
  GtkWidget *dialog;
  GtkWidget *combo_box;
  UDisksObject *object;
  UDisksBlockDevice *block;
  const gchar *scheme;
  const gchar *cur_type;
  const gchar **part_types;
  guint n;
  gint active_index;
  ChangePartitionTypeData data;
  const gchar *type_to_set;

  object = gdu_volume_grid_get_selected_device (GDU_VOLUME_GRID (window->volume_grid));
  g_assert (object != NULL);
  block = udisks_object_peek_block_device (object);
  g_assert (block != NULL);

  dialog = gdu_window_get_widget (window, "change-partition-type-dialog");
  combo_box = gdu_window_get_widget (window, "change-partition-type-combo-box");
  gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (window));
  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

  scheme = udisks_block_device_get_part_entry_scheme (block);
  cur_type = udisks_block_device_get_part_entry_type (block);
  part_types = udisks_util_get_part_types_for_scheme (scheme);
  active_index = -1;
  gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (combo_box));
  for (n = 0; part_types != NULL && part_types[n] != NULL; n++)
    {
      const gchar *type;
      gchar *type_for_display;
      type = part_types[n];
      type_for_display = udisks_util_get_part_type_for_display (scheme, type);
      if (g_strcmp0 (type, cur_type) == 0)
        active_index = n;
      gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo_box), NULL, type_for_display);
      g_free (type_for_display);
    }

  g_signal_connect (combo_box,
                    "changed",
                    G_CALLBACK (on_change_partition_type_combo_box_changed),
                    &data);
  memset (&data, '\0', sizeof (ChangePartitionTypeData));
  data.dialog = dialog;
  data.orig_type = g_strdup (cur_type);
  data.part_types = part_types;

  if (active_index > 0)
    gtk_combo_box_set_active (GTK_COMBO_BOX (combo_box), active_index);

  gtk_widget_show_all (dialog);
  response = gtk_dialog_run (GTK_DIALOG (dialog));
  if (response != GTK_RESPONSE_OK)
    goto out;

  type_to_set = part_types[gtk_combo_box_get_active (GTK_COMBO_BOX (combo_box))];

  g_debug ("TODO: set partition type to %s", type_to_set);

 out:
  g_signal_handlers_disconnect_by_func (combo_box,
                                        G_CALLBACK (on_change_partition_type_combo_box_changed),
                                        &data);
  gtk_widget_hide (dialog);
  g_free (part_types);
  g_free (data.orig_type);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_change_partition_label (GduWindow *window)
{
  g_debug ("TODO: %s", G_STRFUNC);
}

static gboolean
on_activate_link (GtkLabel    *label,
                  const gchar *uri,
                  gpointer     user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  gboolean handled;

  handled = FALSE;
  if (!g_str_has_prefix (uri, "palimpsest://"))
    goto out;

  handled = TRUE;

  if (g_strcmp0 (uri, "palimpsest://change/devtab-volume-label-value-label") == 0)
    on_change_filesystem_label (window);
  else if (g_strcmp0 (uri, "palimpsest://change/devtab-volume-partition-type-value-label") == 0)
    on_change_partition_type (window);
  else if (g_strcmp0 (uri, "palimpsest://change/devtab-volume-partition-label-value-label") == 0)
    on_change_partition_label (window);
  else
    g_warning ("Unhandled action: %s", uri);

 out:
  return handled;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
mount_cb (UDisksFilesystem *filesystem,
          GAsyncResult     *res,
          gpointer          user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  GError *error;

  error = NULL;
  if (!udisks_filesystem_call_mount_finish (filesystem,
                                            NULL, /* out_mount_path */
                                            res,
                                            &error))
    {
      gdu_window_show_error (window,
                             _("Error mounting filesystem"),
                             error);
      g_error_free (error);
    }
  g_object_unref (window);
}

static void
on_devtab_action_mount_activated (GtkAction *action,
                                  gpointer   user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  UDisksObject *object;
  UDisksFilesystem *filesystem;
  const gchar *options[] = {NULL};

  object = gdu_volume_grid_get_selected_device (GDU_VOLUME_GRID (window->volume_grid));
  filesystem = udisks_object_peek_filesystem (object);
  udisks_filesystem_call_mount (filesystem,
                                "", /* filesystem type */
                                options, /* options */
                                NULL, /* cancellable */
                                (GAsyncReadyCallback) mount_cb,
                                g_object_ref (window));
}

/* ---------------------------------------------------------------------------------------------------- */

static void
unmount_cb (UDisksFilesystem *filesystem,
            GAsyncResult     *res,
            gpointer          user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  GError *error;

  error = NULL;
  if (!udisks_filesystem_call_unmount_finish (filesystem,
                                              res,
                                              &error))
    {
      gdu_window_show_error (window,
                             _("Error unmounting filesystem"),
                             error);
      g_error_free (error);
    }
  g_object_unref (window);
}

static void
on_devtab_action_unmount_activated (GtkAction *action,
                                    gpointer   user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  UDisksObject *object;
  UDisksFilesystem *filesystem;
  const gchar *options[] = {NULL};

  object = gdu_volume_grid_get_selected_device (GDU_VOLUME_GRID (window->volume_grid));
  filesystem = udisks_object_peek_filesystem (object);
  udisks_filesystem_call_unmount (filesystem,
                                  options, /* options */
                                  NULL, /* cancellable */
                                  (GAsyncReadyCallback) unmount_cb,
                                  g_object_ref (window));
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_devtab_action_generic_activated (GtkAction *action,
                                    gpointer   user_data)
{
  g_debug ("%s: TODO", G_STRFUNC);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_devtab_action_partition_create_activated (GtkAction *action,
                                             gpointer   user_data)
{
  g_debug ("%s: TODO", G_STRFUNC);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_devtab_action_eject_activated (GtkAction *action,
                                  gpointer   user_data)
{
  g_debug ("%s: TODO", G_STRFUNC);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_devtab_action_unlock_activated (GtkAction *action,
                                   gpointer   user_data)
{
  g_debug ("%s: TODO", G_STRFUNC);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_devtab_action_lock_activated (GtkAction *action,
                                 gpointer   user_data)
{
  g_debug ("%s: TODO", G_STRFUNC);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
swapspace_start_cb (UDisksSwapspace  *swapspace,
                    GAsyncResult     *res,
                    gpointer          user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  GError *error;

  error = NULL;
  if (!udisks_swapspace_call_start_finish (swapspace,
                                           res,
                                           &error))
    {
      gdu_window_show_error (window,
                             _("Error starting swap"),
                             error);
      g_error_free (error);
    }
  g_object_unref (window);
}

static void
on_devtab_action_activate_swap_activated (GtkAction *action, gpointer user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  UDisksObject *object;
  UDisksSwapspace *swapspace;
  const gchar *options[] = {NULL};

  object = gdu_volume_grid_get_selected_device (GDU_VOLUME_GRID (window->volume_grid));
  swapspace = udisks_object_peek_swapspace (object);
  udisks_swapspace_call_start (swapspace,
                               options, /* options */
                               NULL, /* cancellable */
                               (GAsyncReadyCallback) swapspace_start_cb,
                               g_object_ref (window));
}

static void
swapspace_stop_cb (UDisksSwapspace  *swapspace,
                   GAsyncResult     *res,
                   gpointer          user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  GError *error;

  error = NULL;
  if (!udisks_swapspace_call_stop_finish (swapspace,
                                          res,
                                          &error))
    {
      gdu_window_show_error (window,
                             _("Error stopping swap"),
                             error);
      g_error_free (error);
    }
  g_object_unref (window);
}

static void
on_devtab_action_deactivate_swap_activated (GtkAction *action, gpointer user_data)
{
  GduWindow *window = GDU_WINDOW (user_data);
  UDisksObject *object;
  UDisksSwapspace *swapspace;
  const gchar *options[] = {NULL};

  object = gdu_volume_grid_get_selected_device (GDU_VOLUME_GRID (window->volume_grid));
  swapspace = udisks_object_peek_swapspace (object);
  udisks_swapspace_call_stop (swapspace,
                              options, /* options */
                              NULL, /* cancellable */
                              (GAsyncReadyCallback) swapspace_stop_cb,
                              g_object_ref (window));
}

/* ---------------------------------------------------------------------------------------------------- */
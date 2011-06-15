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
#include <glib/gi18n.h>

#include "gdudevicetreemodel.h"
#include "gduutils.h"

struct _GduDeviceTreeModel
{
  GtkTreeStore parent_instance;

  UDisksClient *client;

  GList *current_luns;
  GtkTreeIter lun_iter;
  gboolean lun_iter_valid;

  GList *current_blocks;
  GtkTreeIter block_iter;
  gboolean block_iter_valid;

  /* These are UDisksObjects for collections, targets and LUNs */
  GList *current_iscsi_objects;
  GtkTreeIter iscsi_iter;
  gboolean iscsi_iter_valid;
};

typedef struct
{
  GtkTreeStoreClass parent_class;
} GduDeviceTreeModelClass;

enum
{
  PROP_0,
  PROP_CLIENT
};

G_DEFINE_TYPE (GduDeviceTreeModel, gdu_device_tree_model, GTK_TYPE_TREE_STORE);

static void coldplug (GduDeviceTreeModel *model);

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
gdu_device_tree_model_finalize (GObject *object)
{
  GduDeviceTreeModel *model = GDU_DEVICE_TREE_MODEL (object);
  GDBusObjectManager *object_manager;

  object_manager = udisks_client_get_object_manager (model->client);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_object_added),
                                        model);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_object_removed),
                                        model);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_interface_added),
                                        model);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_interface_removed),
                                        model);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_interface_proxy_properties_changed),
                                        model);

  g_list_foreach (model->current_luns, (GFunc) g_object_unref, NULL);
  g_list_free (model->current_luns);
  g_object_unref (model->client);

  G_OBJECT_CLASS (gdu_device_tree_model_parent_class)->finalize (object);
}

static void
gdu_device_tree_model_init (GduDeviceTreeModel *model)
{
}

static void
gdu_device_tree_model_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  GduDeviceTreeModel *model = GDU_DEVICE_TREE_MODEL (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_value_set_object (value, gdu_device_tree_model_get_client (model));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gdu_device_tree_model_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  GduDeviceTreeModel *model = GDU_DEVICE_TREE_MODEL (object);

  switch (prop_id)
    {
    case PROP_CLIENT:
      model->client = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  UDisksObject *object;
  const gchar *object_path;
  GtkTreeIter iter;
  gboolean found;
} FindIterData;

static gboolean
find_iter_for_object_cb (GtkTreeModel  *model,
                         GtkTreePath   *path,
                         GtkTreeIter   *iter,
                         gpointer       user_data)
{
  FindIterData *data = user_data;
  UDisksObject *iter_object;

  iter_object = NULL;

  gtk_tree_model_get (model,
                      iter,
                      GDU_DEVICE_TREE_MODEL_COLUMN_OBJECT, &iter_object,
                      -1);
  if (iter_object == NULL)
    goto out;

  if (iter_object == data->object)
    {
      data->iter = *iter;
      data->found = TRUE;
      goto out;
    }

  if (g_strcmp0 (g_dbus_object_get_object_path (G_DBUS_OBJECT (iter_object)), data->object_path) == 0)
    {
      data->iter = *iter;
      data->found = TRUE;
      goto out;
    }


 out:
  if (iter_object != NULL)
    g_object_unref (iter_object);
  return data->found;
}

static gboolean
find_iter_for_object (GduDeviceTreeModel *model,
                      UDisksObject       *object,
                      GtkTreeIter        *out_iter)
{
  FindIterData data;

  memset (&data, 0, sizeof (data));
  data.object = object;
  data.found = FALSE;
  gtk_tree_model_foreach (GTK_TREE_MODEL (model),
                          find_iter_for_object_cb,
                          &data);
  if (data.found)
    {
      if (out_iter != NULL)
        *out_iter = data.iter;
    }

  return data.found;
}

static gboolean
find_iter_for_object_path (GduDeviceTreeModel *model,
                           const gchar        *object_path,
                           GtkTreeIter        *out_iter)
{
  FindIterData data;

  memset (&data, 0, sizeof (data));
  data.object_path = object_path;
  data.found = FALSE;
  gtk_tree_model_foreach (GTK_TREE_MODEL (model),
                          find_iter_for_object_cb,
                          &data);
  if (data.found)
    {
      if (out_iter != NULL)
        *out_iter = data.iter;
    }

  return data.found;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
diff_sorted_lists (GList         *list1,
                   GList         *list2,
                   GCompareFunc   compare,
                   GList        **added,
                   GList        **removed)
{
  gint order;

  *added = *removed = NULL;

  while (list1 != NULL &&
         list2 != NULL)
    {
      order = (*compare) (list1->data, list2->data);
      if (order < 0)
        {
          *removed = g_list_prepend (*removed, list1->data);
          list1 = list1->next;
        }
      else if (order > 0)
        {
          *added = g_list_prepend (*added, list2->data);
          list2 = list2->next;
        }
      else
        { /* same item */
          list1 = list1->next;
          list2 = list2->next;
        }
    }

  while (list1 != NULL)
    {
      *removed = g_list_prepend (*removed, list1->data);
      list1 = list1->next;
    }
  while (list2 != NULL)
    {
      *added = g_list_prepend (*added, list2->data);
      list2 = list2->next;
    }
}

/* ---------------------------------------------------------------------------------------------------- */

static gint
_g_dbus_object_compare (GDBusObject *a,
                        GDBusObject *b)
{
  return g_strcmp0 (g_dbus_object_get_object_path (a),
                    g_dbus_object_get_object_path (b));
}

/* ---------------------------------------------------------------------------------------------------- */

static void
gdu_device_tree_model_constructed (GObject *object)
{
  GduDeviceTreeModel *model = GDU_DEVICE_TREE_MODEL (object);
  GType types[GDU_DEVICE_TREE_MODEL_N_COLUMNS];
  GDBusObjectManager *object_manager;

  types[0] = G_TYPE_STRING;
  types[1] = G_TYPE_BOOLEAN;
  types[2] = G_TYPE_STRING;
  types[3] = G_TYPE_ICON;
  types[4] = G_TYPE_STRING;
  types[5] = G_TYPE_DBUS_OBJECT;
  G_STATIC_ASSERT (6 == GDU_DEVICE_TREE_MODEL_N_COLUMNS);
  gtk_tree_store_set_column_types (GTK_TREE_STORE (model),
                                   GDU_DEVICE_TREE_MODEL_N_COLUMNS,
                                   types);

  g_assert (gtk_tree_model_get_flags (GTK_TREE_MODEL (model)) & GTK_TREE_MODEL_ITERS_PERSIST);

  object_manager = udisks_client_get_object_manager (model->client);
  g_signal_connect (object_manager,
                    "object-added",
                    G_CALLBACK (on_object_added),
                    model);
  g_signal_connect (object_manager,
                    "object-removed",
                    G_CALLBACK (on_object_removed),
                    model);
  g_signal_connect (object_manager,
                    "interface-added",
                    G_CALLBACK (on_interface_added),
                    model);
  g_signal_connect (object_manager,
                    "interface-removed",
                    G_CALLBACK (on_interface_removed),
                    model);
  g_signal_connect (object_manager,
                    "interface-proxy-properties-changed",
                    G_CALLBACK (on_interface_proxy_properties_changed),
                    model);
  coldplug (model);

  if (G_OBJECT_CLASS (gdu_device_tree_model_parent_class)->constructed != NULL)
    G_OBJECT_CLASS (gdu_device_tree_model_parent_class)->constructed (object);
}

static void
gdu_device_tree_model_class_init (GduDeviceTreeModelClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = gdu_device_tree_model_finalize;
  gobject_class->constructed  = gdu_device_tree_model_constructed;
  gobject_class->get_property = gdu_device_tree_model_get_property;
  gobject_class->set_property = gdu_device_tree_model_set_property;

  /**
   * GduDeviceTreeModel:client:
   *
   * The #UDisksClient used by the #GduDeviceTreeModel instance.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_CLIENT,
                                   g_param_spec_object ("client",
                                                        "Client",
                                                        "The client used by the tree model",
                                                        UDISKS_TYPE_CLIENT,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}

/**
 * gdu_device_tree_model_new:
 * @client: A #UDisksClient.
 *
 * Creates a new #GduDeviceTreeModel for viewing the devices belonging to
 * @client.
 *
 * Returns: A #GduDeviceTreeModel. Free with g_object_unref().
 */
GduDeviceTreeModel *
gdu_device_tree_model_new (UDisksClient *client)
{
  return GDU_DEVICE_TREE_MODEL (g_object_new (GDU_TYPE_DEVICE_TREE_MODEL,
                                       "client", client,
                                       NULL));
}

/**
 * gdu_device_tree_model_get_client:
 * @model: A #GduDeviceTreeModel.
 *
 * Gets the #UDisksClient used by @model.
 *
 * Returns: (transfer none): A #UDisksClient. Do not free, the object
 * belongs to @model.
 */
UDisksClient *
gdu_device_tree_model_get_client (GduDeviceTreeModel *model)
{
  g_return_val_if_fail (GDU_IS_DEVICE_TREE_MODEL (model), NULL);
  return model->client;
}

/* ---------------------------------------------------------------------------------------------------- */

static GtkTreeIter *
get_lun_header_iter (GduDeviceTreeModel *model)
{
  gchar *s;

  if (model->lun_iter_valid)
    goto out;

  s = g_strdup_printf ("<small><span foreground=\"#555555\">%s</span></small>",
                       _("Direct-Attached Storage"));
  gtk_tree_store_insert_with_values (GTK_TREE_STORE (model),
                                     &model->lun_iter,
                                     NULL, /* GtkTreeIter *parent */
                                     0,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_IS_HEADING, TRUE,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_HEADING_TEXT, s,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_SORT_KEY, "00_lun",
                                     -1);
  g_free (s);

  model->lun_iter_valid = TRUE;

 out:
  return &model->lun_iter;
}

static void
nuke_lun_header (GduDeviceTreeModel *model)
{
  if (model->lun_iter_valid)
    {
      gtk_tree_store_remove (GTK_TREE_STORE (model), &model->lun_iter);
      model->lun_iter_valid = FALSE;
    }
}

static void
add_lun (GduDeviceTreeModel *model,
         UDisksObject       *object,
         GtkTreeIter        *parent)
{
  UDisksLun *lun;
  GIcon *drive_icon;
  GIcon *media_icon;
  gchar *name;
  gchar *description;
  gchar *media_description;
  gchar *s;
  gchar *sort_key;
  GtkTreeIter iter;

  lun = udisks_object_peek_lun (object);
  udisks_util_get_lun_info (lun, &name, &description, &drive_icon, &media_description, &media_icon);
  s = g_strdup_printf ("%s\n"
                       "<small><span foreground=\"#555555\">%s</span></small>",
                       description,
                       name);

  //g_debug ("lun %s ->\n"
  //         " drive_icon=%s\n"
  //         " media_icon=%s\n"
  //         "\n",
  //         g_dbus_object_get_object_path (object),
  //         g_icon_to_string (drive_icon),
  //         g_icon_to_string (media_icon));

  sort_key = g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object))); /* for now */
  gtk_tree_store_insert_with_values (GTK_TREE_STORE (model),
                                     &iter,
                                     parent,
                                     0,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_ICON, drive_icon,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_NAME, s,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_SORT_KEY, sort_key,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_OBJECT, object,
                                     -1);
  if (media_icon != NULL)
    g_object_unref (media_icon);
  g_object_unref (drive_icon);
  g_free (sort_key);
  g_free (s);
  g_free (media_description);
  g_free (description);
  g_free (name);
}

static void
remove_lun (GduDeviceTreeModel *model,
            UDisksObject       *object)
{
  GtkTreeIter iter;

  if (!find_iter_for_object (model,
                             object,
                             &iter))
    {
      g_warning ("Error finding iter for object at %s",
                 g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
      goto out;
    }

  gtk_tree_store_remove (GTK_TREE_STORE (model), &iter);

 out:
  ;
}

static gboolean
should_include_lun (UDisksObject *object,
                    gboolean      allow_iscsi)
{
  UDisksLun *lun;
  gboolean ret;

  ret = FALSE;

  lun = udisks_object_peek_lun (object);

  /* unless specificlly allowed, don't show LUNs paired with an iSCSI target */
  if (!allow_iscsi && g_strcmp0 (udisks_lun_get_iscsi_target (lun), "/") != 0)
    goto out;

  ret = TRUE;

 out:
  return ret;
}

static void
update_luns (GduDeviceTreeModel *model)
{
  GDBusObjectManager *object_manager;
  GList *objects;
  GList *luns;
  GList *added_luns;
  GList *removed_luns;
  GList *l;

  object_manager = udisks_client_get_object_manager (model->client);
  objects = g_dbus_object_manager_get_objects (object_manager);

  luns = NULL;
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksLun *lun;

      lun = udisks_object_peek_lun (object);
      if (lun == NULL)
        continue;

      if (should_include_lun (object, FALSE))
        luns = g_list_prepend (luns, g_object_ref (object));
    }

  luns = g_list_sort (luns, (GCompareFunc) _g_dbus_object_compare);
  model->current_luns = g_list_sort (model->current_luns, (GCompareFunc) _g_dbus_object_compare);
  diff_sorted_lists (model->current_luns,
                     luns,
                     (GCompareFunc) _g_dbus_object_compare,
                     &added_luns,
                     &removed_luns);

  for (l = removed_luns; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      g_assert (g_list_find (model->current_luns, object) != NULL);
      model->current_luns = g_list_remove (model->current_luns, object);
      remove_lun (model, object);
      g_object_unref (object);
    }
  for (l = added_luns; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      model->current_luns = g_list_prepend (model->current_luns, g_object_ref (object));
      add_lun (model, object, get_lun_header_iter (model));
    }

  if (g_list_length (model->current_luns) == 0)
    nuke_lun_header (model);

  g_list_free (added_luns);
  g_list_free (removed_luns);
  g_list_foreach (luns, (GFunc) g_object_unref, NULL);
  g_list_free (luns);

  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
}

/* ---------------------------------------------------------------------------------------------------- */

static GtkTreeIter *
get_block_header_iter (GduDeviceTreeModel *model)
{
  gchar *s;

  if (model->block_iter_valid)
    goto out;

  s = g_strdup_printf ("<small><span foreground=\"#555555\">%s</span></small>",
                       _("Other Devices"));
  gtk_tree_store_insert_with_values (GTK_TREE_STORE (model),
                                     &model->block_iter,
                                     NULL, /* GtkTreeIter *parent */
                                     0,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_IS_HEADING, TRUE,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_HEADING_TEXT, s,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_SORT_KEY, "01_block",
                                     -1);
  g_free (s);

  model->block_iter_valid = TRUE;

 out:
  return &model->block_iter;
}

static void
nuke_block_header (GduDeviceTreeModel *model)
{
  if (model->block_iter_valid)
    {
      gtk_tree_store_remove (GTK_TREE_STORE (model), &model->block_iter);
      model->block_iter_valid = FALSE;
    }
}

static void
add_block (GduDeviceTreeModel  *model,
           UDisksObject        *object,
           GtkTreeIter         *parent)
{
  UDisksBlockDevice *block;
  GIcon *icon;
  gchar *s;
  gchar *sort_key;
  GtkTreeIter iter;
  const gchar *preferred_device;
  const gchar *loop_backing_file;
  guint64 size;
  gchar *size_str;

  block = udisks_object_peek_block_device (object);
  size = udisks_block_device_get_size (block);
  size_str = udisks_util_get_size_for_display (size, FALSE, FALSE);

  preferred_device = udisks_block_device_get_preferred_device (block);
  loop_backing_file = udisks_block_device_get_loop_backing_file (block);
  if (strlen (loop_backing_file) > 0)
    {
      gchar *loop_name;

      /* Translators: This is for a /dev/loop device - %s is the size of the device e.g. "230 MB". */
      loop_name = g_strdup_printf (_("%s Loop Device"), size_str);

      /* loop devices */
      icon = g_themed_icon_new ("drive-removable-media"); /* for now */
      s = g_strdup_printf ("%s\n"
                           "<small><span foreground=\"#555555\">%s</span></small>",
                           loop_name,
                           loop_backing_file);
      g_free (loop_name);
    }
  else
    {
      gchar *block_name;

      /* Translators: This is for a block device which we failed to categorize  - %s is
       * the size of the device e.g. "230 MB".
       */
      block_name = g_strdup_printf (_("%s Block Device"),
                                    size_str);

      /* fallback: preferred device and drive-harddisk icon */
      icon = g_themed_icon_new ("drive-removable-media"); /* for now */

      s = g_strdup_printf ("%s\n"
                           "<small><span foreground=\"#555555\">%s</span></small>",
                           block_name,
                           preferred_device);
    }

  sort_key = g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object))); /* for now */
  gtk_tree_store_insert_with_values (GTK_TREE_STORE (model),
                                     &iter,
                                     parent,
                                     0,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_ICON, icon,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_NAME, s,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_SORT_KEY, sort_key,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_OBJECT, object,
                                     -1);
  g_object_unref (icon);
  g_free (sort_key);
  g_free (s);
  g_free (size_str);
}

static void
remove_block (GduDeviceTreeModel  *model,
              UDisksObject        *object)
{
  GtkTreeIter iter;

  if (!find_iter_for_object (model,
                             object,
                             &iter))
    {
      g_warning ("Error finding iter for object at %s",
                 g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
      goto out;
    }

  gtk_tree_store_remove (GTK_TREE_STORE (model), &iter);

 out:
  ;
}

static gboolean
should_include_block (UDisksObject *object)
{
  UDisksBlockDevice *block;
  gboolean ret;
  const gchar *device;
  const gchar *lun;
  const gchar *crypto_backing_device;
  guint64 size;

  ret = FALSE;

  block = udisks_object_peek_block_device (object);

  /* RAM devices are useless */
  device = udisks_block_device_get_device (block);
  if (g_str_has_prefix (device, "/dev/ram"))
    goto out;

  /* Don't show devices of size zero - otherwise we'd end up showing unused loop devices */
  size = udisks_block_device_get_size (block);
  if (size == 0)
    goto out;

  /* Only include devices if they are top-level */
  if (udisks_block_device_get_part_entry (block))
    goto out;

  /* Don't include if already shown in "Direct-Attached devices" */
  lun = udisks_block_device_get_lun (block);
  if (g_strcmp0 (lun, "/") != 0)
    goto out;

  /* Don't include if already shown in volume grid as an unlocked device */
  crypto_backing_device = udisks_block_device_get_crypto_backing_device (block);
  if (g_strcmp0 (crypto_backing_device, "/") != 0)
    goto out;

  ret = TRUE;

 out:
  return ret;
}

static void
update_blocks (GduDeviceTreeModel *model)
{
  GDBusObjectManager *object_manager;
  GList *objects;
  GList *blocks;
  GList *added_blocks;
  GList *removed_blocks;
  GList *l;

  object_manager = udisks_client_get_object_manager (model->client);
  objects = g_dbus_object_manager_get_objects (object_manager);

  blocks = NULL;
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlockDevice *block;

      block = udisks_object_peek_block_device (object);
      if (block == NULL)
        continue;

      if (should_include_block (object))
        blocks = g_list_prepend (blocks, g_object_ref (object));
    }

  blocks = g_list_sort (blocks, (GCompareFunc) _g_dbus_object_compare);
  model->current_blocks = g_list_sort (model->current_blocks, (GCompareFunc) _g_dbus_object_compare);
  diff_sorted_lists (model->current_blocks,
                     blocks,
                     (GCompareFunc) _g_dbus_object_compare,
                     &added_blocks,
                     &removed_blocks);

  for (l = removed_blocks; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);

      g_assert (g_list_find (model->current_blocks, object) != NULL);

      model->current_blocks = g_list_remove (model->current_blocks, object);
      remove_block (model, object);
      g_object_unref (object);
    }
  for (l = added_blocks; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      model->current_blocks = g_list_prepend (model->current_blocks, g_object_ref (object));
      add_block (model, object, get_block_header_iter (model));
    }

  if (g_list_length (model->current_blocks) == 0)
    nuke_block_header (model);

  g_list_free (added_blocks);
  g_list_free (removed_blocks);
  g_list_foreach (blocks, (GFunc) g_object_unref, NULL);
  g_list_free (blocks);

  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
}

/* ---------------------------------------------------------------------------------------------------- */

static GtkTreeIter *
get_iscsi_iter (GduDeviceTreeModel *model)
{
  gchar *s;

  if (model->iscsi_iter_valid)
    goto out;

  s = g_strdup_printf ("<small><span foreground=\"#555555\">%s</span></small>",
                       _("iSCSI"));
  gtk_tree_store_insert_with_values (GTK_TREE_STORE (model),
                                     &model->iscsi_iter,
                                     NULL, /* GtkTreeIter *parent */
                                     0,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_IS_HEADING, TRUE,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_HEADING_TEXT, s,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_SORT_KEY, "02_iscsi",
                                     -1);
  g_free (s);

  model->iscsi_iter_valid = TRUE;

 out:
  return &model->iscsi_iter;
}

static void
nuke_iscsi_iter (GduDeviceTreeModel *model)
{
  if (model->iscsi_iter_valid)
    {
      gtk_tree_store_remove (GTK_TREE_STORE (model), &model->iscsi_iter);
      model->iscsi_iter_valid = FALSE;
    }
}

static void
add_iscsi_collection (GduDeviceTreeModel  *model,
                      UDisksObject        *object,
                      GtkTreeIter         *parent)
{
  UDisksIScsiCollection *collection;
  const gchar *mechanism;
  GIcon *icon;
  gchar *s;
  gchar *sort_key;
  GtkTreeIter iter;

  collection = udisks_object_peek_iscsi_collection (object);
  mechanism = udisks_iscsi_collection_get_mechanism (collection);

  icon = g_themed_icon_new_with_default_fallbacks ("network_local");
  if (g_strcmp0 (mechanism, "sendtargets") == 0)
    {
      s = g_strdup_printf ("%s\n"
                           "<small><span foreground=\"#555555\">%s</span></small>",
                           _("SendTargets Discovery"),
                           udisks_iscsi_collection_get_discovery_address (collection));
    }
  else if (g_strcmp0 (mechanism, "isns") == 0)
    {
      s = g_strdup_printf ("%s\n"
                           "<small><span foreground=\"#555555\">%s</span></small>",
                           _("iSNS Discovery"),
                           udisks_iscsi_collection_get_discovery_address (collection));
    }
  else if (g_strcmp0 (mechanism, "static") == 0)
    {
      s = g_strdup (_("Statically Configured"));
    }
  else if (g_strcmp0 (mechanism, "firmware") == 0)
    {
      s = g_strdup (_("Configured by Firmware"));
    }
  else
    {
      /* This is a fallback */
      s = g_strdup_printf ("%s\n"
                           "<small><span foreground=\"#555555\">%s</span></small>",
                           mechanism,
                           udisks_iscsi_collection_get_discovery_address (collection));
    }

  sort_key = g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object))); /* for now */
  gtk_tree_store_insert_with_values (GTK_TREE_STORE (model),
                                     &iter,
                                     parent,
                                     0,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_ICON, icon,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_NAME, s,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_SORT_KEY, sort_key,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_OBJECT, object,
                                     -1);
  g_object_unref (icon);
  g_free (sort_key);
  g_free (s);
}

static void
add_iscsi_target (GduDeviceTreeModel  *model,
                  UDisksObject        *object,
                  GtkTreeIter         *parent)
{
  UDisksIScsiTarget *target;
  GIcon *icon;
  gchar *s;
  gchar *sort_key;
  GtkTreeIter iter;

  target = udisks_object_peek_iscsi_target (object);

#if 0
  GIcon *base_icon;
  GIcon *emblem_icon;
  GEmblem *emblem;
  emblem_icon = g_themed_icon_new_with_default_fallbacks ("emblem-web");
  emblem = g_emblem_new (emblem_icon);
  base_icon = g_themed_icon_new_with_default_fallbacks ("drive-harddisk");
  icon = g_emblemed_icon_new (base_icon, emblem);
  g_object_unref (emblem);
  g_object_unref (base_icon);
  g_object_unref (emblem_icon);
#endif
  icon = g_themed_icon_new_with_default_fallbacks ("network-server");

  s = g_strdup_printf ("%s\n"
                       "<small><span foreground=\"#555555\">%s</span></small>",
                       "Remote iSCSI Target", /* TODO: alias */
                       udisks_iscsi_target_get_name (target));

  sort_key = g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object))); /* for now */
  gtk_tree_store_insert_with_values (GTK_TREE_STORE (model),
                                     &iter,
                                     parent,
                                     0,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_ICON, icon,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_NAME, s,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_SORT_KEY, sort_key,
                                     GDU_DEVICE_TREE_MODEL_COLUMN_OBJECT, object,
                                     -1);
  g_object_unref (icon);
  g_free (sort_key);
  g_free (s);
}

static void
remove_iscsi_target (GduDeviceTreeModel  *model,
                     UDisksObject        *object)
{
  GtkTreeIter iter;

  if (!find_iter_for_object (model,
                             object,
                             &iter))
    {
      g_warning ("Error finding iter for object at %s",
                 g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
      goto out;
    }

  gtk_tree_store_remove (GTK_TREE_STORE (model), &iter);

 out:
  ;
}

static gint
count_targets_for_collection (GDBusObjectManager    *object_manager,
                              UDisksIScsiCollection *collection)
{
  gint ret;
  GList *l;
  GList *objects;
  GDBusObject *collection_object;
  const gchar *collection_object_path;

  collection_object = g_dbus_interface_get_object (G_DBUS_INTERFACE (collection));
  collection_object_path = g_dbus_object_get_object_path (collection_object);

  ret = 0;
  objects = g_dbus_object_manager_get_objects (object_manager);
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksIScsiTarget *target;
      target = udisks_object_peek_iscsi_target (object);
      if (target == NULL)
        continue;
      if (g_strcmp0 (udisks_iscsi_target_get_collection (target), collection_object_path) == 0)
        ret++;
    }
  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);

  return ret;
}

static void
update_iscsi_targets (GduDeviceTreeModel *model)
{
  GDBusObjectManager *object_manager;
  GList *objects;
  GList *iscsi_objects;
  GList *added;
  GList *removed;
  GList *l;

  object_manager = udisks_client_get_object_manager (model->client);
  objects = g_dbus_object_manager_get_objects (object_manager);

  iscsi_objects = NULL;
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksIScsiTarget *target;
      UDisksIScsiCollection *collection;

      collection = udisks_object_peek_iscsi_collection (object);
      if (collection != NULL)
        {
          /* Don't include an object for static or firmware unless they are non-empty */
          if (g_strcmp0 (udisks_iscsi_collection_get_mechanism (collection), "static") == 0 ||
              g_strcmp0 (udisks_iscsi_collection_get_mechanism (collection), "firmware") == 0)
            {
              if (count_targets_for_collection (object_manager, collection) == 0)
                continue;
            }
          iscsi_objects = g_list_prepend (iscsi_objects, g_object_ref (object));
        }

      target = udisks_object_peek_iscsi_target (object);
      if (target != NULL)
        {
          GList *ll;
          const gchar *target_object_path;

          iscsi_objects = g_list_prepend (iscsi_objects, g_object_ref (object));

          /* also include the LUNs that are associated with this target */
          target_object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));
          for (ll = objects; ll != NULL; ll = ll->next)
            {
              UDisksObject *lun_object = UDISKS_OBJECT (ll->data);
              UDisksLun *lun;
              lun = udisks_object_peek_lun (lun_object);
              if (lun != NULL)
                {
                  if (g_strcmp0 (udisks_lun_get_iscsi_target (lun), target_object_path) == 0)
                    {
                      if (should_include_lun (lun_object, TRUE))
                        iscsi_objects = g_list_prepend (iscsi_objects, g_object_ref (lun_object));
                    }
                }
            }
        }
    }

  iscsi_objects = g_list_sort (iscsi_objects, (GCompareFunc) _g_dbus_object_compare);
  model->current_iscsi_objects = g_list_sort (model->current_iscsi_objects, (GCompareFunc) _g_dbus_object_compare);
  diff_sorted_lists (model->current_iscsi_objects,
                     iscsi_objects,
                     (GCompareFunc) _g_dbus_object_compare,
                     &added,
                     &removed);

  for (l = removed; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);

      g_assert (g_list_find (model->current_iscsi_objects, object) != NULL);

      model->current_iscsi_objects = g_list_remove (model->current_iscsi_objects, object);
      remove_iscsi_target (model, object);
      g_object_unref (object);
    }

  /* Three passes: first add the iSCSI collections ... */
  for (l = added; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      if (udisks_object_peek_iscsi_collection (object) != NULL)
        {
          model->current_iscsi_objects = g_list_prepend (model->current_iscsi_objects,
                                                         g_object_ref (object));
          add_iscsi_collection (model, object, get_iscsi_iter (model));
        }
    }
  /*  ... then the targets ... */
  for (l = added; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      if (udisks_object_peek_iscsi_target (object) != NULL)
        {
          GtkTreeIter iter;
          GtkTreeIter *piter;
          model->current_iscsi_objects = g_list_prepend (model->current_iscsi_objects,
                                                         g_object_ref (object));
          piter = &iter;
          if (!find_iter_for_object_path (model,
                                          udisks_iscsi_target_get_collection (udisks_object_peek_iscsi_target (object)),
                                          &iter))
            piter = get_iscsi_iter (model);
          add_iscsi_target (model, object, piter);
        }
    }
  /* ... and finally the LUNs */
  for (l = added; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      if (udisks_object_peek_lun (object) != NULL)
        {
          GtkTreeIter iter;
          model->current_iscsi_objects = g_list_prepend (model->current_iscsi_objects,
                                                         g_object_ref (object));
          g_warn_if_fail (find_iter_for_object_path (model,
                                                     udisks_lun_get_iscsi_target (udisks_object_peek_lun (object)),
                                                     &iter));
          add_lun (model, object, &iter);
        }
    }

  if (g_list_length (model->current_iscsi_objects) == 0)
    nuke_iscsi_iter (model);

  g_list_free (added);
  g_list_free (removed);
  g_list_foreach (iscsi_objects, (GFunc) g_object_unref, NULL);
  g_list_free (iscsi_objects);

  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_all (GduDeviceTreeModel *model)
{
  /* TODO: if this is CPU intensive we could coalesce all updates / schedule timeouts */
  update_luns (model);
  update_blocks (model);
  update_iscsi_targets (model);
}

static void
coldplug (GduDeviceTreeModel *model)
{
  update_all (model);
}

static void
on_object_added (GDBusObjectManager  *manager,
                 GDBusObject         *object,
                 gpointer             user_data)
{
  GduDeviceTreeModel *model = GDU_DEVICE_TREE_MODEL (user_data);
  update_all (model);
}

static void
on_object_removed (GDBusObjectManager  *manager,
                   GDBusObject         *object,
                   gpointer             user_data)
{
  GduDeviceTreeModel *model = GDU_DEVICE_TREE_MODEL (user_data);
  update_all (model);
}

static void
on_interface_added (GDBusObjectManager  *manager,
                    GDBusObject         *object,
                    GDBusInterface      *interface,
                    gpointer             user_data)
{
  GduDeviceTreeModel *model = GDU_DEVICE_TREE_MODEL (user_data);
  update_all (model);
}

static void
on_interface_removed (GDBusObjectManager  *manager,
                      GDBusObject         *object,
                      GDBusInterface      *interface,
                      gpointer             user_data)
{
  GduDeviceTreeModel *model = GDU_DEVICE_TREE_MODEL (user_data);
  update_all (model);
}

static void
on_interface_proxy_properties_changed (GDBusObjectManagerClient   *manager,
                                       GDBusObjectProxy           *object_proxy,
                                       GDBusProxy                 *interface_proxy,
                                       GVariant                   *changed_properties,
                                       const gchar *const         *invalidated_properties,
                                       gpointer                    user_data)
{
  GduDeviceTreeModel *model = GDU_DEVICE_TREE_MODEL (user_data);
  update_all (model);
}

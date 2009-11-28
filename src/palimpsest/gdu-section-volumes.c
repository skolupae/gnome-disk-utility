/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/* gdu-section-volumes.c
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

#include <config.h>
#include <string.h>
#include <glib/gi18n.h>
#include <dbus/dbus-glib.h>
#include <stdlib.h>
#include <math.h>

#include <gdu-gtk/gdu-gtk.h>
#include "gdu-section-volumes.h"

struct _GduSectionVolumesPrivate
{
        GduPresentable *cur_volume;

        GtkWidget *grid;
        GtkWidget *details_table;
        GtkWidget *button_table;

        /* shared between all volume types */
        GduDetailsElement *usage_element;
        GduDetailsElement *capacity_element;
        GduDetailsElement *partition_type_element;
        GduDetailsElement *partition_flags_element;
        GduDetailsElement *partition_label_element;
        GduDetailsElement *device_element;

        /* elements for the 'filesystem' usage */
        GduDetailsElement *fs_type_element;
        GduDetailsElement *fs_available_element;
        GduDetailsElement *fs_label_element;
        GduDetailsElement *fs_mount_point_element;

        GduButtonElement *fs_mount_button;
        GduButtonElement *fs_unmount_button;
        GduButtonElement *fs_check_button;
        GduButtonElement *fs_change_label_button;
        GduButtonElement *format_button;
        GduButtonElement *partition_edit_button;
        GduButtonElement *partition_delete_button;
        GduButtonElement *partition_create_button;
        GduButtonElement *luks_lock_button;
        GduButtonElement *luks_unlock_button;
        GduButtonElement *luks_forget_passphrase_button;
        GduButtonElement *luks_change_passphrase_button;
};

G_DEFINE_TYPE (GduSectionVolumes, gdu_section_volumes, GDU_TYPE_SECTION)

/* ---------------------------------------------------------------------------------------------------- */

static void
gdu_section_volumes_finalize (GObject *object)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (object);

        if (section->priv->cur_volume != NULL)
                g_object_unref (section->priv->cur_volume);

        if (G_OBJECT_CLASS (gdu_section_volumes_parent_class)->finalize != NULL)
                G_OBJECT_CLASS (gdu_section_volumes_parent_class)->finalize (object);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
unmount_op_callback (GduDevice *device,
                     GError    *error,
                     gpointer   user_data)
{
        GduShell *shell = GDU_SHELL (user_data);

        /* TODO: handle busy mounts using GtkMountOperation */

        if (error != NULL) {
                GtkWidget *dialog;
                dialog = gdu_error_dialog_new_for_volume (GTK_WINDOW (gdu_shell_get_toplevel (shell)),
                                                          device,
                                                          _("Error unmounting volume"),
                                                          error);
                gtk_widget_show_all (dialog);
                gtk_window_present (GTK_WINDOW (dialog));
                gtk_dialog_run (GTK_DIALOG (dialog));
                gtk_widget_destroy (dialog);
                g_error_free (error);
        }
        g_object_unref (shell);
}

static void
on_unmount_button_clicked (GduButtonElement *button_element,
                           gpointer          user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);

        GduPresentable *v;
        GduDevice *d;

        v = NULL;
        d = NULL;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));
        if (v == NULL)
                goto out;

        d = gdu_presentable_get_device (v);
        if (d == NULL)
                goto out;

        gdu_device_op_filesystem_unmount (d,
                                          unmount_op_callback,
                                          g_object_ref (gdu_section_get_shell (GDU_SECTION (section))));

 out:
        if (d != NULL)
                g_object_unref (d);
        if (v != NULL)
                g_object_unref (v);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
mount_op_callback (GduDevice *device,
                   gchar     *mount_point,
                   GError    *error,
                   gpointer   user_data)
{
        GduShell *shell = GDU_SHELL (user_data);

        if (error != NULL) {
                GtkWidget *dialog;
                dialog = gdu_error_dialog_new_for_volume (GTK_WINDOW (gdu_shell_get_toplevel (shell)),
                                                          device,
                                                          _("Error mounting volume"),
                                                          error);
                gtk_widget_show_all (dialog);
                gtk_window_present (GTK_WINDOW (dialog));
                gtk_dialog_run (GTK_DIALOG (dialog));
                gtk_widget_destroy (dialog);
                g_error_free (error);
        } else {
                g_free (mount_point);
        }
        g_object_unref (shell);
}

static void
on_mount_button_clicked (GduButtonElement *button_element,
                         gpointer          user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);
        GduPresentable *v;
        GduDevice *d;

        v = NULL;
        d = NULL;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));
        if (v == NULL)
                goto out;

        d = gdu_presentable_get_device (v);
        if (d == NULL)
                goto out;

        gdu_device_op_filesystem_mount (d,
                                        NULL,
                                        mount_op_callback,
                                        g_object_ref (gdu_section_get_shell (GDU_SECTION (section))));

 out:
        if (d != NULL)
                g_object_unref (d);
        if (v != NULL)
                g_object_unref (v);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
partition_delete_op_callback (GduDevice *device,
                              GError    *error,
                              gpointer   user_data)
{
        GduShell *shell = GDU_SHELL (user_data);

        if (error != NULL) {
                GtkWidget *dialog;
                dialog = gdu_error_dialog_new_for_volume (GTK_WINDOW (gdu_shell_get_toplevel (shell)),
                                                          device,
                                                          _("Error deleting partition"),
                                                          error);
                gtk_widget_show_all (dialog);
                gtk_window_present (GTK_WINDOW (dialog));
                gtk_dialog_run (GTK_DIALOG (dialog));
                gtk_widget_destroy (dialog);
                g_error_free (error);
        }
        g_object_unref (shell);
}

static void
on_partition_delete_button_clicked (GduButtonElement *button_element,
                                    gpointer          user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);
        GduPresentable *v;
        GduDevice *d;
        GtkWindow *toplevel;
        GtkWidget *dialog;
        gint response;

        v = NULL;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));
        if (v == NULL)
                goto out;

        d = gdu_presentable_get_device (v);
        if (d == NULL)
                goto out;

        toplevel = GTK_WINDOW (gdu_shell_get_toplevel (gdu_section_get_shell (GDU_SECTION (section))));
        dialog = gdu_confirmation_dialog_new (toplevel,
                                              v,
                                              _("Are you sure you want to delete the partition?"),
                                              _("_Delete"));
        gtk_widget_show_all (dialog);
        response = gtk_dialog_run (GTK_DIALOG (dialog));
        if (response == GTK_RESPONSE_OK) {
                gdu_device_op_partition_delete (d,
                                                partition_delete_op_callback,
                                                g_object_ref (gdu_section_get_shell (GDU_SECTION (section))));
        }
        gtk_widget_destroy (dialog);

 out:
        if (d != NULL)
                g_object_unref (d);
        if (v != NULL)
                g_object_unref (v);
}

/* ---------------------------------------------------------------------------------------------------- */


typedef struct {
        GduShell *shell;
        GduPresentable *presentable;
        char *encrypt_passphrase;
        gboolean save_in_keyring;
        gboolean save_in_keyring_session;
} CreateFilesystemData;

static void
create_filesystem_data_free (CreateFilesystemData *data)
{
        if (data->shell != NULL)
                g_object_unref (data->shell);
        if (data->presentable != NULL)
                g_object_unref (data->presentable);
        if (data->encrypt_passphrase != NULL) {
                memset (data->encrypt_passphrase, '\0', strlen (data->encrypt_passphrase));
                g_free (data->encrypt_passphrase);
        }
        g_free (data);
}

static void
filesystem_create_op_callback (GduDevice  *device,
                               GError     *error,
                               gpointer    user_data)
{
        CreateFilesystemData *data = user_data;

        if (error != NULL) {
                GtkWidget *dialog;
                dialog = gdu_error_dialog_new_for_volume (GTK_WINDOW (gdu_shell_get_toplevel (data->shell)),
                                                          device,
                                                          _("Error creating filesystem"),
                                                          error);
                gtk_widget_show_all (dialog);
                gtk_window_present (GTK_WINDOW (dialog));
                gtk_dialog_run (GTK_DIALOG (dialog));
                gtk_widget_destroy (dialog);
                g_error_free (error);
        } else if (data->encrypt_passphrase != NULL) {
                /* now set the passphrase if requested */
                if (data->save_in_keyring || data->save_in_keyring_session) {
                        gdu_util_save_secret (device,
                                              data->encrypt_passphrase,
                                              data->save_in_keyring_session);
                }
        }
        if (data != NULL)
                create_filesystem_data_free (data);
}

static void
on_format_button_clicked (GduButtonElement *button_element,
                          gpointer          user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);
        GduPresentable *v;
        GduDevice *d;
        GtkWindow *toplevel;
        GtkWidget *dialog;
        GtkWidget *confirmation_dialog;
        gint response;

        v = NULL;
        dialog = NULL;
        confirmation_dialog = NULL;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));
        if (v == NULL)
                goto out;

        d = gdu_presentable_get_device (v);
        if (d == NULL)
                goto out;

        toplevel = GTK_WINDOW (gdu_shell_get_toplevel (gdu_section_get_shell (GDU_SECTION (section))));
        dialog = gdu_format_dialog_new (toplevel,
                                        v,
                                        GDU_FORMAT_DIALOG_FLAGS_NONE);
        gtk_widget_show_all (dialog);
        response = gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_hide (dialog);
        if (response == GTK_RESPONSE_OK) {
                confirmation_dialog = gdu_confirmation_dialog_new (toplevel,
                                                                   v,
                                                                   _("Are you sure you want to format the volume?"),
                                                                   _("_Format"));
                gtk_widget_show_all (confirmation_dialog);
                response = gtk_dialog_run (GTK_DIALOG (confirmation_dialog));
                gtk_widget_hide (confirmation_dialog);
                if (response == GTK_RESPONSE_OK) {
                        CreateFilesystemData *data;

                        data = g_new0 (CreateFilesystemData, 1);
                        data->shell = g_object_ref (gdu_section_get_shell (GDU_SECTION (section)));
                        data->presentable = g_object_ref (v);

                        if (gdu_format_dialog_get_encrypt (GDU_FORMAT_DIALOG (dialog))) {
                                data->encrypt_passphrase = gdu_util_dialog_ask_for_new_secret (
                                      gdu_shell_get_toplevel (gdu_section_get_shell (GDU_SECTION (section))),
                                      &data->save_in_keyring,
                                      &data->save_in_keyring_session);
                                if (data->encrypt_passphrase == NULL) {
                                        create_filesystem_data_free (data);
                                        goto out;
                                }
                        }

                        gdu_device_op_filesystem_create (d,
                                                         gdu_format_dialog_get_fs_type (GDU_FORMAT_DIALOG (dialog)),
                                                         gdu_format_dialog_get_fs_label (GDU_FORMAT_DIALOG (dialog)),
                                                         data->encrypt_passphrase,
                                                         gdu_format_dialog_get_take_ownership (GDU_FORMAT_DIALOG (dialog)),
                                                         filesystem_create_op_callback,
                                                         data);

                }
        }
 out:
        if (dialog != NULL)
                gtk_widget_destroy (dialog);
        if (confirmation_dialog != NULL)
                gtk_widget_destroy (confirmation_dialog);

        if (d != NULL)
                g_object_unref (d);
        if (v != NULL)
                g_object_unref (v);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
partition_modify_op_callback (GduDevice *device,
                              GError    *error,
                              gpointer   user_data)
{
        GduShell *shell = GDU_SHELL (user_data);

        if (error != NULL) {
                GtkWidget *dialog;
                dialog = gdu_error_dialog_new_for_volume (GTK_WINDOW (gdu_shell_get_toplevel (shell)),
                                                          device,
                                                          _("Error modifying partition"),
                                                          error);
                gtk_widget_show_all (dialog);
                gtk_window_present (GTK_WINDOW (dialog));
                gtk_dialog_run (GTK_DIALOG (dialog));
                gtk_widget_destroy (dialog);
                g_error_free (error);
        }
        g_object_unref (shell);
}

static void
on_partition_edit_button_clicked (GduButtonElement *button_element,
                                  gpointer          user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);
        GduPresentable *v;
        GduDevice *d;
        GtkWindow *toplevel;
        GtkWidget *dialog;
        gint response;

        v = NULL;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));
        if (v == NULL)
                goto out;

        d = gdu_presentable_get_device (v);
        if (d == NULL)
                goto out;

        toplevel = GTK_WINDOW (gdu_shell_get_toplevel (gdu_section_get_shell (GDU_SECTION (section))));
        dialog = gdu_edit_partition_dialog_new (toplevel, v);
        gtk_widget_show_all (dialog);
        response = gtk_dialog_run (GTK_DIALOG (dialog));
        if (response == GTK_RESPONSE_APPLY) {
                gchar *partition_type;
                gchar *partition_label;
                gchar **partition_flags;

                g_object_get (dialog,
                              "partition-type", &partition_type,
                              "partition-label", &partition_label,
                              "partition-flags", &partition_flags,
                              NULL);

                gdu_device_op_partition_modify (d,
                                                partition_type,
                                                partition_label,
                                                partition_flags,
                                                partition_modify_op_callback,
                                                g_object_ref (gdu_section_get_shell (GDU_SECTION (section))));

                g_free (partition_type);
                g_free (partition_label);
                g_strfreev (partition_flags);
        }
        gtk_widget_destroy (dialog);

 out:
        if (d != NULL)
                g_object_unref (d);
        if (v != NULL)
                g_object_unref (v);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_luks_forget_passphrase_button_clicked (GduButtonElement *button_element,
                                          gpointer          user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);
        GtkWidget *dialog;
        GduPresentable *v;
        GduDevice *d;
        gint response;

        v = NULL;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));
        if (v == NULL)
                goto out;

        d = gdu_presentable_get_device (v);
        if (d == NULL)
                goto out;

        dialog = gdu_confirmation_dialog_new (GTK_WINDOW (gdu_shell_get_toplevel (gdu_section_get_shell (GDU_SECTION (section)))),
                                              v,
                                              _("Are you sure you want to forget the passphrase?"),
                                              _("_Forget"));
        gtk_widget_show_all (dialog);
        response = gtk_dialog_run (GTK_DIALOG (dialog));
        if (response == GTK_RESPONSE_OK) {
                gdu_util_delete_secret (d);
        }
        gtk_widget_destroy (dialog);

        gdu_section_update (GDU_SECTION (section));

 out:
        if (d != NULL)
                g_object_unref (d);
        if (v != NULL)
                g_object_unref (v);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
luks_lock_op_callback (GduDevice *device,
                       GError    *error,
                       gpointer   user_data)
{
        GduShell *shell = GDU_SHELL (user_data);

        if (error != NULL) {
                GtkWidget *dialog;
                dialog = gdu_error_dialog_new_for_volume (GTK_WINDOW (gdu_shell_get_toplevel (shell)),
                                                          device,
                                                          _("Error locking LUKS volume"),
                                                          error);
                gtk_widget_show_all (dialog);
                gtk_window_present (GTK_WINDOW (dialog));
                gtk_dialog_run (GTK_DIALOG (dialog));
                gtk_widget_destroy (dialog);
                g_error_free (error);
        }
        g_object_unref (shell);
}

static void
on_luks_lock_button_clicked (GduButtonElement *button_element,
                             gpointer          user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);
        GduPresentable *v;
        GduDevice *d;

        v = NULL;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));
        if (v == NULL)
                goto out;

        d = gdu_presentable_get_device (v);
        if (d == NULL)
                goto out;

        gdu_device_op_luks_lock (d,
                                 luks_lock_op_callback,
                                 g_object_ref (gdu_section_get_shell (GDU_SECTION (section))));

 out:
        if (d != NULL)
                g_object_unref (d);
        if (v != NULL)
                g_object_unref (v);
}

/* ---------------------------------------------------------------------------------------------------- */

static void unlock_action_do (GduSectionVolumes *section,
                              GduPresentable *presentable,
                              gboolean bypass_keyring,
                              gboolean indicate_wrong_passphrase);

typedef struct {
        GduSectionVolumes *section;
        GduPresentable *presentable;
        gboolean asked_user;
} UnlockData;

static UnlockData *
unlock_data_new (GduSectionVolumes *section,
                 GduPresentable    *presentable,
                 gboolean           asked_user)
{
        UnlockData *data;
        data = g_new0 (UnlockData, 1);
        data->section = g_object_ref (section);
        data->presentable = g_object_ref (presentable);
        data->asked_user = asked_user;
        return data;
}

static void
unlock_data_free (UnlockData *data)
{
        g_object_unref (data->section);
        g_object_unref (data->presentable);
        g_free (data);
}

static gboolean
unlock_retry (gpointer user_data)
{
        UnlockData *data = user_data;
        GduDevice *device;
        gboolean indicate_wrong_passphrase;

        device = gdu_presentable_get_device (data->presentable);
        if (device != NULL) {
                indicate_wrong_passphrase = FALSE;

                if (!data->asked_user) {
                        /* if we attempted to unlock the device without asking the user
                         * then the password must have come from the keyring.. hence,
                         * since we failed, the password in the keyring is bad. Remove
                         * it.
                         */
                        g_warning ("removing bad password from keyring");
                        gdu_util_delete_secret (device);
                } else {
                        /* we did ask the user on the last try and that passphrase
                         * didn't work.. make sure the new dialog tells him that
                         */
                        indicate_wrong_passphrase = TRUE;
                }

                unlock_action_do (data->section, data->presentable, TRUE, indicate_wrong_passphrase);
                g_object_unref (device);
        }
        unlock_data_free (data);
        return FALSE;
}

static void
unlock_op_cb (GduDevice *device,
              char      *object_path_of_cleartext_device,
              GError    *error,
              gpointer   user_data)
{
        UnlockData *data = user_data;
        GduShell *shell;

        shell = gdu_section_get_shell (GDU_SECTION (data->section));

        if (error != NULL && error->code == GDU_ERROR_INHIBITED) {
                GtkWidget *dialog;

                dialog = gdu_error_dialog_new_for_volume (GTK_WINDOW (gdu_shell_get_toplevel (shell)),
                                                          device,
                                                          _("Error unlocking LUKS volume"),
                                                          error);
                gtk_widget_show_all (dialog);
                gtk_window_present (GTK_WINDOW (dialog));
                gtk_dialog_run (GTK_DIALOG (dialog));
                gtk_widget_destroy (dialog);

                g_error_free (error);
        } else if (error != NULL) {
                /* retry */
                g_idle_add (unlock_retry, data);
                g_error_free (error);
        } else {
                unlock_data_free (data);
                g_free (object_path_of_cleartext_device);
        }
}

static void
unlock_action_do (GduSectionVolumes *section,
                  GduPresentable    *presentable,
                  gboolean           bypass_keyring,
                  gboolean           indicate_wrong_passphrase)
{
        gchar *secret;
        gboolean asked_user;
        GduDevice *device;

        device = gdu_presentable_get_device (presentable);
        if (device != NULL) {
                GduShell *shell;

                shell = gdu_section_get_shell (GDU_SECTION (section));

                secret = gdu_util_dialog_ask_for_secret (GTK_WIDGET (gdu_shell_get_toplevel (shell)),
                                                         presentable,
                                                         bypass_keyring,
                                                         indicate_wrong_passphrase,
                                                         &asked_user);
                if (secret != NULL) {
                        gdu_device_op_luks_unlock (device,
                                                   secret,
                                                   unlock_op_cb,
                                                   unlock_data_new (section,
                                                                    presentable,
                                                                    asked_user));
                        /* scrub the password */
                        memset (secret, '\0', strlen (secret));
                        g_free (secret);
                }

                g_object_unref (device);
        }
}


static void
on_luks_unlock_button_clicked (GduButtonElement *button_element,
                               gpointer          user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);
        GduPresentable *v;

        v = NULL;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));
        if (v == NULL)
                goto out;

        unlock_action_do (section, v, FALSE, FALSE);

 out:
        if (v != NULL)
                g_object_unref (v);
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct {
        char *old_secret;
        char *new_secret;
        gboolean save_in_keyring;
        gboolean save_in_keyring_session;
        GduPresentable *presentable;
        GduSectionVolumes *section;
} ChangePassphraseData;

static void
change_passphrase_data_free (ChangePassphraseData *data)
{
        /* scrub the secrets */
        if (data->old_secret != NULL) {
                memset (data->old_secret, '\0', strlen (data->old_secret));
                g_free (data->old_secret);
        }
        if (data->new_secret != NULL) {
                memset (data->new_secret, '\0', strlen (data->new_secret));
                g_free (data->new_secret);
        }
        if (data->presentable != NULL)
                g_object_unref (data->presentable);
        if (data->section != NULL)
                g_object_unref (data->section);
        g_free (data);
}

static void change_passphrase_do (GduSectionVolumes *section,
                                  GduPresentable    *presentable,
                                  gboolean           bypass_keyring,
                                  gboolean           indicate_wrong_passphrase);

static void
change_passphrase_completed (GduDevice  *device,
                             GError     *error,
                             gpointer    user_data)
{
        ChangePassphraseData *data = user_data;

        if (error == NULL) {
                /* It worked! Now update the keyring */

                if (data->save_in_keyring || data->save_in_keyring_session)
                        gdu_util_save_secret (device, data->new_secret, data->save_in_keyring_session);
                else
                        gdu_util_delete_secret (device);

                change_passphrase_data_free (data);
        } else {
                /* It didn't work. Because the given passphrase was wrong. Try again,
                 * this time forcibly bypassing the keyring and telling the user
                 * the given passphrase was wrong.
                 */
                change_passphrase_do (data->section, data->presentable, TRUE, TRUE);
                change_passphrase_data_free (data);
        }
}

static void
change_passphrase_do (GduSectionVolumes *section,
                      GduPresentable    *presentable,
                      gboolean           bypass_keyring,
                      gboolean           indicate_wrong_passphrase)
{
        GduDevice *device;
        ChangePassphraseData *data;

        device = gdu_presentable_get_device (presentable);
        if (device == NULL) {
                goto out;
        }

        data = g_new0 (ChangePassphraseData, 1);
        data->presentable = g_object_ref (presentable);
        data->section = g_object_ref (section);

        if (!gdu_util_dialog_change_secret (gdu_shell_get_toplevel (gdu_section_get_shell (GDU_SECTION (section))),
                                            presentable,
                                            &data->old_secret,
                                            &data->new_secret,
                                            &data->save_in_keyring,
                                            &data->save_in_keyring_session,
                                            bypass_keyring,
                                            indicate_wrong_passphrase)) {
                change_passphrase_data_free (data);
                goto out;
        }

        gdu_device_op_luks_change_passphrase (device,
                                              data->old_secret,
                                              data->new_secret,
                                              change_passphrase_completed,
                                              data);

out:
        if (device != NULL) {
                g_object_unref (device);
        }
}

static void
on_luks_change_passphrase_button_clicked (GduButtonElement *button_element,
                                          gpointer          user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);
        GduPresentable *v;

        v = NULL;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));
        if (v == NULL)
                goto out;

        change_passphrase_do (section, v, FALSE, FALSE);

 out:
        if (v != NULL)
                g_object_unref (v);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
can_create_partition (GduSectionVolumes *section,
                      GduVolumeHole     *hole,
                      gboolean          *out_can_create_extended)
{
        GduPresentable *drive;
        GduDevice *drive_device;
        gboolean can_create;
        gboolean can_create_extended;
        guint num_primary;
        gboolean has_extended;
        const gchar *part_scheme;
        GduPresentable *enclosing_presentable_for_hole;

        can_create = FALSE;
        can_create_extended = FALSE;

        enclosing_presentable_for_hole = NULL;
        drive_device = NULL;

        drive = gdu_section_get_presentable (GDU_SECTION (section));
        drive_device = gdu_presentable_get_device (drive);
        if (drive_device == NULL)
                goto out;

        part_scheme = gdu_device_partition_table_get_scheme (drive_device);

        if (g_strcmp0 (part_scheme, "mbr") != 0) {
                can_create = TRUE;
                goto out;
        }

        enclosing_presentable_for_hole = gdu_presentable_get_enclosing_presentable (GDU_PRESENTABLE (hole));
        if (GDU_IS_DRIVE (enclosing_presentable_for_hole)) {
                /* hole is in primary partition space */
                if (gdu_drive_count_mbr_partitions (GDU_DRIVE (drive), &num_primary, &has_extended)) {
                        if (num_primary < 4) {
                                can_create = TRUE;
                                if (!has_extended)
                                        can_create_extended = TRUE;
                        }
                }
        } else {
                /* hole is in an extended partition */
                can_create = TRUE;
        }

 out:
        if (enclosing_presentable_for_hole != NULL)
                g_object_unref (enclosing_presentable_for_hole);

        if (drive_device != NULL)
                g_object_unref (drive_device);

        if (out_can_create_extended)
                *out_can_create_extended = can_create_extended;

        return can_create;
}


typedef struct {
        GduShell *shell;
        GduPresentable *presentable;
        char *encrypt_passphrase;
        gboolean save_in_keyring;
        gboolean save_in_keyring_session;
} CreatePartitionData;

static void
create_partition_data_free (CreatePartitionData *data)
{
        if (data->shell != NULL)
                g_object_unref (data->shell);
        if (data->presentable != NULL)
                g_object_unref (data->presentable);
        if (data->encrypt_passphrase != NULL) {
                memset (data->encrypt_passphrase, '\0', strlen (data->encrypt_passphrase));
                g_free (data->encrypt_passphrase);
        }
        g_free (data);
}

static void
partition_create_op_callback (GduDevice  *device,
                              gchar      *created_device_object_path,
                              GError     *error,
                              gpointer    user_data)
{
        CreatePartitionData *data = user_data;

        if (error != NULL) {
                GtkWidget *dialog;
                dialog = gdu_error_dialog_new_for_drive (GTK_WINDOW (gdu_shell_get_toplevel (data->shell)),
                                                         device,
                                                         _("Error creating partition"),
                                                         error);
                gtk_widget_show_all (dialog);
                gtk_window_present (GTK_WINDOW (dialog));
                gtk_dialog_run (GTK_DIALOG (dialog));
                gtk_widget_destroy (dialog);
                g_error_free (error);
        } else {
                if (data->encrypt_passphrase != NULL) {
                        GduDevice *cleartext_device;
                        GduPool *pool;

                        pool = gdu_device_get_pool (device);
                        cleartext_device = gdu_pool_get_by_object_path (pool, created_device_object_path);
                        if (cleartext_device != NULL) {
                                const gchar *cryptotext_device_object_path;

                                cryptotext_device_object_path = gdu_device_luks_cleartext_get_slave (cleartext_device);
                                if (cryptotext_device_object_path != NULL) {
                                        GduDevice *cryptotext_device;

                                        cryptotext_device = gdu_pool_get_by_object_path (pool,
                                                                                         cryptotext_device_object_path);
                                        if (cryptotext_device != NULL) {
                                                /* now set the passphrase if requested */
                                                if (data->save_in_keyring || data->save_in_keyring_session) {
                                                        gdu_util_save_secret (cryptotext_device,
                                                                              data->encrypt_passphrase,
                                                                              data->save_in_keyring_session);
                                                }
                                                g_object_unref (cryptotext_device);
                                        }
                                }
                                g_object_unref (cleartext_device);
                        }
                }

                g_free (created_device_object_path);
        }

        if (data != NULL)
                create_partition_data_free (data);
}

static void
on_partition_create_button_clicked (GduButtonElement *button_element,
                                    gpointer          user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);
        GduPresentable *v;
        GduPresentable *drive;
        GduDevice *drive_device;
        GtkWindow *toplevel;
        GtkWidget *dialog;
        gint response;
        GduFormatDialogFlags flags;
        const gchar *part_scheme;
        gboolean can_create_extended;

        v = NULL;
        drive_device = NULL;
        dialog = NULL;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));
        if (v == NULL || !GDU_IS_VOLUME_HOLE (v))
                goto out;

        drive = gdu_section_get_presentable (GDU_SECTION (section));
        drive_device = gdu_presentable_get_device (drive);
        if (drive_device == NULL)
                goto out;

        part_scheme = gdu_device_partition_table_get_scheme (drive_device);

        flags = GDU_FORMAT_DIALOG_FLAGS_NONE;
        if (!can_create_partition (section, GDU_VOLUME_HOLE (v), &can_create_extended))
                goto out;
        if (can_create_extended)
                flags |= GDU_FORMAT_DIALOG_FLAGS_ALLOW_MSDOS_EXTENDED;

        toplevel = GTK_WINDOW (gdu_shell_get_toplevel (gdu_section_get_shell (GDU_SECTION (section))));
        dialog = gdu_create_partition_dialog_new_for_drive (toplevel,
                                                            drive_device,
                                                            gdu_presentable_get_size (v),
                                                            flags);
        gtk_widget_show_all (dialog);
        response = gtk_dialog_run (GTK_DIALOG (dialog));
        gtk_widget_hide (dialog);
        if (response == GTK_RESPONSE_OK) {
                CreatePartitionData *data;
                gchar *fs_type;
                gchar *fs_label;
                gboolean fs_take_ownership;
                guint64 offset;
                guint64 size;
                gchar *part_type;

                data = g_new0 (CreatePartitionData, 1);
                data->shell = g_object_ref (gdu_section_get_shell (GDU_SECTION (section)));
                data->presentable = g_object_ref (v);

                if (gdu_format_dialog_get_encrypt (GDU_FORMAT_DIALOG (dialog))) {
                        data->encrypt_passphrase = gdu_util_dialog_ask_for_new_secret (GTK_WIDGET (toplevel),
                                                                                       &data->save_in_keyring,
                                                                                       &data->save_in_keyring_session);
                        if (data->encrypt_passphrase == NULL) {
                                create_partition_data_free (data);
                                goto out;
                        }
                }

                fs_type = gdu_format_dialog_get_fs_type (GDU_FORMAT_DIALOG (dialog));
                fs_label = gdu_format_dialog_get_fs_label (GDU_FORMAT_DIALOG (dialog));
                fs_take_ownership = gdu_format_dialog_get_take_ownership (GDU_FORMAT_DIALOG (dialog));

                offset = gdu_presentable_get_offset (v);
                size = gdu_create_partition_dialog_get_size (GDU_CREATE_PARTITION_DIALOG (dialog));

                if (strcmp (fs_type, "msdos_extended_partition") == 0) {
                        part_type = g_strdup ("0x05");
                        g_free (fs_type);
                        g_free (fs_label);
                        fs_type = g_strdup ("");
                        fs_label = g_strdup ("");
                        fs_take_ownership = FALSE;
                } else {
                        part_type = gdu_util_get_default_part_type_for_scheme_and_fstype (part_scheme,
                                                                                          fs_type,
                                                                                          size);
                }

                gdu_device_op_partition_create (drive_device,
                                                offset,
                                                size,
                                                part_type,
                                                "",   /* empty partition label for now */
                                                NULL, /* no flags for the partition */
                                                fs_type,
                                                fs_label,
                                                data->encrypt_passphrase,
                                                fs_take_ownership,
                                                partition_create_op_callback,
                                                data);

                g_free (fs_type);
                g_free (fs_label);
                g_free (part_type);
        }
 out:
        if (dialog != NULL)
                gtk_widget_destroy (dialog);

        if (drive_device != NULL)
                g_object_unref (drive_device);
        if (v != NULL)
                g_object_unref (v);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
filesystem_set_label_op_callback (GduDevice *device,
                                  GError    *error,
                                  gpointer   user_data)
{
        GduShell *shell = GDU_SHELL (user_data);

        if (error != NULL) {
                GtkWidget *dialog;
                dialog = gdu_error_dialog_new_for_volume (GTK_WINDOW (gdu_shell_get_toplevel (shell)),
                                                          device,
                                                          _("Error changing label"),
                                                          error);
                gtk_widget_show_all (dialog);
                gtk_window_present (GTK_WINDOW (dialog));
                gtk_dialog_run (GTK_DIALOG (dialog));
                gtk_widget_destroy (dialog);
                g_error_free (error);
        }
        g_object_unref (shell);
}

static void
on_fs_change_label_button_clicked (GduButtonElement *button_element,
                                   gpointer          user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);
        GduPresentable *v;
        GduDevice *d;
        GtkWindow *toplevel;
        GtkWidget *dialog;
        gint response;

        v = NULL;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));
        if (v == NULL)
                goto out;

        d = gdu_presentable_get_device (v);
        if (d == NULL)
                goto out;

        toplevel = GTK_WINDOW (gdu_shell_get_toplevel (gdu_section_get_shell (GDU_SECTION (section))));
        dialog = gdu_edit_filesystem_dialog_new (toplevel, v);
        gtk_widget_show_all (dialog);
        response = gtk_dialog_run (GTK_DIALOG (dialog));
        if (response == GTK_RESPONSE_APPLY) {
                gchar *label;

                label = gdu_edit_filesystem_dialog_get_label (GDU_EDIT_FILESYSTEM_DIALOG (dialog));

                gdu_device_op_filesystem_set_label (d,
                                                    label,
                                                    filesystem_set_label_op_callback,
                                                    g_object_ref (gdu_section_get_shell (GDU_SECTION (section))));

                g_free (label);
        }
        gtk_widget_destroy (dialog);

 out:
        if (d != NULL)
                g_object_unref (d);
        if (v != NULL)
                g_object_unref (v);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
fsck_op_callback (GduDevice *device,
                  gboolean   is_clean,
                  GError    *error,
                  gpointer   user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);
        GduShell *shell;

        shell = gdu_section_get_shell (GDU_SECTION (section));

        if (error != NULL) {
                GtkWidget *dialog;
                dialog = gdu_error_dialog_new_for_volume (GTK_WINDOW (gdu_shell_get_toplevel (shell)),
                                                          device,
                                                          _("Error checking filesystem on volume"),
                                                          error);
                gtk_widget_show_all (dialog);
                gtk_window_present (GTK_WINDOW (dialog));
                gtk_dialog_run (GTK_DIALOG (dialog));
                gtk_widget_destroy (dialog);
                g_error_free (error);
        } else {
                GtkWidget *dialog;
                gchar *name;
                gchar *vpd_name;
                GduPool *pool;
                GduPresentable *p;

                /* TODO: would probably be nice with GduInformationDialog or, more
                 * generally, a GduProgressDialog class
                 */
                pool = gdu_device_get_pool (device);
                p = gdu_pool_get_volume_by_device (pool, device);

                name = gdu_presentable_get_name (p);
                vpd_name = gdu_presentable_get_vpd_name (p);

                dialog = gtk_message_dialog_new (GTK_WINDOW (GTK_WINDOW (gdu_shell_get_toplevel (shell))),
                                                 GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 is_clean ? GTK_MESSAGE_INFO : GTK_MESSAGE_WARNING,
                                                 GTK_BUTTONS_CLOSE,
                                                 _("File system check on \"%s\" (%s) completed"),
                                                 name, vpd_name);
                if (is_clean) {
                        gtk_message_dialog_format_secondary_markup (GTK_MESSAGE_DIALOG (dialog),
                                                                    _("File system is clean."));
                } else {
                        gtk_message_dialog_format_secondary_markup (GTK_MESSAGE_DIALOG (dialog),
                                                                    _("File system is <b>NOT</b> clean."));
                }
                gtk_window_present (GTK_WINDOW (dialog));
                gtk_dialog_run (GTK_DIALOG (dialog));
                gtk_widget_destroy (dialog);

                g_free (vpd_name);
                g_free (name);
                g_object_unref (pool);
                g_object_unref (p);
        }
        g_object_unref (shell);
}

static void
on_fsck_button_clicked (GduButtonElement *button_element,
                        gpointer          user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);

        GduPresentable *v;
        GduDevice *d;

        v = NULL;
        d = NULL;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));
        if (v == NULL)
                goto out;

        d = gdu_presentable_get_device (v);
        if (d == NULL)
                goto out;

        gdu_device_op_filesystem_check (d,
                                        fsck_op_callback,
                                        g_object_ref (section));

 out:
        if (d != NULL)
                g_object_unref (d);
        if (v != NULL)
                g_object_unref (v);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_usage_element_activated (GduDetailsElement *element,
                            gpointer           user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);
        GduPresentable *v;
        GduDevice *d;
        GduLinuxMdDrive *linux_md_drive;
        GduPool *pool;

        v = NULL;
        d = NULL;
        linux_md_drive = NULL;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));
        if (v == NULL)
                goto out;

        d = gdu_presentable_get_device (v);
        if (d == NULL)
                goto out;

        pool = gdu_device_get_pool (d);

        linux_md_drive = gdu_pool_get_linux_md_drive_by_uuid (pool, gdu_device_linux_md_component_get_uuid (d));
        if (linux_md_drive == NULL)
                goto out;

        gdu_shell_select_presentable (gdu_section_get_shell (GDU_SECTION (section)), GDU_PRESENTABLE (linux_md_drive));

 out:
        if (linux_md_drive != NULL)
                g_object_unref (linux_md_drive);
        if (pool != NULL)
                g_object_unref (pool);
        if (d != NULL)
                g_object_unref (d);
        if (v != NULL)
                g_object_unref (v);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
gdu_section_volumes_update (GduSection *_section)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (_section);
        GduPresentable *v;
        GduDevice *d;
        gchar *s;
        gchar *s2;
        const gchar *id_usage;
        const gchar *id_type;
        gboolean show_fs_mount_button;
        gboolean show_fs_unmount_button;
        gboolean show_fs_check_button;
        gboolean show_fs_change_label_button;
        gboolean show_format_button;
        gboolean show_partition_edit_button;
        gboolean show_partition_delete_button;
        gboolean show_partition_create_button;
        gboolean show_luks_lock_button;
        gboolean show_luks_unlock_button;
        gboolean show_luks_forget_passphrase_button;
        gboolean show_luks_change_passphrase_button;
        GduKnownFilesystem *kfs;
        GPtrArray *elements;

        v = NULL;
        d = NULL;
        kfs = NULL;
        id_usage = "";
        id_type = "";
        show_fs_mount_button = FALSE;
        show_fs_unmount_button = FALSE;
        show_fs_check_button = FALSE;
        show_fs_change_label_button = FALSE;
        show_format_button = FALSE;
        show_partition_edit_button = FALSE;
        show_partition_delete_button = FALSE;
        show_partition_create_button = FALSE;
        show_luks_lock_button = FALSE;
        show_luks_unlock_button = FALSE;
        show_luks_forget_passphrase_button = FALSE;
        show_luks_change_passphrase_button = FALSE;

        v = gdu_volume_grid_get_selected (GDU_VOLUME_GRID (section->priv->grid));

        if (v != NULL) {
                d = gdu_presentable_get_device (v);
                if (d != NULL) {
                        GduPool *pool;

                        pool = gdu_device_get_pool (d);

                        id_usage = gdu_device_id_get_usage (d);
                        id_type = gdu_device_id_get_type (d);
                        kfs = gdu_pool_get_known_filesystem_by_id (pool, id_type);

                        g_object_unref (pool);
                }
        }

        /* ---------------------------------------------------------------------------------------------------- */
        /* rebuild table  */

        if (section->priv->cur_volume != NULL)
                g_object_unref (section->priv->cur_volume);
        section->priv->cur_volume = v != NULL ? g_object_ref (v) : NULL;

        section->priv->usage_element = NULL;
        section->priv->capacity_element = NULL;
        section->priv->partition_type_element = NULL;
        section->priv->partition_flags_element = NULL;
        section->priv->partition_label_element = NULL;
        section->priv->device_element = NULL;
        section->priv->fs_type_element = NULL;
        section->priv->fs_label_element = NULL;
        section->priv->fs_available_element = NULL;
        section->priv->fs_mount_point_element = NULL;

        elements = g_ptr_array_new_with_free_func (g_object_unref);

        section->priv->usage_element = gdu_details_element_new (_("Usage:"), NULL, NULL);
        g_ptr_array_add (elements, section->priv->usage_element);
        g_signal_connect (section->priv->usage_element,
                          "activated",
                          G_CALLBACK (on_usage_element_activated),
                          section);

        section->priv->device_element = gdu_details_element_new (_("Device:"), NULL, NULL);
        g_ptr_array_add (elements, section->priv->device_element);

        section->priv->partition_type_element = gdu_details_element_new (_("Partition Type:"), NULL, NULL);
        g_ptr_array_add (elements, section->priv->partition_type_element);

        if (d != NULL && gdu_device_is_partition (d)) {
                section->priv->partition_label_element = gdu_details_element_new (_("Partition Label:"), NULL, NULL);
                g_ptr_array_add (elements, section->priv->partition_label_element);

                section->priv->partition_flags_element = gdu_details_element_new (_("Partition Flags:"), NULL, NULL);
                g_ptr_array_add (elements, section->priv->partition_flags_element);
        }

        section->priv->capacity_element = gdu_details_element_new (_("Capacity:"), NULL, NULL);
        g_ptr_array_add (elements, section->priv->capacity_element);

        if (g_strcmp0 (id_usage, "filesystem") == 0) {
                section->priv->fs_type_element = gdu_details_element_new (_("Type:"), NULL, NULL);
                g_ptr_array_add (elements, section->priv->fs_type_element);

                section->priv->fs_available_element = gdu_details_element_new (_("Available:"), NULL, NULL);
                g_ptr_array_add (elements, section->priv->fs_available_element);

                section->priv->fs_label_element = gdu_details_element_new (_("Label:"), NULL, NULL);
                g_ptr_array_add (elements, section->priv->fs_label_element);

                section->priv->fs_mount_point_element = gdu_details_element_new (_("Mount Point:"), NULL, NULL);
                g_ptr_array_add (elements, section->priv->fs_mount_point_element);
        }

        gdu_details_table_set_elements (GDU_DETAILS_TABLE (section->priv->details_table), elements);
        g_ptr_array_unref (elements);

        /* ---------------------------------------------------------------------------------------------------- */
        /* reset all elements */

        if (section->priv->usage_element != NULL) {
                gdu_details_element_set_text (section->priv->usage_element, "–");
                gdu_details_element_set_action_text (section->priv->usage_element, NULL);
        }
        if (section->priv->capacity_element != NULL) {
                if (v != NULL) {
                        s = gdu_util_get_size_for_display (gdu_presentable_get_size (v), FALSE, TRUE);
                        gdu_details_element_set_text (section->priv->capacity_element, s);
                        g_free (s);
                } else {
                        gdu_details_element_set_text (section->priv->capacity_element, "–");
                }
        }
        if (section->priv->partition_type_element != NULL) {
                if (d != NULL && gdu_device_is_partition (d)) {
                        const gchar *partition_label;
                        const gchar * const *partition_flags;
                        guint n;
                        GString *str;

                        str = g_string_new (NULL);
                        partition_flags = (const gchar * const *) gdu_device_partition_get_flags (d);
                        for (n = 0; partition_flags != NULL && partition_flags[n] != NULL; n++) {
                                const gchar *flag = partition_flags[n];

                                if (str->len > 0)
                                        g_string_append (str, ", ");

                                if (g_strcmp0 (flag, "boot") == 0) {
                                        /* Translators: This is for the 'boot' MBR/APM partition flag */
                                        g_string_append (str, _("Bootable"));
                                } else if (g_strcmp0 (flag, "required") == 0) {
                                        /* Translators: This is for the 'required' GPT partition flag */
                                        g_string_append (str, _("Required"));
                                } else if (g_strcmp0 (flag, "allocated") == 0) {
                                        /* Translators: This is for the 'allocated' APM partition flag */
                                        g_string_append (str, _("Allocated"));
                                } else if (g_strcmp0 (flag, "allow_read") == 0) {
                                        /* Translators: This is for the 'allow_read' APM partition flag */
                                        g_string_append (str, _("Allow Read"));
                                } else if (g_strcmp0 (flag, "allow_write") == 0) {
                                        /* Translators: This is for the 'allow_write' APM partition flag */
                                        g_string_append (str, _("Allow Write"));
                                } else if (g_strcmp0 (flag, "boot_code_is_pic") == 0) {
                                        /* Translators: This is for the 'boot_code_is_pic' APM partition flag */
                                        g_string_append (str, _("Boot Code PIC"));
                                } else {
                                        g_string_append (str, flag);
                                }
                        }

                        s = gdu_util_get_desc_for_part_type (gdu_device_partition_get_scheme (d),
                                                             gdu_device_partition_get_type (d));
                        gdu_details_element_set_text (section->priv->partition_type_element, s);
                        g_free (s);
                        if (str->len > 0) {
                                gdu_details_element_set_text (section->priv->partition_flags_element, str->str);
                        } else {
                                gdu_details_element_set_text (section->priv->partition_flags_element, "–");
                        }
                        g_string_free (str, TRUE);

                        show_partition_delete_button = TRUE;

                        partition_label = gdu_device_partition_get_label (d);
                        if (partition_label != NULL && strlen (partition_label) > 0) {
                                gdu_details_element_set_text (section->priv->partition_label_element, partition_label);
                        } else {
                                gdu_details_element_set_text (section->priv->partition_label_element, "–");
                        }

                        /* Don't show partition edit button for extended partitions */
                        show_partition_edit_button = TRUE;
                        if (g_strcmp0 (gdu_device_partition_get_scheme (d), "mbr") == 0) {
                                gint partition_type;
                                const gchar *partition_type_str;

                                partition_type_str = gdu_device_partition_get_type (d);
                                partition_type = strtol (partition_type_str, NULL, 0);
                                if ((partition_type == 0x05 || partition_type == 0x0f || partition_type == 0x85)) {
                                        show_partition_edit_button = FALSE;
                                }
                        }
                } else {
                        gdu_details_element_set_text (section->priv->partition_type_element, "–");
                }
        }
        if (section->priv->device_element != NULL) {
                if (d != NULL) {
                        gdu_details_element_set_text (section->priv->device_element,
                                                      gdu_device_get_device_file (d));
                } else {
                        gdu_details_element_set_text (section->priv->device_element, "–");
                }
        }
        if (section->priv->fs_type_element != NULL)
                gdu_details_element_set_text (section->priv->fs_type_element, "–");
        if (section->priv->fs_available_element != NULL)
                gdu_details_element_set_text (section->priv->fs_available_element, "–");
        if (section->priv->fs_label_element != NULL)
                gdu_details_element_set_text (section->priv->fs_label_element, "–");
        if (section->priv->fs_mount_point_element != NULL)
                gdu_details_element_set_text (section->priv->fs_mount_point_element, "–");

        if (v == NULL)
                goto out;

        /* ---------------------------------------------------------------------------------------------------- */
        /* populate according to usage */

        show_format_button = TRUE;
        if (g_strcmp0 (id_usage, "filesystem") == 0) {
                const gchar *label;

                gdu_details_element_set_text (section->priv->usage_element, _("Filesystem"));
                s = gdu_util_get_fstype_for_display (gdu_device_id_get_type (d),
                                                     gdu_device_id_get_version (d),
                                                     TRUE);
                gdu_details_element_set_text (section->priv->fs_type_element, s);
                g_free (s);
                label = gdu_device_id_get_label (d);
                if (label != NULL && strlen (label) > 0)
                        gdu_details_element_set_text (section->priv->fs_label_element, label);

                /* TODO: figure out amount of free space */
                gdu_details_element_set_text (section->priv->fs_available_element, "–");


                if (gdu_device_is_mounted (d)) {
                        const gchar* const *mount_paths;

                        /* For now we ignore if the device is mounted in multiple places */
                        mount_paths = (const gchar* const *) gdu_device_get_mount_paths (d);
                        s = g_strdup_printf ("<a title=\"%s\" href=\"file://%s\">%s</a>",
                                              /* Translators: this the mount point hyperlink tooltip */
                                              _("View files on the volume"),
                                             mount_paths[0],
                                             mount_paths[0]);
                        /* Translators: this the the text for the mount point
                         * item - %s is the mount point, e.g. '/media/disk'
                         */
                        s2 = g_strdup_printf (_("Mounted at %s"), s);
                        gdu_details_element_set_text (section->priv->fs_mount_point_element, s2);
                        g_free (s);
                        g_free (s2);

                        show_fs_unmount_button = TRUE;
                } else {
                        gdu_details_element_set_text (section->priv->fs_mount_point_element, _("Not Mounted"));
                        show_fs_mount_button = TRUE;
                }

                show_fs_check_button = TRUE;

        } else if (g_strcmp0 (id_usage, "crypto") == 0) {

                if (g_strcmp0 (gdu_device_luks_get_holder (d), "/") == 0) {
                        show_luks_unlock_button = TRUE;
                        gdu_details_element_set_text (section->priv->usage_element, _("Encrypted Volume (Locked)"));
                } else {
                        show_luks_lock_button = TRUE;
                        gdu_details_element_set_text (section->priv->usage_element, _("Encrypted Volume (Unlocked)"));
                }
                if (gdu_util_have_secret (d))
                        show_luks_forget_passphrase_button = TRUE;
                show_luks_change_passphrase_button = TRUE;

        } else if (g_strcmp0 (id_usage, "other") == 0 && g_strcmp0 (id_type, "swap") == 0) {

                gdu_details_element_set_text (section->priv->usage_element, _("Swap Space"));

        } else if (d != NULL && gdu_device_is_linux_md_component (d)) {

                gdu_details_element_set_text (section->priv->usage_element, _("RAID Component"));
                gdu_details_element_set_action_text (section->priv->usage_element, _("Go to array"));

        } else if (g_strcmp0 (id_usage, "") == 0 &&
                   d != NULL && gdu_device_is_partition (d) &&
                   g_strcmp0 (gdu_device_partition_get_scheme (d), "mbr") == 0 &&
                   (g_strcmp0 (gdu_device_partition_get_type (d), "0x05") == 0 ||
                    g_strcmp0 (gdu_device_partition_get_type (d), "0x0f") == 0 ||
                    g_strcmp0 (gdu_device_partition_get_type (d), "0x85") == 0)) {
                gdu_details_element_set_text (section->priv->usage_element, _("Container for Logical Partitions"));

                show_format_button = FALSE;
        } else if (GDU_IS_VOLUME_HOLE (v)) {
                GduDevice *drive_device;
                gdu_details_element_set_text (section->priv->usage_element, _("Unallocated Space"));
                drive_device = gdu_presentable_get_device (gdu_section_get_presentable (GDU_SECTION (section)));
                gdu_details_element_set_text (section->priv->device_element,
                                              gdu_device_get_device_file (drive_device));
                g_object_unref (drive_device);

                if (can_create_partition (section, GDU_VOLUME_HOLE (v), NULL))
                        show_partition_create_button = TRUE;
                show_format_button = FALSE;
        }

        if (kfs != NULL) {
                if (show_fs_unmount_button) {
                        if (gdu_known_filesystem_get_supports_online_label_rename (kfs)) {
                                show_fs_change_label_button = TRUE;
                        }
                } else {
                        if (gdu_known_filesystem_get_supports_label_rename (kfs)) {
                                show_fs_change_label_button = TRUE;
                        }
                }
        }

 out:
        gdu_button_element_set_visible (section->priv->fs_mount_button, show_fs_mount_button);
        gdu_button_element_set_visible (section->priv->fs_unmount_button, show_fs_unmount_button);
        gdu_button_element_set_visible (section->priv->fs_check_button, show_fs_check_button);
        gdu_button_element_set_visible (section->priv->fs_change_label_button, show_fs_change_label_button);
        gdu_button_element_set_visible (section->priv->format_button, show_format_button);
        gdu_button_element_set_visible (section->priv->partition_edit_button, show_partition_edit_button);
        gdu_button_element_set_visible (section->priv->partition_delete_button, show_partition_delete_button);
        gdu_button_element_set_visible (section->priv->partition_create_button, show_partition_create_button);
        gdu_button_element_set_visible (section->priv->luks_lock_button, show_luks_lock_button);
        gdu_button_element_set_visible (section->priv->luks_unlock_button, show_luks_unlock_button);
        gdu_button_element_set_visible (section->priv->luks_forget_passphrase_button, show_luks_forget_passphrase_button);
        gdu_button_element_set_visible (section->priv->luks_change_passphrase_button, show_luks_change_passphrase_button);

        if (d != NULL)
                g_object_unref (d);
        if (v != NULL)
                g_object_unref (v);
        if (kfs != NULL)
                g_object_unref (kfs);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_grid_changed (GduVolumeGrid *grid,
                 gpointer       user_data)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (user_data);

        gdu_section_volumes_update (GDU_SECTION (section));
}

/* ---------------------------------------------------------------------------------------------------- */

static void
gdu_section_volumes_constructed (GObject *object)
{
        GduSectionVolumes *section = GDU_SECTION_VOLUMES (object);
        GPtrArray *button_elements;
        GduButtonElement *button_element;
        GtkWidget *grid;
        GtkWidget *align;
        GtkWidget *label;
        GtkWidget *vbox2;
        GtkWidget *table;
        gchar *s;

        gtk_box_set_spacing (GTK_BOX (section), 12);

        /*------------------------------------- */

        label = gtk_label_new (NULL);
        gtk_misc_set_alignment (GTK_MISC (label), 0.0, 0.5);
        s = g_strconcat ("<b>", _("_Volumes"), "</b>", NULL);
        gtk_label_set_markup (GTK_LABEL (label), s);
        gtk_label_set_use_underline (GTK_LABEL (label), TRUE);
        g_free (s);
        gtk_box_pack_start (GTK_BOX (section), label, FALSE, FALSE, 0);

        align = gtk_alignment_new (0.5, 0.5, 1.0, 1.0);
        gtk_alignment_set_padding (GTK_ALIGNMENT (align), 0, 0, 12, 0);
        gtk_box_pack_start (GTK_BOX (section), align, FALSE, FALSE, 0);

        vbox2 = gtk_vbox_new (FALSE, 6);
        gtk_container_add (GTK_CONTAINER (align), vbox2);

        grid = gdu_volume_grid_new (GDU_DRIVE (gdu_section_get_presentable (GDU_SECTION (section))));
        gtk_label_set_mnemonic_widget (GTK_LABEL (label), grid);
        section->priv->grid = grid;
        gtk_box_pack_start (GTK_BOX (vbox2),
                            grid,
                            FALSE,
                            FALSE,
                            0);
        g_signal_connect (grid,
                          "changed",
                          G_CALLBACK (on_grid_changed),
                          section);

        table = gdu_details_table_new (2, NULL);
        section->priv->details_table = table;
        gtk_box_pack_start (GTK_BOX (vbox2), table, FALSE, FALSE, 0);

        align = gtk_alignment_new (0.5, 0.5, 1.0, 1.0);
        gtk_alignment_set_padding (GTK_ALIGNMENT (align), 0, 0, 12, 0);
        gtk_box_pack_start (GTK_BOX (vbox2), align, FALSE, FALSE, 0);

        table = gdu_button_table_new (2, NULL);
        section->priv->button_table = table;
        gtk_container_add (GTK_CONTAINER (align), table);
        button_elements = g_ptr_array_new_with_free_func (g_object_unref);

        button_element = gdu_button_element_new ("gdu-mount",
                                                 _("_Mount Volume"),
                                                 _("Mount the volume"));
        g_signal_connect (button_element,
                          "clicked",
                          G_CALLBACK (on_mount_button_clicked),
                          section);
        g_ptr_array_add (button_elements, button_element);
        section->priv->fs_mount_button = button_element;

        button_element = gdu_button_element_new ("gdu-unmount",
                                                 _("_Unmount Volume"),
                                                 _("Unmount the volume"));
        g_signal_connect (button_element,
                          "clicked",
                          G_CALLBACK (on_unmount_button_clicked),
                          section);
        g_ptr_array_add (button_elements, button_element);
        section->priv->fs_unmount_button = button_element;

        button_element = gdu_button_element_new ("nautilus-gdu",
                                                 _("Fo_rmat Volume"),
                                                 _("Erase or format the volume"));
        g_signal_connect (button_element,
                          "clicked",
                          G_CALLBACK (on_format_button_clicked),
                          section);
        g_ptr_array_add (button_elements, button_element);
        section->priv->format_button = button_element;

        button_element = gdu_button_element_new ("gdu-check-disk",
                                                 _("_Check Filesystem"),
                                                 _("Check and repair the filesystem"));
        g_signal_connect (button_element,
                          "clicked",
                          G_CALLBACK (on_fsck_button_clicked),
                          section);
        g_ptr_array_add (button_elements, button_element);
        section->priv->fs_check_button = button_element;

        /* TODO: better icon */
        button_element = gdu_button_element_new (GTK_STOCK_BOLD,
                                                 _("Edit _Label"),
                                                 _("Change the label of the volume"));
        g_signal_connect (button_element,
                          "clicked",
                          G_CALLBACK (on_fs_change_label_button_clicked),
                          section);
        g_ptr_array_add (button_elements, button_element);
        section->priv->fs_change_label_button = button_element;

        button_element = gdu_button_element_new (GTK_STOCK_EDIT,
                                                 _("Ed_it Partition"),
                                                 _("Change partition type, label and flags"));
        g_signal_connect (button_element,
                          "clicked",
                          G_CALLBACK (on_partition_edit_button_clicked),
                          section);
        g_ptr_array_add (button_elements, button_element);
        section->priv->partition_edit_button = button_element;

        button_element = gdu_button_element_new (GTK_STOCK_DELETE,
                                                 _("D_elete Partition"),
                                                 _("Delete the partition"));
        g_signal_connect (button_element,
                          "clicked",
                          G_CALLBACK (on_partition_delete_button_clicked),
                          section);
        g_ptr_array_add (button_elements, button_element);
        section->priv->partition_delete_button = button_element;

        button_element = gdu_button_element_new (GTK_STOCK_ADD,
                                                 _("_Create Partition"),
                                                 _("Create a new partition"));
        g_signal_connect (button_element,
                          "clicked",
                          G_CALLBACK (on_partition_create_button_clicked),
                          section);
        g_ptr_array_add (button_elements, button_element);
        section->priv->partition_create_button = button_element;

        button_element = gdu_button_element_new ("gdu-encrypted-lock",
                                                 _("_Lock Volume"),
                                                 _("Make encrypted data unavailable"));
        g_signal_connect (button_element,
                          "clicked",
                          G_CALLBACK (on_luks_lock_button_clicked),
                          section);
        g_ptr_array_add (button_elements, button_element);
        section->priv->luks_lock_button = button_element;

        button_element = gdu_button_element_new ("gdu-encrypted-unlock",
                                                 _("_Unlock Volume"),
                                                 _("Make encrypted data available"));
        g_signal_connect (button_element,
                          "clicked",
                          G_CALLBACK (on_luks_unlock_button_clicked),
                          section);
        g_ptr_array_add (button_elements, button_element);
        section->priv->luks_unlock_button = button_element;

        button_element = gdu_button_element_new (GTK_STOCK_CLEAR,
                                                 _("Forge_t Passphrase"),
                                                 _("Delete passphrase from keyring"));
        g_signal_connect (button_element,
                          "clicked",
                          G_CALLBACK (on_luks_forget_passphrase_button_clicked),
                          section);
        g_ptr_array_add (button_elements, button_element);
        section->priv->luks_forget_passphrase_button = button_element;

        /* TODO: not a great choice for the icon but _EDIT would conflict with "Edit Partition */
        button_element = gdu_button_element_new (GTK_STOCK_FIND_AND_REPLACE,
                                                 _("Change _Passphrase"),
                                                 _("Change passphrase"));
        g_signal_connect (button_element,
                          "clicked",
                          G_CALLBACK (on_luks_change_passphrase_button_clicked),
                          section);
        g_ptr_array_add (button_elements, button_element);
        section->priv->luks_change_passphrase_button = button_element;

        gdu_button_table_set_elements (GDU_BUTTON_TABLE (section->priv->button_table), button_elements);
        g_ptr_array_unref (button_elements);

        /* -------------------------------------------------------------------------------- */

        gtk_widget_show_all (GTK_WIDGET (section));

        if (G_OBJECT_CLASS (gdu_section_volumes_parent_class)->constructed != NULL)
                G_OBJECT_CLASS (gdu_section_volumes_parent_class)->constructed (object);
}

static void
gdu_section_volumes_class_init (GduSectionVolumesClass *klass)
{
        GObjectClass *gobject_class;
        GduSectionClass *section_class;

        gobject_class = G_OBJECT_CLASS (klass);
        section_class = GDU_SECTION_CLASS (klass);

        gobject_class->finalize    = gdu_section_volumes_finalize;
        gobject_class->constructed = gdu_section_volumes_constructed;
        section_class->update      = gdu_section_volumes_update;

        g_type_class_add_private (klass, sizeof (GduSectionVolumesPrivate));
}

static void
gdu_section_volumes_init (GduSectionVolumes *section)
{
        section->priv = G_TYPE_INSTANCE_GET_PRIVATE (section, GDU_TYPE_SECTION_VOLUMES, GduSectionVolumesPrivate);
}

GtkWidget *
gdu_section_volumes_new (GduShell       *shell,
                         GduPresentable *presentable)
{
        return GTK_WIDGET (g_object_new (GDU_TYPE_SECTION_VOLUMES,
                                         "shell", shell,
                                         "presentable", presentable,
                                         NULL));
}

gboolean
gdu_section_volumes_select_volume (GduSectionVolumes *section,
                                   GduPresentable    *volume)
{
        g_return_val_if_fail (GDU_IS_SECTION_VOLUMES (section), FALSE);
        return gdu_volume_grid_select (GDU_VOLUME_GRID (section->priv->grid), volume);
}
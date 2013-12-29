/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 William Jon McCann <jmccann@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"

#include <gio/gio.h>

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>
#include <signal.h>
#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <glib-object.h>

#include "gdm-common.h"

#include "gdm-session-enum-types.h"
#include "gdm-launch-environment.h"

#define DBUS_LAUNCH_COMMAND BINDIR "/dbus-launch --exit-with-session"

extern char **environ;

#define GDM_LAUNCH_ENVIRONMENT_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GDM_TYPE_LAUNCH_ENVIRONMENT, GdmLaunchEnvironmentPrivate))

struct GdmLaunchEnvironmentPrivate
{
        GdmSession     *session;
        char           *command;
        GPid            pid;

        GdmSessionVerificationMode verification_mode;

        char           *user_name;
        char           *runtime_dir;

        char           *session_id;
        char           *x11_display_name;
        char           *x11_display_seat_id;
        char           *x11_display_device;
        char           *x11_display_hostname;
        char           *x11_authority_file;
        char           *dbus_session_bus_address;
        gboolean        x11_display_is_local;
};

enum {
        PROP_0,
        PROP_VERIFICATION_MODE,
        PROP_X11_DISPLAY_NAME,
        PROP_X11_DISPLAY_SEAT_ID,
        PROP_X11_DISPLAY_DEVICE,
        PROP_X11_DISPLAY_HOSTNAME,
        PROP_X11_AUTHORITY_FILE,
        PROP_X11_DISPLAY_IS_LOCAL,
        PROP_USER_NAME,
        PROP_RUNTIME_DIR,
        PROP_COMMAND,
        PROP_DBUS_SESSION_BUS_ADDRESS,
};

enum {
        OPENED,
        STARTED,
        STOPPED,
        EXITED,
        DIED,
        LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0, };

static void     gdm_launch_environment_class_init    (GdmLaunchEnvironmentClass *klass);
static void     gdm_launch_environment_init          (GdmLaunchEnvironment      *launch_environment);
static void     gdm_launch_environment_finalize      (GObject                   *object);

G_DEFINE_TYPE (GdmLaunchEnvironment, gdm_launch_environment, G_TYPE_OBJECT)

static void
load_lang_config_file (const char  *config_file,
                       const char **str_array)
{
        gchar         *contents = NULL;
        gchar         *p;
        gchar         *str_joinv;
        gchar         *pattern;
        gchar         *key;
        gchar         *value;
        gsize          length;
        GError        *error;
        GString       *line;
        GRegex        *re;

        g_return_if_fail (config_file != NULL);
        g_return_if_fail (str_array != NULL);

        if (!g_file_test (config_file, G_FILE_TEST_EXISTS)) {
                g_debug ("Cannot access '%s'", config_file);
                return;
        }

        error = NULL;
        if (!g_file_get_contents (config_file, &contents, &length, &error)) {
                g_debug ("Failed to parse '%s': %s",
                         config_file,
                         (error && error->message) ? error->message : "(null)");
                g_error_free (error);
                return;
        }

        if (!g_utf8_validate (contents, length, NULL)) {
                g_warning ("Invalid UTF-8 in '%s'", config_file);
                g_free (contents);
                return;
        }

        str_joinv = g_strjoinv ("|", (char **) str_array);
        if (str_joinv == NULL) {
                g_warning ("Error in joined");
                g_free (contents);
                return;
        }

        pattern = g_strdup_printf ("(?P<key>(%s))=(\")?(?P<value>[^\"]*)?(\")?",
                                   str_joinv);
        error = NULL;
        re = g_regex_new (pattern, 0, 0, &error);
        g_free (pattern);
        g_free (str_joinv);
        if (re == NULL) {
                g_warning ("Failed to regex: %s",
                           (error && error->message) ? error->message : "(null)");
                g_error_free (error);
                g_free (contents);
                return;
        }

        line = g_string_new ("");
        for (p = contents; p && *p; p = g_utf8_find_next_char (p, NULL)) {
                gunichar ch;
                GMatchInfo *match_info = NULL;

                ch = g_utf8_get_char (p);
                if ((ch != '\n') && (ch != '\0')) {
                        g_string_append_unichar (line, ch);
                        continue;
                }

                if (line->str && g_utf8_get_char (line->str) == '#') {
                        goto next_line;
                }

                if (!g_regex_match (re, line->str, 0, &match_info)) {
                        goto next_line;
                }

                if (!g_match_info_matches (match_info)) {
                        goto next_line;
                }

                key = g_match_info_fetch_named (match_info, "key");
                value = g_match_info_fetch_named (match_info, "value");

                if (key && *key && value && *value) {
                        g_setenv (key, value, TRUE);
                } else if (key && *key) {
                        g_unsetenv (key);
                }

                g_free (key);
                g_free (value);
next_line:
                g_match_info_free (match_info);
                g_string_set_size (line, 0);
        }

        g_string_free (line, TRUE);
        g_regex_unref (re);
        g_free (contents);
}

static GHashTable *
build_launch_environment (GdmLaunchEnvironment *launch_environment,
                          gboolean              start_session)
{
        GHashTable    *hash;
        struct passwd *pwent;
        static const char * const optional_environment[] = {
                "LANG", "LANGUAGE", "LC_CTYPE", "LC_NUMERIC", "LC_TIME",
                "LC_COLLATE", "LC_MONETARY", "LC_MESSAGES", "LC_PAPER",
                "LC_NAME", "LC_ADDRESS", "LC_TELEPHONE", "LC_MEASUREMENT",
                "LC_IDENTIFICATION", "LC_ALL", "WINDOWPATH",
                NULL
        };
        char *system_data_dirs;
        int i;

        load_lang_config_file (LANG_CONFIG_FILE,
                               (const char **) optional_environment);

        /* create a hash table of current environment, then update keys has necessary */
        hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

        for (i = 0; optional_environment[i] != NULL; i++) {
                if (g_getenv (optional_environment[i]) == NULL) {
                        continue;
                }

                g_hash_table_insert (hash,
                                     g_strdup (optional_environment[i]),
                                     g_strdup (g_getenv (optional_environment[i])));
        }

        system_data_dirs = g_strjoinv (":", (char **) g_get_system_data_dirs ());

        g_hash_table_insert (hash,
                             g_strdup ("XDG_DATA_DIRS"),
                             g_strdup_printf ("%s:%s",
                                              DATADIR "/gdm/greeter",
                                              system_data_dirs));
        g_free (system_data_dirs);

        g_hash_table_insert (hash, g_strdup ("XAUTHORITY"), g_strdup (launch_environment->priv->x11_authority_file));
        g_hash_table_insert (hash, g_strdup ("DISPLAY"), g_strdup (launch_environment->priv->x11_display_name));

        g_hash_table_insert (hash, g_strdup ("LOGNAME"), g_strdup (launch_environment->priv->user_name));
        g_hash_table_insert (hash, g_strdup ("USER"), g_strdup (launch_environment->priv->user_name));
        g_hash_table_insert (hash, g_strdup ("USERNAME"), g_strdup (launch_environment->priv->user_name));

        g_hash_table_insert (hash, g_strdup ("GDM_VERSION"), g_strdup (VERSION));
        g_hash_table_remove (hash, "MAIL");

        g_hash_table_insert (hash, g_strdup ("HOME"), g_strdup ("/"));
        g_hash_table_insert (hash, g_strdup ("PWD"), g_strdup ("/"));
        g_hash_table_insert (hash, g_strdup ("SHELL"), g_strdup ("/bin/sh"));

        gdm_get_pwent_for_name (launch_environment->priv->user_name, &pwent);
        if (pwent != NULL) {
                if (pwent->pw_dir != NULL && pwent->pw_dir[0] != '\0') {
                        g_hash_table_insert (hash, g_strdup ("HOME"), g_strdup (pwent->pw_dir));
                        g_hash_table_insert (hash, g_strdup ("PWD"), g_strdup (pwent->pw_dir));
                }

                g_hash_table_insert (hash, g_strdup ("SHELL"), g_strdup (pwent->pw_shell));
        }

        if (start_session && launch_environment->priv->x11_display_seat_id != NULL) {
                char *seat_id;

                seat_id = launch_environment->priv->x11_display_seat_id;
                if (g_str_has_prefix (seat_id, "/org/freedesktop/ConsoleKit/")) {
                        seat_id += strlen ("/org/freedesktop/ConsoleKit/");
                }

                g_hash_table_insert (hash, g_strdup ("GDM_SEAT_ID"), g_strdup (seat_id));
        }

        g_hash_table_insert (hash, g_strdup ("PATH"), g_strdup (g_getenv ("PATH")));

        g_hash_table_insert (hash, g_strdup ("RUNNING_UNDER_GDM"), g_strdup ("true"));
        g_hash_table_insert (hash, g_strdup ("GVFS_DISABLE_FUSE"), g_strdup ("1"));
        g_hash_table_insert (hash, g_strdup ("GIO_USE_VFS"), g_strdup ("local"));
        g_hash_table_insert (hash, g_strdup ("GVFS_REMOTE_VOLUME_MONITOR_IGNORE"), g_strdup ("1"));
        g_hash_table_insert (hash, g_strdup ("DCONF_PROFILE"), g_strdup ("gdm"));

        if (launch_environment->priv->dbus_session_bus_address) {
                g_hash_table_insert (hash, g_strdup ("DBUS_SESSION_BUS_ADDRESS"), g_strdup (launch_environment->priv->dbus_session_bus_address));
        }

        return hash;
}

static void
on_session_setup_complete (GdmSession        *session,
                           const char        *service_name,
                           GdmLaunchEnvironment *launch_environment)
{
        GHashTable       *hash;
        GHashTableIter    iter;
        gpointer          key, value;

        hash = build_launch_environment (launch_environment, TRUE);

        g_hash_table_iter_init (&iter, hash);
        while (g_hash_table_iter_next (&iter, &key, &value)) {
                gdm_session_set_environment_variable (launch_environment->priv->session, key, value);
        }
        g_hash_table_destroy (hash);

        gdm_session_select_session_type (launch_environment->priv->session, "LoginWindow");
}

static void
on_session_opened (GdmSession           *session,
                   const char           *service_name,
                   const char           *session_id,
                   GdmLaunchEnvironment *launch_environment)
{
        launch_environment->priv->session_id = g_strdup (session_id);

        g_signal_emit (G_OBJECT (launch_environment), signals [OPENED], 0);
        gdm_session_start_session (launch_environment->priv->session, service_name);
}

static void
on_session_started (GdmSession           *session,
                    const char           *service_name,
                    int                   pid,
                    GdmLaunchEnvironment *launch_environment)
{
        launch_environment->priv->pid = pid;
        g_signal_emit (G_OBJECT (launch_environment), signals [STARTED], 0);
}

static void
on_session_exited (GdmSession           *session,
                   int                   exit_code,
                   GdmLaunchEnvironment *launch_environment)
{
        gdm_session_stop_conversation (launch_environment->priv->session, "gdm-launch-environment");

        g_signal_emit (G_OBJECT (launch_environment), signals [EXITED], 0, exit_code);
}

static void
on_session_died (GdmSession           *session,
                 int                   signal_number,
                 GdmLaunchEnvironment *launch_environment)
{
        gdm_session_stop_conversation (launch_environment->priv->session, "gdm-launch-environment");

        g_signal_emit (G_OBJECT (launch_environment), signals [DIED], 0, signal_number);
}

static void
on_conversation_started (GdmSession           *session,
                         const char           *service_name,
                         GdmLaunchEnvironment *launch_environment)
{
        char             *log_path;
        char             *log_file;

        log_file = g_strdup_printf ("%s-greeter.log", launch_environment->priv->x11_display_name);
        log_path = g_build_filename (LOGDIR, log_file, NULL);
        g_free (log_file);

        gdm_session_setup_for_program (launch_environment->priv->session,
                                       "gdm-launch-environment",
                                       launch_environment->priv->user_name,
                                       log_path);
        g_free (log_path);
}

static void
on_conversation_stopped (GdmSession           *session,
                         const char           *service_name,
                         GdmLaunchEnvironment *launch_environment)
{
        GdmSession *conversation_session;

        conversation_session = launch_environment->priv->session;
        launch_environment->priv->session = NULL;

        g_debug ("GdmLaunchEnvironment: conversation stopped");

        if (launch_environment->priv->pid > 1) {
                gdm_signal_pid (-launch_environment->priv->pid, SIGTERM);
                g_signal_emit (G_OBJECT (launch_environment), signals [STOPPED], 0);
        }

        if (conversation_session != NULL) {
                gdm_session_close (conversation_session);
                g_object_unref (conversation_session);
        }
}

static gboolean
ensure_directory_with_uid_gid (const char  *path,
                               uid_t        uid,
                               gid_t        gid,
                               GError     **error)
{
        if (mkdir (path, 0700) == -1 && errno != EEXIST) {
                g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Failed to create directory %s: %s", path,
                             g_strerror (errno));
                return FALSE;
        }
        if (chown (path, uid, gid) == -1) {
                g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Failed to set owner of %s: %s", path,
                             g_strerror (errno));
                return FALSE;
        }
        return TRUE;
}

/**
 * gdm_launch_environment_start:
 * @disp: Pointer to a GdmDisplay structure
 *
 * Starts a local X launch_environment. Handles retries and fatal errors properly.
 */
gboolean
gdm_launch_environment_start (GdmLaunchEnvironment *launch_environment)
{
        gboolean          res = FALSE;
        GError           *local_error = NULL;
        GError          **error = &local_error;
        struct passwd    *passwd_entry;
        uid_t             uid;
        gid_t             gid;

        g_debug ("GdmLaunchEnvironment: Starting...");

        if (!gdm_get_pwent_for_name (launch_environment->priv->user_name, &passwd_entry)) {
                g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Unknown user %s",
                             launch_environment->priv->user_name);
                goto out;
        }

        uid = passwd_entry->pw_uid;
        gid = passwd_entry->pw_gid;

        g_debug ("GdmLaunchEnvironment: Setting up run time dir %s",
                 launch_environment->priv->runtime_dir);
        if (!ensure_directory_with_uid_gid (launch_environment->priv->runtime_dir, uid, gid, error)) {
                goto out;
        }

        /* Create the home directory too */
        if (!ensure_directory_with_uid_gid (passwd_entry->pw_dir, uid, gid, error))
                goto out;

        launch_environment->priv->session = gdm_session_new (launch_environment->priv->verification_mode,
                                                             uid,
                                                             launch_environment->priv->x11_display_name,
                                                             launch_environment->priv->x11_display_hostname,
                                                             launch_environment->priv->x11_display_device,
                                                             launch_environment->priv->x11_display_seat_id,
                                                             launch_environment->priv->x11_authority_file,
                                                             launch_environment->priv->x11_display_is_local,
                                                             NULL);

        g_signal_connect (launch_environment->priv->session,
                          "conversation-started",
                          G_CALLBACK (on_conversation_started),
                          launch_environment);
        g_signal_connect (launch_environment->priv->session,
                          "conversation-stopped",
                          G_CALLBACK (on_conversation_stopped),
                          launch_environment);
        g_signal_connect (launch_environment->priv->session,
                          "setup-complete",
                          G_CALLBACK (on_session_setup_complete),
                          launch_environment);
        g_signal_connect (launch_environment->priv->session,
                          "session-opened",
                          G_CALLBACK (on_session_opened),
                          launch_environment);
        g_signal_connect (launch_environment->priv->session,
                          "session-started",
                          G_CALLBACK (on_session_started),
                          launch_environment);
        g_signal_connect (launch_environment->priv->session,
                          "session-exited",
                          G_CALLBACK (on_session_exited),
                          launch_environment);
        g_signal_connect (launch_environment->priv->session,
                          "session-died",
                          G_CALLBACK (on_session_died),
                          launch_environment);

        gdm_session_start_conversation (launch_environment->priv->session, "gdm-launch-environment");

        if (launch_environment->priv->dbus_session_bus_address) {
                gdm_session_select_program (launch_environment->priv->session, launch_environment->priv->command);
        } else {
                /* wrap it in dbus-launch */
                char *command = g_strdup_printf ("%s %s", DBUS_LAUNCH_COMMAND, launch_environment->priv->command);

                gdm_session_select_program (launch_environment->priv->session, command);
                g_free (command);
        }

        res = TRUE;
 out:
        if (local_error) {
                g_critical ("GdmLaunchEnvironment: %s", local_error->message);
                g_clear_error (&local_error);
        }
        return res;
}

gboolean
gdm_launch_environment_stop (GdmLaunchEnvironment *launch_environment)
{
        if (launch_environment->priv->pid > 1) {
                gdm_signal_pid (-launch_environment->priv->pid, SIGTERM);
        } else {
                if (launch_environment->priv->session != NULL) {
                        gdm_session_stop_conversation (launch_environment->priv->session, "gdm-launch-environment");
                        gdm_session_close (launch_environment->priv->session);

                        g_clear_object (&launch_environment->priv->session);
                }

                g_signal_emit (G_OBJECT (launch_environment), signals [STOPPED], 0);
        }

        return TRUE;
}

GdmSession *
gdm_launch_environment_get_session (GdmLaunchEnvironment *launch_environment)
{
        return launch_environment->priv->session;
}

char *
gdm_launch_environment_get_session_id (GdmLaunchEnvironment *launch_environment)
{
        return g_strdup (launch_environment->priv->session_id);
}

static void
_gdm_launch_environment_set_verification_mode (GdmLaunchEnvironment           *launch_environment,
                                               GdmSessionVerificationMode      verification_mode)
{
        launch_environment->priv->verification_mode = verification_mode;
}

static void
_gdm_launch_environment_set_x11_display_name (GdmLaunchEnvironment *launch_environment,
                                              const char           *name)
{
        g_free (launch_environment->priv->x11_display_name);
        launch_environment->priv->x11_display_name = g_strdup (name);
}

static void
_gdm_launch_environment_set_x11_display_seat_id (GdmLaunchEnvironment *launch_environment,
                                                 const char           *sid)
{
        g_free (launch_environment->priv->x11_display_seat_id);
        launch_environment->priv->x11_display_seat_id = g_strdup (sid);
}

static void
_gdm_launch_environment_set_x11_display_hostname (GdmLaunchEnvironment *launch_environment,
                                                  const char           *name)
{
        g_free (launch_environment->priv->x11_display_hostname);
        launch_environment->priv->x11_display_hostname = g_strdup (name);
}

static void
_gdm_launch_environment_set_x11_display_device (GdmLaunchEnvironment *launch_environment,
                                                const char           *name)
{
        g_free (launch_environment->priv->x11_display_device);
        launch_environment->priv->x11_display_device = g_strdup (name);
}

static void
_gdm_launch_environment_set_x11_display_is_local (GdmLaunchEnvironment *launch_environment,
                                                  gboolean              is_local)
{
        launch_environment->priv->x11_display_is_local = is_local;
}

static void
_gdm_launch_environment_set_x11_authority_file (GdmLaunchEnvironment *launch_environment,
                                                const char           *file)
{
        g_free (launch_environment->priv->x11_authority_file);
        launch_environment->priv->x11_authority_file = g_strdup (file);
}

static void
_gdm_launch_environment_set_user_name (GdmLaunchEnvironment *launch_environment,
                                       const char           *name)
{
        g_free (launch_environment->priv->user_name);
        launch_environment->priv->user_name = g_strdup (name);
}

static void
_gdm_launch_environment_set_runtime_dir (GdmLaunchEnvironment *launch_environment,
                                         const char           *dir)
{
        g_free (launch_environment->priv->runtime_dir);
        launch_environment->priv->runtime_dir = g_strdup (dir);
}

static void
_gdm_launch_environment_set_dbus_session_bus_address (GdmLaunchEnvironment *launch_environment,
                                                      const char           *addr)
{
        g_free (launch_environment->priv->dbus_session_bus_address);
        launch_environment->priv->dbus_session_bus_address = g_strdup (addr);
}

static void
_gdm_launch_environment_set_command (GdmLaunchEnvironment *launch_environment,
                                     const char           *name)
{
        g_free (launch_environment->priv->command);
        launch_environment->priv->command = g_strdup (name);
}

static void
gdm_launch_environment_set_property (GObject      *object,
                                     guint         prop_id,
                                     const GValue *value,
                                     GParamSpec   *pspec)
{
        GdmLaunchEnvironment *self;

        self = GDM_LAUNCH_ENVIRONMENT (object);

        switch (prop_id) {
        case PROP_VERIFICATION_MODE:
                _gdm_launch_environment_set_verification_mode (self, g_value_get_enum (value));
                break;
        case PROP_X11_DISPLAY_NAME:
                _gdm_launch_environment_set_x11_display_name (self, g_value_get_string (value));
                break;
        case PROP_X11_DISPLAY_SEAT_ID:
                _gdm_launch_environment_set_x11_display_seat_id (self, g_value_get_string (value));
                break;
        case PROP_X11_DISPLAY_HOSTNAME:
                _gdm_launch_environment_set_x11_display_hostname (self, g_value_get_string (value));
                break;
        case PROP_X11_DISPLAY_DEVICE:
                _gdm_launch_environment_set_x11_display_device (self, g_value_get_string (value));
                break;
        case PROP_X11_DISPLAY_IS_LOCAL:
                _gdm_launch_environment_set_x11_display_is_local (self, g_value_get_boolean (value));
                break;
        case PROP_X11_AUTHORITY_FILE:
                _gdm_launch_environment_set_x11_authority_file (self, g_value_get_string (value));
                break;
        case PROP_USER_NAME:
                _gdm_launch_environment_set_user_name (self, g_value_get_string (value));
                break;
        case PROP_RUNTIME_DIR:
                _gdm_launch_environment_set_runtime_dir (self, g_value_get_string (value));
                break;
        case PROP_DBUS_SESSION_BUS_ADDRESS:
                _gdm_launch_environment_set_dbus_session_bus_address (self, g_value_get_string (value));
                break;
        case PROP_COMMAND:
                _gdm_launch_environment_set_command (self, g_value_get_string (value));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gdm_launch_environment_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
        GdmLaunchEnvironment *self;

        self = GDM_LAUNCH_ENVIRONMENT (object);

        switch (prop_id) {
        case PROP_VERIFICATION_MODE:
                g_value_set_enum (value, self->priv->verification_mode);
                break;
        case PROP_X11_DISPLAY_NAME:
                g_value_set_string (value, self->priv->x11_display_name);
                break;
        case PROP_X11_DISPLAY_SEAT_ID:
                g_value_set_string (value, self->priv->x11_display_seat_id);
                break;
        case PROP_X11_DISPLAY_HOSTNAME:
                g_value_set_string (value, self->priv->x11_display_hostname);
                break;
        case PROP_X11_DISPLAY_DEVICE:
                g_value_set_string (value, self->priv->x11_display_device);
                break;
        case PROP_X11_DISPLAY_IS_LOCAL:
                g_value_set_boolean (value, self->priv->x11_display_is_local);
                break;
        case PROP_X11_AUTHORITY_FILE:
                g_value_set_string (value, self->priv->x11_authority_file);
                break;
        case PROP_USER_NAME:
                g_value_set_string (value, self->priv->user_name);
                break;
        case PROP_RUNTIME_DIR:
                g_value_set_string (value, self->priv->runtime_dir);
                break;
        case PROP_DBUS_SESSION_BUS_ADDRESS:
                g_value_set_string (value, self->priv->dbus_session_bus_address);
                break;
        case PROP_COMMAND:
                g_value_set_string (value, self->priv->command);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
                break;
        }
}

static void
gdm_launch_environment_class_init (GdmLaunchEnvironmentClass *klass)
{
        GObjectClass    *object_class = G_OBJECT_CLASS (klass);

        object_class->get_property = gdm_launch_environment_get_property;
        object_class->set_property = gdm_launch_environment_set_property;
        object_class->finalize = gdm_launch_environment_finalize;

        g_type_class_add_private (klass, sizeof (GdmLaunchEnvironmentPrivate));

        g_object_class_install_property (object_class,
                                         PROP_VERIFICATION_MODE,
                                         g_param_spec_enum ("verification-mode",
                                                            "verification mode",
                                                            "verification mode",
                                                            GDM_TYPE_SESSION_VERIFICATION_MODE,
                                                            GDM_SESSION_VERIFICATION_MODE_LOGIN,
                                                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_X11_DISPLAY_NAME,
                                         g_param_spec_string ("x11-display-name",
                                                              "name",
                                                              "name",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_X11_DISPLAY_SEAT_ID,
                                         g_param_spec_string ("x11-display-seat-id",
                                                              "seat id",
                                                              "seat id",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_X11_DISPLAY_HOSTNAME,
                                         g_param_spec_string ("x11-display-hostname",
                                                              "hostname",
                                                              "hostname",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_X11_DISPLAY_DEVICE,
                                         g_param_spec_string ("x11-display-device",
                                                              "device",
                                                              "device",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_X11_DISPLAY_IS_LOCAL,
                                         g_param_spec_boolean ("x11-display-is-local",
                                                               "is local",
                                                               "is local",
                                                               FALSE,
                                                               G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_X11_AUTHORITY_FILE,
                                         g_param_spec_string ("x11-authority-file",
                                                              "authority file",
                                                              "authority file",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_USER_NAME,
                                         g_param_spec_string ("user-name",
                                                              "user name",
                                                              "user name",
                                                              GDM_USERNAME,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_RUNTIME_DIR,
                                         g_param_spec_string ("runtime-dir",
                                                              "runtime dir",
                                                              "runtime dir",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        g_object_class_install_property (object_class,
                                         PROP_COMMAND,
                                         g_param_spec_string ("command",
                                                              "command",
                                                              "command",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
        g_object_class_install_property (object_class,
                                         PROP_DBUS_SESSION_BUS_ADDRESS,
                                         g_param_spec_string ("dbus-session-bus-address",
                                                              "D-Bus session bus address",
                                                              "D-Bus session bus address",
                                                              NULL,
                                                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
        signals [OPENED] =
                g_signal_new ("opened",
                              G_OBJECT_CLASS_TYPE (object_class),
                              G_SIGNAL_RUN_FIRST,
                              G_STRUCT_OFFSET (GdmLaunchEnvironmentClass, opened),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE,
                              0);
        signals [STARTED] =
                g_signal_new ("started",
                              G_OBJECT_CLASS_TYPE (object_class),
                              G_SIGNAL_RUN_FIRST,
                              G_STRUCT_OFFSET (GdmLaunchEnvironmentClass, started),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE,
                              0);
        signals [STOPPED] =
                g_signal_new ("stopped",
                              G_OBJECT_CLASS_TYPE (object_class),
                              G_SIGNAL_RUN_FIRST,
                              G_STRUCT_OFFSET (GdmLaunchEnvironmentClass, stopped),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE,
                              0);
        signals [EXITED] =
                g_signal_new ("exited",
                              G_OBJECT_CLASS_TYPE (object_class),
                              G_SIGNAL_RUN_FIRST,
                              G_STRUCT_OFFSET (GdmLaunchEnvironmentClass, exited),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__INT,
                              G_TYPE_NONE,
                              1,
                              G_TYPE_INT);
        signals [DIED] =
                g_signal_new ("died",
                              G_OBJECT_CLASS_TYPE (object_class),
                              G_SIGNAL_RUN_FIRST,
                              G_STRUCT_OFFSET (GdmLaunchEnvironmentClass, died),
                              NULL,
                              NULL,
                              g_cclosure_marshal_VOID__INT,
                              G_TYPE_NONE,
                              1,
                              G_TYPE_INT);
}

static void
gdm_launch_environment_init (GdmLaunchEnvironment *launch_environment)
{

        launch_environment->priv = GDM_LAUNCH_ENVIRONMENT_GET_PRIVATE (launch_environment);

        launch_environment->priv->command = NULL;
        launch_environment->priv->session = NULL;
}

static void
gdm_launch_environment_finalize (GObject *object)
{
        GdmLaunchEnvironment *launch_environment;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GDM_IS_LAUNCH_ENVIRONMENT (object));

        launch_environment = GDM_LAUNCH_ENVIRONMENT (object);

        g_return_if_fail (launch_environment->priv != NULL);

        gdm_launch_environment_stop (launch_environment);

        if (launch_environment->priv->session) {
                g_object_unref (launch_environment->priv->session);
        }

        g_free (launch_environment->priv->command);
        g_free (launch_environment->priv->user_name);
        g_free (launch_environment->priv->runtime_dir);
        g_free (launch_environment->priv->dbus_session_bus_address);
        g_free (launch_environment->priv->x11_display_name);
        g_free (launch_environment->priv->x11_display_seat_id);
        g_free (launch_environment->priv->x11_display_device);
        g_free (launch_environment->priv->x11_display_hostname);
        g_free (launch_environment->priv->x11_authority_file);
        g_free (launch_environment->priv->session_id);

        G_OBJECT_CLASS (gdm_launch_environment_parent_class)->finalize (object);
}

/* vi: set et sw=4 ts=4 cino=t0,(0: */
/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of libaccounts-glib
 *
 * Copyright (C) 2009-2010 Nokia Corporation.
 *
 * Contact: Alberto Mardegan <alberto.mardegan@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

/**
 * SECTION:ag-manager
 * @short_description: The account manager object
 * @include: libaccounts-glib/ag-manager.h
 *
 * The #AgManager is the main object in this library.
 */

#include "config.h"
#include "ag-manager.h"

#include "ag-account-service.h"
#include "ag-errors.h"
#include "ag-internals.h"
#include "ag-util.h"
#include <dbus/dbus-glib-lowlevel.h>
#include <sched.h>
#include <sqlite3.h>
#include <string.h>
#include <unistd.h>

#ifndef DATABASE_DIR
#define DATABASE_DIR ".accounts"
#endif

enum
{
    PROP_0,

    PROP_SERVICE_TYPE,
};

enum
{
    ACCOUNT_CREATED,
    ACCOUNT_DELETED,
    ACCOUNT_ENABLED,
    ACCOUNT_UPDATED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct _AgManagerPrivate {
    sqlite3 *db;

    sqlite3_stmt *begin_stmt;
    sqlite3_stmt *commit_stmt;
    sqlite3_stmt *rollback_stmt;

    sqlite3_int64 last_service_id;
    sqlite3_int64 last_account_id;

    DBusConnection *dbus_conn;

    /* Cache for AgService */
    GHashTable *services;

    /* Weak references to loaded accounts */
    GHashTable *accounts;

    /* list of StoreCbData awaiting for exclusive locks */
    GList *locks;

    /* list of EmittedSignalData for the signals emitted by this instance */
    GList *emitted_signals;

    /* list of ProcessedSignalData, to avoid processing signals twice */
    GList *processed_signals;

    /* D-Bus object paths we are listening to */
    GPtrArray *object_paths;

    GError *last_error;

    guint db_timeout;

    guint abort_on_db_timeout : 1;
    guint is_disposed : 1;

    gchar *service_type;
};

typedef struct {
    AgManager *manager;
    AgAccount *account;
    gchar *sql;
    AgAccountChanges *changes;
    guint id;
    AgAccountStoreCb callback;
    gpointer user_data;
} StoreCbData;

typedef struct {
    struct timespec ts;
    gboolean must_process;
} EmittedSignalData;

typedef struct {
    struct timespec ts;
} ProcessedSignalData;

G_DEFINE_TYPE (AgManager, ag_manager, G_TYPE_OBJECT);

#define AG_MANAGER_PRIV(obj) (AG_MANAGER(obj)->priv)

static void store_cb_data_free (StoreCbData *sd);
static void account_weak_notify (gpointer userdata, GObject *dead_account);

typedef gpointer (*AgDataFileLoadFunc) (AgManager *self,
                                        const gchar *base_name);

static void
add_data_files_from_dir (AgManager *manager, const gchar *dirname,
                         GHashTable *loaded_files, const gchar *suffix,
                         AgDataFileLoadFunc load_file_func)
{
    const gchar *filename;
    gchar *base_name;
    gint suffix_length;
    gpointer loaded_file;
    GDir *dir;

    g_return_if_fail (dirname != NULL);

    dir = g_dir_open (dirname, 0, NULL);
    if (!dir) return;

    suffix_length = strlen (suffix);

    while ((filename = g_dir_read_name (dir)) != NULL)
    {
        if (filename[0] == '.')
            continue;

        if (!g_str_has_suffix (filename, suffix))
            continue;

        base_name = g_strndup (filename, (strlen (filename) - suffix_length));

        /* if there is already a service with the same name in the list, then
         * we skip this one (we process directories in descending order of
         * priority) */
        if (g_hash_table_lookup (loaded_files, base_name) != NULL)
        {
            g_free (base_name);
            continue;
        }

        loaded_file = load_file_func (manager, base_name);
        if (G_UNLIKELY (!loaded_file))
        {
            g_free (base_name);
            continue;
        }

        g_hash_table_insert (loaded_files, base_name, loaded_file);
    }

    g_dir_close (dir);
}

static GList *
list_data_files (AgManager *manager,
                 const gchar *suffix,
                 const gchar *env_var,
                 const gchar *subdir,
                 AgDataFileLoadFunc load_file_func)
{
    GHashTable *loaded_files;
    GList *file_list;
    const gchar * const *dirs;
    const gchar *env_dirname, *datadir;
    gchar *dirname;

    loaded_files = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, NULL);

    env_dirname = g_getenv (env_var);
    if (env_dirname)
    {
        add_data_files_from_dir (manager, env_dirname, loaded_files, suffix,
                                 load_file_func);
        /* If the environment variable is set, don't look in other places */
        goto finish;
    }

    datadir = g_get_user_data_dir ();
    if (G_LIKELY (datadir))
    {
        dirname = g_build_filename (datadir, subdir, NULL);
        add_data_files_from_dir (manager, dirname, loaded_files, suffix,
                                 load_file_func);
        g_free (dirname);
    }

    dirs = g_get_system_data_dirs ();
    for (datadir = *dirs; datadir != NULL; dirs++, datadir = *dirs)
    {
        dirname = g_build_filename (datadir, subdir, NULL);
        add_data_files_from_dir (manager, dirname, loaded_files, suffix,
                                 load_file_func);
        g_free (dirname);
    }

finish:
    file_list = g_hash_table_get_values (loaded_files);
    g_hash_table_unref (loaded_files);

    return file_list;
}

static inline GList *
_ag_providers_list (AgManager *self)
{
    return list_data_files (self, ".provider",
                            "AG_PROVIDERS", PROVIDER_FILES_DIR,
                            (AgDataFileLoadFunc)ag_manager_get_provider);
}

static inline GList *
_ag_services_list (AgManager *self)
{
    return list_data_files (self, ".service",
                            "AG_SERVICES", SERVICE_FILES_DIR,
                            (AgDataFileLoadFunc)ag_manager_get_service);
}

static GList *
get_account_services_from_accounts (AgManager *manager,
                                    GList *account_ids,
                                    gboolean enabled_only)
{
    GList *ret = NULL, *account_list;

    for (account_list = account_ids;
         account_list != NULL;
         account_list = account_list->next)
    {
        AgAccount *account;
        GList *service_ids, *service_elem;

        account = ag_manager_get_account (manager,
                                          (AgAccountId)
                                          GPOINTER_TO_INT(account_list->data));
        if (G_UNLIKELY (account == NULL))
            continue;

        service_ids = enabled_only ?
            ag_account_list_enabled_services (account) :
            ag_account_list_services (account);
        for (service_elem = service_ids;
             service_elem != NULL;
             service_elem = service_elem->next)
        {
            AgService *service = (AgService *) service_elem->data;
            AgAccountService *account_service;

            account_service = ag_account_service_new (account, service);
            if (G_UNLIKELY (account_service == NULL))
                continue;

            ret = g_list_prepend (ret, account_service);
        }

        ag_service_list_free (service_ids);
        g_object_unref (account);
    }

    return ret;
}

static void
set_error_from_db (AgManager *manager)
{
    AgManagerPrivate *priv = manager->priv;
    AgError code;
    GError *error;

    switch (sqlite3_errcode (priv->db))
    {
    case SQLITE_DONE:
    case SQLITE_OK:
        _ag_manager_take_error (manager, NULL);
        return;
    case SQLITE_BUSY:
        code = AG_ERROR_DB_LOCKED;
        if (priv->abort_on_db_timeout)
            g_error ("Accounts DB timeout: causing application to abort.");
        break;
    default:
        code = AG_ERROR_DB;
        break;
    }

    error = g_error_new (AG_ERRORS, code, "SQLite error %d: %s",
                         sqlite3_errcode (priv->db),
                         sqlite3_errmsg (priv->db));
    _ag_manager_take_error (manager, error);
}

static gboolean
timed_unref_account (gpointer account)
{
    DEBUG_REFS ("Releasing temporary reference on account %u",
                AG_ACCOUNT (account)->id);
    g_object_unref (account);
    return FALSE;
}

static gboolean
parse_message_header (DBusMessageIter *iter,
                      struct timespec *ts, AgAccountId *id,
                      gboolean *created, gboolean *deleted,
                      const gchar **provider_name)
{
#define EXPECT_TYPE(t) \
    if (G_UNLIKELY (dbus_message_iter_get_arg_type (iter) != t)) return FALSE

    EXPECT_TYPE (DBUS_TYPE_UINT32);
    dbus_message_iter_get_basic (iter, &ts->tv_sec);
    dbus_message_iter_next (iter);

    EXPECT_TYPE (DBUS_TYPE_UINT32);
    dbus_message_iter_get_basic (iter, &ts->tv_nsec);
    dbus_message_iter_next (iter);

    EXPECT_TYPE (DBUS_TYPE_UINT32);
    dbus_message_iter_get_basic (iter, id);
    dbus_message_iter_next (iter);

    EXPECT_TYPE (DBUS_TYPE_BOOLEAN);
    dbus_message_iter_get_basic (iter, created);
    dbus_message_iter_next (iter);

    EXPECT_TYPE (DBUS_TYPE_BOOLEAN);
    dbus_message_iter_get_basic (iter, deleted);
    dbus_message_iter_next (iter);

    EXPECT_TYPE (DBUS_TYPE_STRING);
    dbus_message_iter_get_basic (iter, provider_name);
    dbus_message_iter_next (iter);

#undef EXPECT_TYPE
    return TRUE;
}

static gboolean
ag_manager_must_emit_updated (AgManager *manager, AgAccountChanges *changes)
{
    AgManagerPrivate *priv = manager->priv;

    /* Don't emit the "updated" signal along with "created" or "deleted" */
    if (changes->created || changes->deleted)
        return FALSE;

    /* The update-event is emitted whenever any value has been changed on
     * particular service of account.
     */
    return (priv->service_type != NULL) ?
        _ag_account_changes_have_service_type (changes, priv->service_type) : FALSE;
}

static gboolean
ag_manager_must_emit_enabled (AgManager *manager, AgAccountChanges *changes)
{
    AgManagerPrivate *priv = manager->priv;

    /* TODO: the enabled-event is emitted whenever enabled status has changed on
     * any service or account. This has some possibility for optimization.
     */
    return (priv->service_type != NULL) ?
        _ag_account_changes_have_enabled (changes) : FALSE;
}

static void
ag_manager_emit_signals (AgManager *manager, AgAccountId account_id,
                         gboolean updated,
                         gboolean enabled,
                         gboolean created,
                         gboolean deleted)
{
    if (updated)
        g_signal_emit_by_name (manager, "account-updated", account_id);

    if (enabled)
        g_signal_emit_by_name (manager, "enabled-event", account_id);

    if (deleted)
        g_signal_emit_by_name (manager, "account-deleted", account_id);

    if (created)
        g_signal_emit_by_name (manager, "account-created", account_id);
}

static gboolean
check_signal_processed (AgManagerPrivate *priv, struct timespec *ts)
{
    ProcessedSignalData *psd;
    GList *list;

    for (list = priv->processed_signals; list != NULL; list = list->next)
    {
        psd = list->data;

        if (psd->ts.tv_sec == ts->tv_sec &&
            psd->ts.tv_nsec == ts->tv_nsec)
        {
            DEBUG_INFO ("Signal already processed: %lu-%lu",
                        ts->tv_sec, ts->tv_nsec);
            g_slice_free (ProcessedSignalData, psd);
            priv->processed_signals =
                g_list_delete_link (priv->processed_signals, list);
            return TRUE;
        }
    }

    /* Add the signal to the list of processed ones; this is necessary if the
     * manager was created for a specific service type, because in that case
     * we are subscribing for DBus signals on two different object paths (the
     * one for our service type, and one for the global settings), so we might
     * get notified about the same signal twice.
     */

    /* Don't keep more than a very few elements in the list */
    for (list = g_list_nth (priv->processed_signals, 2);
         list != NULL;
         list = g_list_nth (priv->processed_signals, 2))
    {
        priv->processed_signals = g_list_delete_link (priv->processed_signals,
                                                      list);
    }

    psd = g_slice_new (ProcessedSignalData);
    psd->ts = *ts;
    priv->processed_signals = g_list_prepend (priv->processed_signals, psd);

    return FALSE;
}

/*
 * checks whether the sender of the message is listed in the object_paths array
 */
static gboolean
message_is_from_interesting_object (DBusMessage *msg, GPtrArray *object_paths)
{
    const gchar *msg_object_path;
    gint i;

    msg_object_path = dbus_message_get_path (msg);
    if (G_UNLIKELY (msg_object_path == NULL))
        return FALSE;

    for (i = 0; i < object_paths->len; i++)
    {
        const gchar *object_path = g_ptr_array_index (object_paths, i);
        if (strcmp (msg_object_path, object_path) == 0)
            return TRUE;
    }
    return FALSE;
}

static DBusHandlerResult
dbus_filter_callback (DBusConnection *dbus_conn, DBusMessage *msg,
                      void *user_data)
{
    AgManager *manager = AG_MANAGER (user_data);
    AgManagerPrivate *priv = manager->priv;
    const gchar *provider_name = NULL;
    AgAccountId account_id = 0;
    AgAccount *account;
    AgAccountChanges *changes;
    struct timespec ts;
    gboolean deleted, created;
    gboolean ret;
    gboolean ours = FALSE;
    gboolean updated = FALSE;
    gboolean enabled = FALSE;
    gboolean must_instantiate = TRUE;
    DBusMessageIter iter;
    GList *list, *node;

    if (!dbus_message_is_signal (msg, AG_DBUS_IFACE, AG_DBUS_SIG_CHANGED))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (!message_is_from_interesting_object(msg, priv->object_paths))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    dbus_message_iter_init (msg, &iter);
    ret = parse_message_header (&iter, &ts, &account_id,
                                &created, &deleted, &provider_name);
    if (G_UNLIKELY (!ret))
    {
        g_warning ("%s: error in parsing signal arguments", G_STRFUNC);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DEBUG_INFO ("path = %s, time = %lu-%lu (%p)",
                dbus_message_get_path (msg), ts.tv_sec, ts.tv_nsec,
                manager);

    /* Do not process the same signal more than once. */
    if (check_signal_processed (priv, &ts))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    list = priv->emitted_signals;
    while (list != NULL)
    {
        EmittedSignalData *esd = list->data;
        node = list;
        list = list->next;

        if (esd->ts.tv_sec == ts.tv_sec &&
            esd->ts.tv_nsec == ts.tv_nsec)
        {
            gboolean must_process = esd->must_process;
            /* message is ours: we can ignore it, as the changes
             * were already processed when the DB transaction succeeded. */
            ours = TRUE;

            DEBUG_INFO ("Signal is ours, must_process = %d", esd->must_process);
            g_slice_free (EmittedSignalData, esd);
            priv->emitted_signals = g_list_delete_link (
                                                    priv->emitted_signals,
                                                    node);
            if (!must_process)
                return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
    }

    /* we must mark our emitted signals for reprocessing, because the current
     * signal might modify some of the fields that were previously modified by
     * us.
     * This ensures that changes coming from different account manager
     * instances are processed in the right order. */
    for (list = priv->emitted_signals; list != NULL; list = list->next)
    {
        EmittedSignalData *esd = list->data;
        DEBUG_INFO ("Marking pending signal for processing");
        esd->must_process = TRUE;
    }

    changes = _ag_account_changes_from_dbus (manager, &iter, created, deleted);

    /* check if the account is loaded */
    account = g_hash_table_lookup (priv->accounts,
                                   GUINT_TO_POINTER (account_id));

    if (!account && !created && !deleted)
        must_instantiate = FALSE;

    if (ours && (deleted || created))
        must_instantiate = FALSE;

    if (!account && must_instantiate)
    {
        /* because of the checks above, this can happen if this is an account
         * created or deleted from another instance.
         * We must emit the signals, and cache the newly created account for a
         * while, because the application is likely to inspect it */
        account = g_object_new (AG_TYPE_ACCOUNT,
                                "manager", manager,
                                "provider", provider_name,
                                "id", account_id,
                                "foreign", created,
                                NULL);
        g_return_val_if_fail (AG_IS_ACCOUNT (account),
                              DBUS_HANDLER_RESULT_NOT_YET_HANDLED);

        g_object_weak_ref (G_OBJECT (account), account_weak_notify, manager);
        g_hash_table_insert (priv->accounts, GUINT_TO_POINTER (account_id),
                             account);
        g_timeout_add_seconds (2, timed_unref_account, account);
    }

    if (changes)
    {
        updated = ag_manager_must_emit_updated (manager, changes);
        enabled = ag_manager_must_emit_enabled (manager, changes);
        if (account)
            _ag_account_done_changes (account, changes);

        _ag_account_changes_free (changes);
    }

    ag_manager_emit_signals (manager, account_id,
                             updated,
                             enabled,
                             created,
                             deleted);

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusMessage *
make_signal_for_service_type (DBusMessage *global_msg,
                              const gchar *service_type)
{
    gchar path[256];
    gchar *escaped_type;
    DBusMessage *msg;

    escaped_type = _ag_dbus_escape_as_identifier (service_type);
    g_snprintf (path, sizeof (path), "%s/%s",
                AG_DBUS_PATH_SERVICE, escaped_type);
    g_free (escaped_type);

    msg = dbus_message_copy (global_msg);
    DEBUG_INFO("Setting path to %s", path);
    if (!dbus_message_set_path (msg, path))
        g_warning("setting path failed!");
    return msg;
}

static void
signal_account_changes_on_service_types (AgManager *manager,
                                         AgAccountChanges *changes,
                                         DBusMessage *global_msg)
{
    GPtrArray *service_types;
    gint i;

    service_types = _ag_account_changes_get_service_types (changes);
    for (i = 0; i < service_types->len; i++)
    {
        const gchar *service_type;
        DBusMessage *msg;
        gboolean ret;

        service_type = g_ptr_array_index(service_types, i);
        msg = make_signal_for_service_type (global_msg, service_type);

        ret = dbus_connection_send (manager->priv->dbus_conn, msg, NULL);
        if (G_UNLIKELY (!ret))
            g_warning ("Emission of DBus signal failed");
        dbus_message_unref (msg);
    }
    g_ptr_array_free (service_types, TRUE);
}

static void
signal_account_changes (AgManager *manager, AgAccount *account,
                        AgAccountChanges *changes)
{
    AgManagerPrivate *priv = manager->priv;
    DBusMessage *msg;
    gboolean ret;
    EmittedSignalData eds;

    clock_gettime(CLOCK_MONOTONIC, &eds.ts);

    msg = _ag_account_build_signal (account, changes, &eds.ts);
    if (G_UNLIKELY (!msg))
    {
        g_warning ("Creation of D-Bus signal failed");
        return;
    }

    ret = dbus_connection_send (priv->dbus_conn, msg, NULL);
    if (G_UNLIKELY (!ret))
    {
        g_warning ("Emission of DBus signal failed");
        goto finish;
    }

    /* emit the signal on all service-types */
    signal_account_changes_on_service_types(manager, changes, msg);

    dbus_connection_flush (priv->dbus_conn);
    DEBUG_INFO ("Emitted signal, time: %lu-%lu", eds.ts.tv_sec, eds.ts.tv_nsec);

    eds.must_process = FALSE;
    priv->emitted_signals =
        g_list_prepend (priv->emitted_signals,
                        g_slice_dup (EmittedSignalData, &eds));

finish:
    dbus_message_unref (msg);
}

static gboolean
got_service (sqlite3_stmt *stmt, AgService **p_service)
{
    AgService *service;

    g_assert (p_service != NULL);

    service = _ag_service_new ();
    service->id = sqlite3_column_int (stmt, 0);
    service->display_name = g_strdup ((gchar *)sqlite3_column_text (stmt, 1));
    service->provider = g_strdup ((gchar *)sqlite3_column_text (stmt, 2));
    service->type = g_strdup ((gchar *)sqlite3_column_text (stmt, 3));

    *p_service = service;
    return TRUE;
}

static gboolean
got_service_id (sqlite3_stmt *stmt, AgService *service)
{
    g_assert (service != NULL);

    service->id = sqlite3_column_int (stmt, 0);
    return TRUE;
}

static gboolean
add_service_to_db (AgManager *manager, AgService *service)
{
    gchar *sql;

    /* Add the service to the DB */
    sql = sqlite3_mprintf ("INSERT INTO Services "
                           "(name, display, provider, type) "
                           "VALUES (%Q, %Q, %Q, %Q);",
                           service->name,
                           service->display_name,
                           service->provider,
                           service->type);
    _ag_manager_exec_query (manager, NULL, NULL, sql);
    sqlite3_free (sql);

    /* The insert statement above might fail in the unlikely case
     * that in the meantime the same service was inserted by some other
     * process; so, instead of calling sqlite3_last_insert_rowid(), we
     * just get the ID with another query. */
    sql = sqlite3_mprintf ("SELECT id FROM Services WHERE name = %Q",
                           service->name);
    _ag_manager_exec_query (manager, (AgQueryCallback)got_service_id,
                            service, sql);
    sqlite3_free (sql);

    return service->id != 0;
}

static gboolean
add_id_to_list (sqlite3_stmt *stmt, GList **plist)
{
    gint id;

    id = sqlite3_column_int (stmt, 0);
    *plist = g_list_prepend (*plist, GINT_TO_POINTER (id));
    return TRUE;
}

static void
account_weak_notify (gpointer userdata, GObject *dead_account)
{
    AgManagerPrivate *priv = AG_MANAGER_PRIV (userdata);
    GHashTableIter iter;
    GObject *account;

    DEBUG_REFS ("called for %p", dead_account);
    g_hash_table_iter_init (&iter, priv->accounts);
    while (g_hash_table_iter_next (&iter, NULL, (gpointer)&account))
    {
        if (account == dead_account)
        {
            g_hash_table_iter_steal (&iter);
            break;
        }
    }
}

static void
account_weak_unref (GObject *account)
{
    g_object_weak_unref (account, account_weak_notify,
                         ag_account_get_manager (AG_ACCOUNT (account)));
}

/*
 * exec_transaction:
 *
 * Executes a transaction, assuming that the exclusive lock has been obtained.
 */
static void
exec_transaction (AgManager *manager, AgAccount *account,
                  const gchar *sql, AgAccountChanges *changes,
                  GError **error)
{
    AgManagerPrivate *priv;
    gchar *err_msg = NULL;
    int ret;
    gboolean updated, enabled;

    DEBUG_LOCKS ("Accounts DB is now locked");
    DEBUG_QUERIES ("called: %s", sql);
    g_return_if_fail (AG_IS_MANAGER (manager));
    priv = manager->priv;
    g_return_if_fail (AG_IS_ACCOUNT (account));
    g_return_if_fail (sql != NULL);
    g_return_if_fail (priv->db != NULL);

    ret = sqlite3_exec (priv->db, sql, NULL, NULL, &err_msg);
    if (G_UNLIKELY (ret != SQLITE_OK))
    {
        *error = g_error_new (AG_ERRORS, AG_ERROR_DB, "%s", err_msg);
        if (err_msg)
            sqlite3_free (err_msg);

        ret = sqlite3_step (priv->rollback_stmt);
        if (G_UNLIKELY (ret != SQLITE_OK))
            g_warning ("Rollback failed");
        sqlite3_reset (priv->rollback_stmt);
        DEBUG_LOCKS ("Accounts DB is now unlocked");
        return;
    }

    ret = sqlite3_step (priv->commit_stmt);
    if (G_UNLIKELY (ret != SQLITE_DONE))
    {
        *error = g_error_new_literal (AG_ERRORS, AG_ERROR_DB,
                                      sqlite3_errmsg (priv->db));
        sqlite3_reset (priv->commit_stmt);
        return;
    }
    sqlite3_reset (priv->commit_stmt);

    DEBUG_LOCKS ("Accounts DB is now unlocked");

    /* everything went well; if this was a new account, we must update the
     * local data structure */
    if (account->id == 0)
    {
        account->id = priv->last_account_id;

        /* insert the account into our cache */
        g_object_weak_ref (G_OBJECT (account), account_weak_notify, manager);
        g_hash_table_insert (priv->accounts, GUINT_TO_POINTER (account->id),
                             account);
    }

    /* emit DBus signals to notify other processes */
    signal_account_changes (manager, account, changes);

    updated = ag_manager_must_emit_updated(manager, changes);

    enabled = ag_manager_must_emit_enabled(manager, changes);
    _ag_account_done_changes (account, changes);

    ag_manager_emit_signals (manager, account->id,
                             updated,
                             enabled,
                             changes->created,
                             changes->deleted);
}

static void
lost_weak_ref (gpointer data, GObject *dead)
{
    StoreCbData *sd = data;
    AgManagerPrivate *priv;

    GError error = { AG_ERRORS, AG_ERROR_DISPOSED, "Account disposed" };

    g_assert ((GObject *)sd->account == dead);
    _ag_account_store_completed (sd->account, sd->changes,
                                 sd->callback, &error, sd->user_data);

    priv = AG_MANAGER_PRIV (sd->manager);
    priv->locks = g_list_remove (priv->locks, sd);
    sd->account = NULL; /* so that the weak reference is not removed */
    store_cb_data_free (sd);
}

static void
store_cb_data_free (StoreCbData *sd)
{
    if (sd->account)
        g_object_weak_unref (G_OBJECT (sd->account), lost_weak_ref, sd);
    if (sd->id)
        g_source_remove (sd->id);
    g_free (sd->sql);
    g_slice_free (StoreCbData, sd);
}

static gboolean
exec_transaction_idle (StoreCbData *sd)
{
    AgManager *manager = sd->manager;
    AgAccount *account = sd->account;
    AgManagerPrivate *priv;
    GError *error = NULL;
    int ret;

    g_return_val_if_fail (AG_IS_MANAGER (manager), FALSE);
    priv = manager->priv;

    g_return_val_if_fail (priv->begin_stmt != NULL, FALSE);
    ret = sqlite3_step (priv->begin_stmt);
    if (ret == SQLITE_BUSY)
    {
        sched_yield ();
        return TRUE; /* call this callback again */
    }

    g_object_ref (manager);
    g_object_ref (account);
    if (ret == SQLITE_DONE)
    {
        exec_transaction (manager, account, sd->sql, sd->changes, &error);
    }
    else
    {
        error = g_error_new_literal (AG_ERRORS, AG_ERROR_DB, "Generic error");
    }
    _ag_account_store_completed (account, sd->changes,
                                 sd->callback, error, sd->user_data);
    if (error)
        g_error_free (error);

    priv->locks = g_list_remove (priv->locks, sd);
    sd->id = 0;
    store_cb_data_free (sd);
    g_object_unref (account);
    g_object_unref (manager);
    return FALSE;
}

static int
prepare_transaction_statements (AgManagerPrivate *priv)
{
    int ret;

    if (G_UNLIKELY (!priv->begin_stmt))
    {
        ret = sqlite3_prepare_v2 (priv->db, "BEGIN EXCLUSIVE;", -1,
                                  &priv->begin_stmt, NULL);
        if (ret != SQLITE_OK) return ret;
    }
    else
        sqlite3_reset (priv->begin_stmt);

    if (G_UNLIKELY (!priv->commit_stmt))
    {
        ret = sqlite3_prepare_v2 (priv->db, "COMMIT;", -1,
                                  &priv->commit_stmt, NULL);
        if (ret != SQLITE_OK) return ret;
    }
    else
        sqlite3_reset (priv->commit_stmt);

    if (G_UNLIKELY (!priv->rollback_stmt))
    {
        ret = sqlite3_prepare_v2 (priv->db, "ROLLBACK;", -1,
                                  &priv->rollback_stmt, NULL);
        if (ret != SQLITE_OK) return ret;
    }
    else
        sqlite3_reset (priv->rollback_stmt);

    return SQLITE_OK;
}

static void
set_last_rowid_as_account_id (sqlite3_context *ctx,
                              int argc, sqlite3_value **argv)
{
    AgManagerPrivate *priv;

    priv = sqlite3_user_data (ctx);
    priv->last_account_id = sqlite3_last_insert_rowid (priv->db);
    sqlite3_result_null (ctx);
}

static void
get_account_id (sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    AgManagerPrivate *priv;

    priv = sqlite3_user_data (ctx);
    sqlite3_result_int64 (ctx, priv->last_account_id);
}

static void
create_functions (AgManagerPrivate *priv)
{
    sqlite3_create_function (priv->db, "set_last_rowid_as_account_id", 0,
                             SQLITE_ANY, priv,
                             set_last_rowid_as_account_id, NULL, NULL);
    sqlite3_create_function (priv->db, "account_id", 0,
                             SQLITE_ANY, priv,
                             get_account_id, NULL, NULL);
}

static void
setup_db_options (sqlite3 *db)
{
    gchar *error;
    int ret;

    error = NULL;
    ret = sqlite3_exec (db, "PRAGMA synchronous = 1", NULL, NULL, &error);
    if (ret != SQLITE_OK)
    {
        g_warning ("%s: couldn't set synchronous mode (%s)",
                   G_STRFUNC, error);
        sqlite3_free (error);
    }

    error = NULL;
    ret = sqlite3_exec (db, "PRAGMA journal_mode = WAL", NULL, NULL, &error);
    if (ret != SQLITE_OK)
    {
        g_warning ("%s: couldn't set journal mode to WAL (%s)",
                   G_STRFUNC, error);
        sqlite3_free (error);
    }
}

static gint
get_db_version (sqlite3 *db)
{
    sqlite3_stmt *stmt;
    gint version = 0, ret;

    ret = sqlite3_prepare(db, "PRAGMA user_version", -1, &stmt, NULL);
    if (G_UNLIKELY(ret != SQLITE_OK)) return 0;

    ret = sqlite3_step(stmt);
    if (G_LIKELY(ret == SQLITE_ROW))
        version = sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);
    return version;
}

static gboolean
create_db (sqlite3 *db, guint timeout)
{
    const gchar *sql;
    gchar *error;
    int ret;

    sql = ""
        "CREATE TABLE IF NOT EXISTS Accounts ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT,"
            "provider TEXT,"
            "enabled INTEGER);"

        "CREATE TABLE IF NOT EXISTS Services ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT NOT NULL UNIQUE,"
            "display TEXT NOT NULL,"
            /* following fields are included for performance reasons */
            "provider TEXT,"
            "type TEXT);"
        "CREATE INDEX IF NOT EXISTS idx_service ON Services(name);"

        "CREATE TABLE IF NOT EXISTS Settings ("
            "account INTEGER NOT NULL,"
            "service INTEGER,"
            "key TEXT NOT NULL,"
            "type TEXT NOT NULL,"
            "value BLOB);"
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_setting ON Settings "
            "(account, service, key);"

        "CREATE TRIGGER IF NOT EXISTS tg_delete_account "
            "BEFORE DELETE ON Accounts FOR EACH ROW BEGIN "
                "DELETE FROM Settings WHERE account = OLD.id; "
            "END;"

        "CREATE TABLE IF NOT EXISTS Signatures ("
            "account INTEGER NOT NULL,"
            "service INTEGER,"
            "key TEXT NOT NULL,"
            "signature TEXT NOT NULL,"
            "token TEXT NOT NULL);"
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_signatures ON Signatures "
           "(account, service, key);"

        "PRAGMA user_version = 1;";

    error = NULL;
    ret = sqlite3_exec (db, sql, NULL, NULL, &error);
    if (ret == SQLITE_BUSY)
    {
        guint t;
        for (t = 5; t < MAX_SQLITE_BUSY_LOOP_TIME_MS; t *= 2)
        {
            DEBUG_LOCKS ("Database locked, retrying...");
            sched_yield ();
            g_assert(error != NULL);
            sqlite3_free (error);
            ret = sqlite3_exec (db, sql, NULL, NULL, &error);
            if (ret != SQLITE_BUSY) break;
            usleep(t * 1000);
        }
    }

    if (ret != SQLITE_OK)
    {
        g_warning ("Error initializing DB: %s", error);
        sqlite3_free (error);
        return FALSE;
    }

    return TRUE;
}

static gboolean
open_db (AgManager *manager)
{
    AgManagerPrivate *priv = manager->priv;
    const gchar *basedir;
    gchar *filename, *pathname;
    gint version;
    gboolean ok = TRUE;
    int ret;

    basedir = g_getenv ("ACCOUNTS");
    if (G_LIKELY (!basedir))
    {
        basedir = g_get_home_dir ();
        pathname = g_build_path (G_DIR_SEPARATOR_S, basedir,
            DATABASE_DIR, NULL);
        if (G_UNLIKELY (g_mkdir_with_parents(pathname, 0755)))
            g_warning ("Cannot create directory: %s", pathname);
        filename = g_build_filename (pathname, "accounts.db", NULL);
        g_free (pathname);
    }
    else
    {
        filename = g_build_filename (basedir, "accounts.db", NULL);
    }
    ret = sqlite3_open (filename, &priv->db);
    g_free (filename);

    if (ret != SQLITE_OK)
    {
        if (priv->db)
        {
            g_warning ("Error opening accounts DB: %s",
                       sqlite3_errmsg (priv->db));
            sqlite3_close (priv->db);
            priv->db = NULL;
        }
        return FALSE;
    }

    /* TODO: busy handler */

    version = get_db_version(priv->db);
    DEBUG_INFO ("DB version: %d", version);
    if (version < 1)
        ok = create_db(priv->db, priv->db_timeout);
    /* insert here code to upgrade the DB from older versions... */

    if (G_UNLIKELY (!ok))
    {
        sqlite3_close (priv->db);
        priv->db = NULL;
        return FALSE;
    }

    setup_db_options (priv->db);
    create_functions (priv);

    return TRUE;
}

static gboolean
add_matches (AgManagerPrivate *priv)
{
    gchar match[DBUS_MAXIMUM_MATCH_RULE_LENGTH];
    DBusError error;
    gint i;

    dbus_error_init (&error);
    for (i = 0; i < priv->object_paths->len; i++)
    {
        const gchar *path = g_ptr_array_index(priv->object_paths, i);

        g_snprintf (match, sizeof (match),
                    "type='signal',interface='" AG_DBUS_IFACE "',path='%s'",
                    path);
        dbus_bus_add_match (priv->dbus_conn, match, &error);
        if (G_UNLIKELY (dbus_error_is_set (&error)))
        {
            g_warning ("Failed to add dbus filter (%s)", error.message);
            dbus_error_free (&error);
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
setup_dbus (AgManager *manager)
{
    AgManagerPrivate *priv = manager->priv;
    DBusError error;
    gboolean ret;

    dbus_error_init (&error);
    priv->dbus_conn = dbus_bus_get (DBUS_BUS_SESSION, &error);
    if (G_UNLIKELY (dbus_error_is_set (&error)))
    {
        g_warning ("Failed to get D-Bus connection (%s)", error.message);
        dbus_error_free (&error);
        return FALSE;
    }

    ret = dbus_connection_add_filter (priv->dbus_conn,
                                      dbus_filter_callback,
                                      manager, NULL);
    if (G_UNLIKELY (!ret))
    {
        g_warning ("Failed to add dbus filter");
        return FALSE;
    }

    if (priv->service_type == NULL)
    {
        /* listen to all changes */
        g_ptr_array_add (priv->object_paths, g_strdup (AG_DBUS_PATH));
    }
    else
    {
        gchar *escaped_type, *path;

        /* listen for changes on our service type only */
        escaped_type = _ag_dbus_escape_as_identifier (priv->service_type);
        path = g_strdup_printf (AG_DBUS_PATH_SERVICE "/%s", escaped_type);
        g_free (escaped_type);
        g_ptr_array_add (priv->object_paths, path);

        /* add also the global service type */
        g_ptr_array_add (priv->object_paths,
                         g_strdup (AG_DBUS_PATH_SERVICE_GLOBAL));
    }

    ret = add_matches(priv);
    if (G_UNLIKELY (!ret)) return FALSE;

    dbus_connection_setup_with_g_main (priv->dbus_conn, NULL);
    return TRUE;
}

static void
ag_manager_init (AgManager *manager)
{
    AgManagerPrivate *priv;

    manager->priv = G_TYPE_INSTANCE_GET_PRIVATE (manager, AG_TYPE_MANAGER,
                                                 AgManagerPrivate);
    priv = manager->priv;

    priv->services =
        g_hash_table_new_full (g_str_hash, g_str_equal,
                               NULL, (GDestroyNotify)ag_service_unref);
    priv->accounts =
        g_hash_table_new_full (NULL, NULL,
                               NULL, (GDestroyNotify)account_weak_unref);

    priv->db_timeout = MAX_SQLITE_BUSY_LOOP_TIME_MS; /* 5 seconds */

    priv->object_paths = g_ptr_array_new_with_free_func (g_free);
}

static GObject *
ag_manager_constructor (GType type, guint n_params,
                        GObjectConstructParam *params)
{
    GObjectClass *object_class = (GObjectClass *)ag_manager_parent_class;
    AgManager *manager;
    GObject *object;

    object = object_class->constructor (type, n_params, params);

    g_return_val_if_fail (object != NULL, NULL);

    manager = AG_MANAGER (object);
    if (G_UNLIKELY (!open_db (manager) || !setup_dbus (manager)))
    {
        g_object_unref (object);
        return NULL;
    }

    return object;
}

static void
ag_manager_set_property (GObject *object, guint property_id,
                         const GValue *value, GParamSpec *pspec)
{
    AgManager *manager = AG_MANAGER (object);
    AgManagerPrivate *priv = manager->priv;

    switch (property_id)
    {
    case PROP_SERVICE_TYPE:
        g_assert (priv->service_type == NULL);
        priv->service_type = g_value_dup_string (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
ag_manager_dispose (GObject *object)
{
    AgManagerPrivate *priv = AG_MANAGER_PRIV (object);

    if (priv->is_disposed) return;
    priv->is_disposed = TRUE;

    DEBUG_REFS ("Disposing manager %p", object);

    while (priv->locks)
    {
        store_cb_data_free (priv->locks->data);
        priv->locks = g_list_delete_link (priv->locks, priv->locks);
    }

    G_OBJECT_CLASS (ag_manager_parent_class)->finalize (object);
}

static void
ag_manager_finalize (GObject *object)
{
    AgManagerPrivate *priv = AG_MANAGER_PRIV (object);

    if (priv->dbus_conn)
    {
        dbus_connection_remove_filter (priv->dbus_conn, dbus_filter_callback,
                                       object);
        dbus_connection_unref (priv->dbus_conn);
    }

    g_ptr_array_free (priv->object_paths, TRUE);

    while (priv->emitted_signals)
    {
        g_slice_free (EmittedSignalData, priv->emitted_signals->data);
        priv->emitted_signals = g_list_delete_link (priv->emitted_signals,
                                                    priv->emitted_signals);
    }

    while (priv->processed_signals)
    {
        g_slice_free (ProcessedSignalData, priv->processed_signals->data);
        priv->processed_signals = g_list_delete_link (priv->processed_signals,
                                                      priv->processed_signals);
    }

    if (priv->begin_stmt)
        sqlite3_finalize (priv->begin_stmt);
    if (priv->commit_stmt)
        sqlite3_finalize (priv->commit_stmt);
    if (priv->rollback_stmt)
        sqlite3_finalize (priv->rollback_stmt);

    if (priv->services)
        g_hash_table_unref (priv->services);

    if (priv->accounts)
        g_hash_table_unref (priv->accounts);

    if (priv->db)
    {
        if (sqlite3_close (priv->db) != SQLITE_OK)
            g_warning ("Failed to close database: %s",
                       sqlite3_errmsg (priv->db));
        priv->db = NULL;
    }
    g_free (priv->service_type);

    if (priv->last_error)
        g_error_free (priv->last_error);

    G_OBJECT_CLASS (ag_manager_parent_class)->finalize (object);
}

static void
ag_manager_account_deleted (AgManager *manager, AgAccountId id)
{
    g_return_if_fail (AG_IS_MANAGER (manager));

    /* The weak reference is removed automatically when the account is removed
     * from the hash table */
    g_hash_table_remove (manager->priv->accounts, GUINT_TO_POINTER (id));
}

static void
ag_manager_class_init (AgManagerClass *klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (AgManagerPrivate));

    klass->account_deleted = ag_manager_account_deleted;
    object_class->constructor = ag_manager_constructor;
    object_class->dispose = ag_manager_dispose;
    object_class->set_property = ag_manager_set_property;
    object_class->finalize = ag_manager_finalize;

    g_object_class_install_property
        (object_class, PROP_SERVICE_TYPE,
         g_param_spec_string ("service-type", "service type", "Set service type",
                              NULL,
                              G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    /**
     * AgManager::account-created:
     * @manager: the #AgManager.
     * @account_id: the #AgAccountId of the account that has been created.
     *
     * Emitted when a new account has been created; note that the account must
     * have been stored in the database: the signal is not emitted just in
     * response to ag_manager_create_account().
     */
    signals[ACCOUNT_CREATED] = g_signal_new ("account-created",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0,
        NULL, NULL,
        g_cclosure_marshal_VOID__UINT,
        G_TYPE_NONE,
        1, G_TYPE_UINT);

    /**
     * AgManager::enabled-event:
     * @manager: the #AgManager.
     * @account_id: the #AgAccountId of the account that has been enabled.
     *
     * If the manager has been created with ag_manager_new_for_service_type(), this
     * signal will be emitted when an account (identified by @account_id) has been
     * modified in such a way that the application might be interested to start/stop
     * using it: the "enabled" flag on the account or in some service supported by the
     * account and matching the #AgManager:service-type have changed.
     * In practice, this signal might be emitted more often than when strictly needed;
     * applications must call ag_account_list_enabled_services() or
     * ag_manager_list_enabled() to get the current state.
     */
    signals[ACCOUNT_ENABLED] = g_signal_new ("enabled-event",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0,
        NULL, NULL,
        g_cclosure_marshal_VOID__UINT,
        G_TYPE_NONE,
        1, G_TYPE_UINT);

    /**
     * AgManager::account-deleted:
     * @manager: the #AgManager.
     * @account_id: the #AgAccountId of the account that has been deleted.
     *
     * Emitted when an account has been deleted.
     * This signal is redundant with AgAccount::deleted, but it's convenient to
     * provide full change notification to #AgManager.
     */
    signals[ACCOUNT_DELETED] = g_signal_new ("account-deleted",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (AgManagerClass, account_deleted),
        NULL, NULL,
        g_cclosure_marshal_VOID__UINT,
        G_TYPE_NONE,
        1, G_TYPE_UINT);

    /**
     * AgManager::account-updated:
     * @manager: the #AgManager.
     * @account_id: the #AgAccountId of the account that has been update.
     *
     * Emitted when particular service of an account has been updated.
     * This signal is redundant with AgAccount::deleted, but it's convenient to
     * provide full change notification to #AgManager.
     */
    signals[ACCOUNT_UPDATED] = g_signal_new ("account-updated",
         G_TYPE_FROM_CLASS (klass),
         G_SIGNAL_RUN_LAST,
         0,
         NULL, NULL,
         g_cclosure_marshal_VOID__UINT,
         G_TYPE_NONE,
         1, G_TYPE_UINT);

    _ag_debug_init();
}

/**
 * ag_manager_new:
 *
 * Create a new #AgManager.
 *
 * Returns: an instance of an #AgManager.
 */
AgManager *
ag_manager_new ()
{
    return g_object_new (AG_TYPE_MANAGER, NULL);
}

GList *
_ag_manager_list_all (AgManager *manager)
{
    GList *list = NULL;
    const gchar *sql;

    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);
    sql = "SELECT id FROM Accounts;";
    _ag_manager_exec_query (manager, (AgQueryCallback)add_id_to_list,
                            &list, sql);
    return list;
}

void
_ag_manager_take_error (AgManager *manager, GError *error)
{
    AgManagerPrivate *priv;

    g_return_if_fail (AG_IS_MANAGER (manager));
    priv = manager->priv;

    if (priv->last_error)
        g_error_free (priv->last_error);
    priv->last_error = error;
}

const GError *
_ag_manager_get_last_error (AgManager *manager)
{
    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);

    return manager->priv->last_error;
}

/**
 * ag_manager_list:
 * @manager: the #AgManager.
 *
 * Lists the accounts. If the #AgManager is created with specified service_type
 * it will return only the accounts supporting this service_type.
 *
 * Returns: (transfer full) (element-type AgAccountId): a #GList of
 * #AgAccountId representing the accounts. Must be free'd with
 * ag_manager_list_free().
 */
GList *
ag_manager_list (AgManager *manager)
{
    AgManagerPrivate *priv;

    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);
    priv = manager->priv;

    if (priv->service_type)
        return ag_manager_list_by_service_type (manager, priv->service_type);

    return _ag_manager_list_all (manager);
}

/**
 * ag_manager_list_by_service_type:
 * @manager: the #AgManager.
 * @service_type: the name of the service type to check for.
 *
 * Lists the accounts supporting the given service type.
 *
 * Returns: (transfer full) (element-type AgAccountId): a #GList of
 * #AgAccountId representing the accounts. Must be free'd with
 * ag_manager_list_free().
 */
GList *
ag_manager_list_by_service_type (AgManager *manager,
                                 const gchar *service_type)
{
    GList *list = NULL;
    char sql[512];

    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);
    sqlite3_snprintf (sizeof (sql), sql,
                      "SELECT DISTINCT account FROM Settings "
                      "JOIN Services ON Settings.service = Services.id "
                      "WHERE Services.type = %Q;",
                      service_type);
    _ag_manager_exec_query (manager, (AgQueryCallback)add_id_to_list,
                            &list, sql);
    return list;
}

/**
 * ag_manager_list_enabled:
 * @manager: the #AgManager.
 *
 * Lists the enabled accounts.
 *
 * Returns: (transfer full) (element-type AgAccountId): a #GList of the enabled
 * #AgAccountId representing the accounts. Must be free'd with
 * ag_manager_list_free().
 */
GList *
ag_manager_list_enabled (AgManager *manager)
{
    GList *list = NULL;
    char sql[512];
    AgManagerPrivate *priv;

    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);
    priv = manager->priv;

    if (priv->service_type == NULL)
    {
        sqlite3_snprintf (sizeof (sql), sql,
                          "SELECT id FROM Accounts WHERE enabled=1;");
        _ag_manager_exec_query (manager, (AgQueryCallback)add_id_to_list,
                                &list, sql);
    }
    else
    {
        list = ag_manager_list_enabled_by_service_type(manager, priv->service_type);
    }
    return list;
}

/**
 * ag_manager_list_enabled_by_service_type:
 * @manager: the #AgManager.
 * @service_type: the name of the service type to check for.
 *
 * Lists the enabled accounts supporting the given service type.
 *
 * Returns: (transfer full) (element-type AgAccountId): a #GList of the enabled
 * #AgAccountId representing the accounts. Must be free'd with
 * ag_manager_list_free().
 */
GList *
ag_manager_list_enabled_by_service_type (AgManager *manager,
                                         const gchar *service_type)
{
    GList *list = NULL;
    char sql[512];

    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);
    g_return_val_if_fail (service_type != NULL, NULL);
    sqlite3_snprintf (sizeof (sql), sql,
                      "SELECT Settings.account FROM Settings "
                      "INNER JOIN Services ON Settings.service = Services.id "
                      "WHERE Settings.key='enabled' AND Settings.value='true' "
                      "AND Services.type = %Q AND Settings.account IN "
                      "(SELECT id FROM Accounts WHERE enabled=1);",
                      service_type);
    _ag_manager_exec_query (manager, (AgQueryCallback)add_id_to_list,
                            &list, sql);
    return list;
}

/**
 * ag_manager_list_free:
 * @list: (element-type AgAccountId): a #GList returned from some #AgManager
 * method.
 *
 * Frees the memory taken by a #GList allocated by #AgManager.
 */
void
ag_manager_list_free (GList *list)
{
    g_list_free (list);
}

/**
 * ag_manager_get_enabled_account_services:
 * @manager: the #AgManager.
 *
 * Gets all the enabled account services. If the @manager was created for a
 * specific service type, only services with that type will be returned.
 * <note>
 *   <para>
 *   This method causes the loading of all the service settings for all the
 *   returned accounts (unless they have been loaded previously). If you are
 *   interested in a specific account/service, consider using
 *   ag_manager_load_account() to first load the the account, and then create
 *   the AgAccountService for that account only.
 *   </para>
 * </note>
 *
 * Returns: (transfer full) (element-type AgAccountService): a list of
 * #AgAccountService objects. When done with it, call g_object_unref() on the
 * list elements.
 */
GList *
ag_manager_get_enabled_account_services (AgManager *manager)
{
    GList *account_ids, *account_services;

    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);

    account_ids = ag_manager_list_enabled (manager);
    account_services = get_account_services_from_accounts (manager,
                                                           account_ids,
                                                           TRUE);
    ag_manager_list_free (account_ids);
    return account_services;
}

/**
 * ag_manager_get_account_services:
 * @manager: the #AgManager.
 *
 * Gets all the account services. If the @manager was created for a
 * specific service type, only services with that type will be returned.
 * <note>
 *   <para>
 *   This method causes the loading of all the service settings for all the
 *   returned accounts (unless they have been loaded previously). If you are
 *   interested in a specific account/service, consider using
 *   ag_manager_load_account() to first load the the account, and then create
 *   the AgAccountService for that account only.
 *   </para>
 * </note>
 *
 * Returns: (transfer full) (element-type AgAccountService): a list of
 * #AgAccountService objects. When done with it, call g_object_unref() on the
 * list elements.
 */
GList *
ag_manager_get_account_services (AgManager *manager)
{
    GList *account_ids, *account_services;

    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);

    account_ids = ag_manager_list (manager);
    account_services = get_account_services_from_accounts (manager,
                                                           account_ids,
                                                           FALSE);
    ag_manager_list_free (account_ids);
    return account_services;
}

/**
 * ag_manager_get_account:
 * @manager: the #AgManager.
 * @account_id: the #AgAccountId of the account.
 *
 * Instantiates the object representing the account identified by
 * @account_id.
 *
 * Returns: (transfer full): an #AgAccount, on which the client must call
 * g_object_unref() when it's done with it, or %NULL if an error occurs.
 */
AgAccount *
ag_manager_get_account (AgManager *manager, AgAccountId account_id)
{
    return ag_manager_load_account (manager, account_id, NULL);
}

/**
 * ag_manager_load_account:
 * @manager: the #AgManager.
 * @account_id: the #AgAccountId of the account.
 * @error: pointer to a #GError, or %NULL.
 *
 * Instantiates the object representing the account identified by
 * @account_id.
 *
 * Returns: (transfer full): an #AgAccount, on which the client must call
 * g_object_unref() when it's done with it, or %NULL if an error occurs.
 */
AgAccount *
ag_manager_load_account (AgManager *manager, AgAccountId account_id,
                         GError **error)
{
    AgManagerPrivate *priv;
    AgAccount *account;

    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);
    g_return_val_if_fail (account_id != 0, NULL);
    priv = manager->priv;

    account = g_hash_table_lookup (priv->accounts,
                                   GUINT_TO_POINTER (account_id));
    if (account)
        return g_object_ref (account);

    /* the account is not loaded; do it now */
    account = g_object_new (AG_TYPE_ACCOUNT,
                            "manager", manager,
                            "id", account_id,
                            NULL);
    if (G_LIKELY (account))
    {
        g_object_weak_ref (G_OBJECT (account), account_weak_notify, manager);
        g_hash_table_insert (priv->accounts, GUINT_TO_POINTER (account_id),
                             account);
    }
    else if (priv->last_error != NULL)
    {
        g_set_error_literal (error,
                             priv->last_error->domain,
                             priv->last_error->code,
                             priv->last_error->message);
    }
    return account;
}

/**
 * ag_manager_create_account:
 * @manager: the #AgManager.
 * @provider_name: name of the provider of the account.
 *
 * Create a new account. The account is not stored in the database until
 * ag_account_store() has successfully returned; the @id field in the
 * #AgAccount structure is also not meant to be valid till the account has been
 * stored.
 *
 * Returns: (transfer full): a new #AgAccount.
 */
AgAccount *
ag_manager_create_account (AgManager *manager, const gchar *provider_name)
{
    AgAccount *account;

    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);

    account = g_object_new (AG_TYPE_ACCOUNT,
                            "manager", manager,
                            "provider", provider_name,
                            NULL);
    return account;
}

/* This is called when creating AgService objects from inside the DBus
 * handler: we don't want to access the Db from there */
AgService *
_ag_manager_get_service_lazy (AgManager *manager, const gchar *service_name,
                              const gchar *service_type, const gint service_id)
{
    AgManagerPrivate *priv;
    AgService *service;

    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);
    g_return_val_if_fail (service_name != NULL, NULL);
    priv = manager->priv;

    service = g_hash_table_lookup (priv->services, service_name);
    if (service)
    {
        if (service->id == 0)
            service->id = service_id;
        return ag_service_ref (service);
    }

    service = _ag_service_new_from_memory (service_name, service_type, service_id);

    g_hash_table_insert (priv->services, service->name, service);
    return ag_service_ref (service);
}

/**
 * ag_manager_get_service:
 * @manager: the #AgManager.
 * @service_name: the name of the service.
 *
 * Loads the service identified by @service_name.
 *
 * Returns: an #AgService, which must be then free'd with ag_service_unref().
 */
AgService *
ag_manager_get_service (AgManager *manager, const gchar *service_name)
{
    AgManagerPrivate *priv;
    AgService *service;
    gchar *sql;

    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);
    g_return_val_if_fail (service_name != NULL, NULL);
    priv = manager->priv;

    service = g_hash_table_lookup (priv->services, service_name);
    if (service)
        return ag_service_ref (service);

    /* First, check if the service is in the DB */
    sql = sqlite3_mprintf ("SELECT id, display, provider, type "
                           "FROM Services WHERE name = %Q", service_name);
    _ag_manager_exec_query (manager, (AgQueryCallback)got_service,
                            &service, sql);
    sqlite3_free (sql);

    if (service)
    {
        /* the basic server data have been loaded from the DB; the service name
         * is still missing, though */
        service->name = g_strdup (service_name);
    }
    else
    {
        /* The service is not in the DB: it must be loaded */
        service = _ag_service_new_from_file (service_name);

        if (service && !add_service_to_db (manager, service))
        {
            g_warning ("Error in adding service %s to DB!", service_name);
            ag_service_unref (service);
            service = NULL;
        }
    }

    if (G_UNLIKELY (!service)) return NULL;

    g_hash_table_insert (priv->services, service->name, service);
    return ag_service_ref (service);
}

guint
_ag_manager_get_service_id (AgManager *manager, AgService *service)
{
    g_return_val_if_fail (AG_IS_MANAGER (manager), 0);

    if (service == NULL) return 0; /* global service */

    if (service->id == 0)
    {
        gchar *sql;
        gint rows;

        /* We got this service name from another process; load the id from the
         * DB - it must already exist */
        sql = sqlite3_mprintf ("SELECT id FROM Services WHERE name = %Q",
                               service->name);
        rows = _ag_manager_exec_query (manager, (AgQueryCallback)got_service_id,
                                       service, sql);
        sqlite3_free (sql);
        if (G_UNLIKELY (rows != 1))
        {
            g_warning ("%s: got %d rows when asking for service %s",
                       G_STRFUNC, rows, service->name);
        }
    }

    return service->id;
}

/**
 * ag_manager_list_services:
 * @manager: the #AgManager.
 *
 * Gets a list of all the installed services.
 * If the #AgManager is created with specified service_type
 * it will return only the installed services supporting this service_type.
 *
 * Returns: (transfer full) (element-type AgService): a list of #AgService,
 * which must be then free'd with ag_service_list_free().
 */
GList *
ag_manager_list_services (AgManager *manager)
{
    AgManagerPrivate *priv;

    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);
    priv = manager->priv;

    if (priv->service_type)
        return ag_manager_list_services_by_type (manager, priv->service_type);

    return _ag_services_list (manager);
}

/**
 * ag_manager_list_services_by_type:
 * @manager: the #AgManager.
 * @service_type: the type of the service.
 *
 * Gets a list of all the installed services of type @service_type.
 *
 * Returns: (transfer full) (element-type AgService): a list of #AgService,
 * which must be then free'd with ag_service_list_free().
 */
GList *
ag_manager_list_services_by_type (AgManager *manager, const gchar *service_type)
{
    GList *all_services, *list;
    GList *services = NULL;

    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);
    g_return_val_if_fail (service_type != NULL, NULL);

    /* if we kept the DB Service table always up-to-date with all known
     * services, then we could just run a query over it. But while we are not,
     * it's simpler to implement the function by reusing the output from
     * _ag_services_list(manager). */
    all_services = _ag_services_list (manager);
    for (list = all_services; list != NULL; list = list->next)
    {
        AgService *service = list->data;
        const gchar *serviceType = ag_service_get_service_type (service);
        if (serviceType && strcmp (serviceType, service_type) == 0)
        {
            services = g_list_prepend (services, service);
        }
        else
            ag_service_unref (service);
    }
    g_list_free (all_services);

    return services;
}

void
_ag_manager_exec_transaction (AgManager *manager, const gchar *sql,
                              AgAccountChanges *changes, AgAccount *account,
                              AgAccountStoreCb callback, gpointer user_data)
{
    AgManagerPrivate *priv = manager->priv;
    GError *error = NULL;
    int ret;

    ret = prepare_transaction_statements (priv);
    if (G_UNLIKELY (ret != SQLITE_OK))
    {
        error = g_error_new (AG_ERRORS, AG_ERROR_DB, "Got error: %s (%d)",
                             sqlite3_errmsg (priv->db), ret);
        goto finish;
    }

    ret = sqlite3_step (priv->begin_stmt);
    if (ret == SQLITE_BUSY)
    {
        if (callback)
        {
            StoreCbData *sd;

            sd = g_slice_new (StoreCbData);
            sd->manager = manager;
            sd->account = account;
            sd->changes = changes;
            sd->callback = callback;
            sd->user_data = user_data;
            sd->sql = g_strdup (sql);
            sd->id = g_idle_add ((GSourceFunc)exec_transaction_idle, sd);
            priv->locks = g_list_prepend (priv->locks, sd);
            g_object_weak_ref (G_OBJECT (account), lost_weak_ref, sd);
        }
        return;
    }

    if (ret != SQLITE_DONE)
    {
        error = g_error_new (AG_ERRORS, AG_ERROR_DB, "Got error: %s (%d)",
                             sqlite3_errmsg (priv->db), ret);
        goto finish;
    }

    exec_transaction (manager, account, sql, changes, &error);

finish:
    _ag_account_store_completed (account, changes,
                                 callback, error, user_data);
    if (error)
        g_error_free (error);
}

void
_ag_manager_exec_transaction_blocking (AgManager *manager, const gchar *sql,
                                       AgAccountChanges *changes,
                                       AgAccount *account,
                                       GError **error)
{
    AgManagerPrivate *priv = manager->priv;
    gint sleep_ms = 200;
    int ret;

    ret = prepare_transaction_statements (priv);
    if (G_UNLIKELY (ret != SQLITE_OK))
    {
        *error = g_error_new (AG_ERRORS, AG_ERROR_DB, "Got error: %s (%d)",
                              sqlite3_errmsg (priv->db), ret);
        return;
    }

    ret = sqlite3_step (priv->begin_stmt);
    while (ret == SQLITE_BUSY)
    {
        /* TODO: instead of this loop, use a semaphore or some other non
         * polling mechanism */
        if (sleep_ms > 30000)
        {
            DEBUG_LOCKS ("Database locked for more than 30 seconds; "
                         "giving up!");
            break;
        }
        DEBUG_LOCKS ("Database locked, sleeping for %ums", sleep_ms);
        g_usleep (sleep_ms * 1000);
        sleep_ms *= 2;
        ret = sqlite3_step (priv->begin_stmt);
    }

    if (ret != SQLITE_DONE)
    {
        *error = g_error_new (AG_ERRORS, AG_ERROR_DB, "Got error: %s (%d)",
                              sqlite3_errmsg (priv->db), ret);
        return;
    }

    exec_transaction (manager, account, sql, changes, error);
}

static guint
timespec_diff_ms(struct timespec *ts1, struct timespec *ts0)
{
    return (ts1->tv_sec - ts0->tv_sec) * 1000 +
        (ts1->tv_nsec - ts0->tv_nsec) / 1000000;
}

/* Executes an SQL statement, and optionally calls
 * the callback for every row of the result.
 * Returns the number of rows fetched.
 */
gint
_ag_manager_exec_query (AgManager *manager,
                        AgQueryCallback callback, gpointer user_data,
                        const gchar *sql)
{
    sqlite3 *db;
    int ret;
    sqlite3_stmt *stmt;
    struct timespec ts0, ts1;
    gint rows = 0;

    g_return_val_if_fail (AG_IS_MANAGER (manager), 0);
    db = manager->priv->db;

    g_return_val_if_fail (db != NULL, 0);

    ret = sqlite3_prepare_v2 (db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK)
    {
        g_warning ("%s: can't compile SQL statement \"%s\": %s", G_STRFUNC, sql,
                   sqlite3_errmsg (db));
        return 0;
    }

    DEBUG_QUERIES ("about to run:\n%s", sql);

    /* get the current time, to abort the operation in case the DB is locked
     * for longer than db_timeout. */
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    do
    {
        ret = sqlite3_step (stmt);

        switch (ret)
        {
            case SQLITE_DONE:
                break;

            case SQLITE_ROW:
                if (callback == NULL || callback (stmt, user_data))
                {
                    rows++;
                }
                break;

            case SQLITE_BUSY:
                clock_gettime(CLOCK_MONOTONIC, &ts1);
                if (timespec_diff_ms(&ts1, &ts0) < manager->priv->db_timeout)
                {
                    /* If timeout was specified and table is locked,
                     * wait instead of executing default runtime
                     * error action. Otherwise, fall through to it. */
                    sched_yield ();
                    break;
                }

            default:
                set_error_from_db (manager);
                g_warning ("%s: runtime error while executing \"%s\": %s",
                           G_STRFUNC, sql, sqlite3_errmsg (db));
                sqlite3_finalize (stmt);
                return rows;
        }
    } while (ret != SQLITE_DONE);

    sqlite3_finalize (stmt);

    return rows;
}

/**
 * ag_manager_get_provider:
 * @manager: the #AgManager.
 * @provider_name: the name of the provider.
 *
 * Loads the provider identified by @provider_name.
 *
 * Returns: an #AgProvider, which must be then free'd with ag_provider_unref().
 */
AgProvider *
ag_manager_get_provider (AgManager *manager, const gchar *provider_name)
{
    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);
    g_return_val_if_fail (provider_name != NULL, NULL);

    /* We don't implement any caching mechanism for AgProvider structures: they
     * shouldn't be loaded that often. */
    return _ag_provider_new_from_file (provider_name);
}

/**
 * ag_manager_list_providers:
 * @manager: the #AgManager.
 *
 * Gets a list of all the installed providers.
 *
 * Returns: (transfer full) (element-type AgProvider): a list of #AgProvider,
 * which must be then free'd with ag_provider_list_free().
 */
GList *
ag_manager_list_providers (AgManager *manager)
{
    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);

    return _ag_providers_list (manager);
}

/**
 * ag_manager_new_for_service_type:
 * @service_type: the name of a service type
 *
 * Create a new #AgManager with the @service_type service type.
 *
 * Returns: an instance of an #AgManager with specified service type.
 */
AgManager *
ag_manager_new_for_service_type (const gchar *service_type)
{
    AgManager *manager;

    g_return_val_if_fail (service_type != NULL, NULL);

    manager = g_object_new (AG_TYPE_MANAGER, "service-type", service_type, NULL);
    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);

    return manager;
}

/**
 * ag_manager_get_service_type:
 * @manager: the #AgManager.
 *
 * Get the service type for @manager.
 *
 * Returns: the name of the service type for the supplied @manager.
 */
const gchar *
ag_manager_get_service_type (AgManager *manager)
{
    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);

    return manager->priv->service_type;
}

/**
 * ag_manager_set_db_timeout:
 * @manager: the #AgManager.
 * @timeout_ms: the new timeout, in milliseconds.
 *
 * Sets the timeout for database operations. This tells the library how long
 * it is allowed to block while waiting for a locked DB to become accessible.
 * Higher values mean a higher chance of successful reads, but also mean that
 * the execution might be blocked for a longer time.
 * The default is 5 seconds.
 */
void
ag_manager_set_db_timeout (AgManager *manager, guint timeout_ms)
{
    g_return_if_fail (AG_IS_MANAGER (manager));
    manager->priv->db_timeout = timeout_ms;
}

/**
 * ag_manager_get_db_timeout:
 * @manager: the #AgManager.
 *
 * Get the timeout of database operations for @manager, in milliseconds.
 *
 * Returns: the timeout (in milliseconds) for database operations.
 */
guint
ag_manager_get_db_timeout (AgManager *manager)
{
    g_return_val_if_fail (AG_IS_MANAGER (manager), 0);
    return manager->priv->db_timeout;
}

/**
 * ag_manager_set_abort_on_db_timeout:
 * @manager: the #AgManager.
 * @abort: whether to abort when a DB timeout occurs.
 *
 * Tells libaccounts whether it should make the client application abort when
 * a timeout error occurs. The default is %FALSE.
 */
void
ag_manager_set_abort_on_db_timeout (AgManager *manager, gboolean abort)
{
    g_return_if_fail (AG_IS_MANAGER (manager));
    manager->priv->abort_on_db_timeout = abort;
}

/**
 * ag_manager_get_abort_on_db_timeout:
 * @manager: the #AgManager.
 *
 * Get whether the library will abort when a timeout error occurs.
 *
 * Returns: %TRUE is the library will abort when a timeout error occurs, %FALSE
 * otherwise.
 */
gboolean
ag_manager_get_abort_on_db_timeout (AgManager *manager)
{
    g_return_val_if_fail (AG_IS_MANAGER (manager), FALSE);
    return manager->priv->abort_on_db_timeout;
}

/**
 * ag_manager_load_service_type:
 * @manager: the #AgManager.
 * @service_type: the name of the service type.
 *
 * Instantiate the service type @service_type.
 *
 * Returns: (transfer full): an #AgServiceType, which must be then free'd with
 * ag_service_type_unref().
 */
AgServiceType *
ag_manager_load_service_type (AgManager *manager, const gchar *service_type)
{
    g_return_val_if_fail (AG_IS_MANAGER (manager), NULL);

    /* Given the small size of the service type file, and the unlikely need to
     * load them more than once, we don't cache them in the manager. But this
     * might change in the future.
     */
    return _ag_service_type_new_from_file (service_type);
}


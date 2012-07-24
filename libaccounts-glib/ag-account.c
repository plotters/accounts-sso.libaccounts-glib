/* vi: set et sw=4 ts=4 cino=t0,(0: */
/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of libaccounts-glib
 *
 * Copyright (C) 2009-2010 Nokia Corporation.
 * Copyright (C) 2012 Canonical Ltd.
 *
 * Contact: Alberto Mardegan <alberto.mardegan@canonical.com>
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
 * SECTION:ag-account
 * @short_description: A representation of an account.
 * @include: libaccounts-glib/ag-account.h
 *
 * An #AgAccount is an object which represents an account. It provides a
 * method for enabling/disabling the account and methods for editing the
 * account settings.
 *
 * Accounts are created by #AgManager with ag_manager_create_account(), and
 * deleted by #AgAccount with ag_account_delete(). These operations, and any
 * other operations which modify the account settings, must be followed by
 * ag_account_store() before the changes are committed to the database.
 * <example id="example-create-new-AgAccount">
 * <title>Creating a new <structname>AgAccount</structname></title>
 * <programlisting>
 * GMainLoop *main_loop = NULL;
 *
 * gboolean account_cleanup_idle (gpointer user_data)
 * {
 *     AgManager *manager;
 *     AgAccount *account = AG_ACCOUNT (user_data);
 *     manager = ag_account_get_manager (account);
 *
 *     g_object_unref (account);
 *     g_object_unref (manager);
 *
 *     g_main_loop_quit (main_loop);
 *
 *     return FALSE;
 * }
 *
 * void account_stored_cb (AgAccount *account,
 *                         const GError *error,
 *                         gpointer user_data)
 * {
 *     AgManager *manager = AG_MANAGER (user_data);
 *
 *     if (error != NULL)
 *     {
 *         g_warning ("Account with ID '%u' failed to store, with error: %s",
 *                    account->id,
 *                    error->message);
 *     }
 *     else
 *     {
 *         g_print ("Account stored with ID: %u", account->id);
 *     }
 *
 *     /&ast; Clean up in an idle callback. &ast;/
 *     g_idle_add (account_cleanup_idle, account);
 *     g_main_loop_run (main_loop);
 * }
 *
 * void store_account (void)
 * {
 *     AgManager *manager;
 *     GList *providers;
 *     const gchar *provider_name;
 *     AgAccount *account;
 *
 *     main_loop = g_main_loop_new 9NULL, FALSE);
 *     manager = ag_manager_new ();
 *     providers = ag_manager_list_providers (manager);
 *     g_assert (providers != NULL);
 *     provider_name = ag_provider_get_name ((AgProvider *) providers->data);
 *     account = ag_manager_create_account (manager, provider_name);
 *
 *     ag_provider_list_free (providers);
 *
 *     /&ast; The account is not valid until it has been stored. &ast;/
 *     ag_account_store (account, account_stored_cb, (gpointer) manager);
 * }
 * </programlisting>
 * </example>
 */

#include "ag-manager.h"
#include "ag-account.h"
#include "ag-errors.h"

#include "ag-internals.h"
#include "ag-marshal.h"
#include "ag-service.h"
#include "ag-util.h"

#include "config.h"

#include <string.h>

#define SERVICE_GLOBAL "global"

enum
{
    PROP_0,

    PROP_ID,
    PROP_MANAGER,
    PROP_PROVIDER,
    PROP_FOREIGN,
};

enum
{
    ENABLED,
    DISPLAY_NAME_CHANGED,
    DELETED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

typedef struct _AgServiceChanges {
    AgService *service; /* this is set only if the change came from this
                           instance */
    gchar *service_type;

    GHashTable *settings;
    GHashTable *signatures;
} AgServiceChanges;

typedef struct _AgServiceSettings {
    AgService *service;
    GHashTable *settings;
} AgServiceSettings;

struct _AgAccountPrivate {
    AgManager *manager;

    /* selected service */
    AgService *service;

    gchar *provider_name;
    gchar *display_name;

    /* cached settings: keys are service names, values are AgServiceSettings
     * structures.
     * It may be that not all services are loaded in this hash table. But if a
     * service is present, then all of its settings are.
     */
    GHashTable *services;

    AgAccountChanges *changes;

    /* Watches: it's a GHashTable whose keys are pointers to AgService
     * elements, and values are GHashTables whose keys and values are
     * AgAccountWatch-es. */
    GHashTable *watches;

    /* Temporary pointer to the services table of the AgAccountChanges
     * structure, to be used while invoking the watches in case some handlers
     * want to retrieve the list of changes (AgAccountService does this).
     */
    GHashTable *changes_for_watches;

    /* The "foreign" flag means that the account has been created by another
     * instance and we got informed about it from D-Bus. In this case, all the
     * informations that we get via D-Bus will be cached in the
     * AgServiceSetting structures. */
    guint foreign : 1;
    guint enabled : 1;
    guint deleted : 1;
};

struct _AgAccountWatch {
    AgService *service;
    gchar *key;
    gchar *prefix;
    AgAccountNotifyCb callback;
    gpointer user_data;
};

/* Same size and member types as AgAccountSettingIter */
typedef struct {
    AgAccount *account;
    GHashTableIter iter;
    gchar *key_prefix;
    gpointer ptr2;
    gint stage;
    gint must_free_prefix;
} RealIter;

typedef struct _AgSignature {
    gchar *signature;
    gchar *token;
} AgSignature;

#define AG_ITER_STAGE_UNSET     0
#define AG_ITER_STAGE_ACCOUNT   1
#define AG_ITER_STAGE_SERVICE   2

G_DEFINE_TYPE (AgAccount, ag_account, G_TYPE_OBJECT);

#define AG_ACCOUNT_PRIV(obj) (AG_ACCOUNT(obj)->priv)

DBusMessage *
_ag_account_build_signal (AgAccount *account, AgAccountChanges *changes,
                          const struct timespec *ts)
{
    DBusMessage *msg;
    DBusMessageIter iter, i_serv, dict, i_struct, i_list;
    gboolean ret;
    const gchar *provider_name;

    /* The object path is not important here; it will be set to a valid
     * value by the AgManager, when sending the signal. */
    msg = dbus_message_new_signal ("/", AG_DBUS_IFACE,
                                   AG_DBUS_SIG_CHANGED);
    g_return_val_if_fail (msg != NULL, NULL);

    provider_name = account->priv->provider_name;
    if (!provider_name) provider_name = "";

    ret = dbus_message_append_args (msg,
                                    DBUS_TYPE_UINT32, &ts->tv_sec,
                                    DBUS_TYPE_UINT32, &ts->tv_nsec,
                                    DBUS_TYPE_UINT32, &account->id,
                                    DBUS_TYPE_BOOLEAN, &changes->created,
                                    DBUS_TYPE_BOOLEAN, &changes->deleted,
                                    DBUS_TYPE_STRING, &provider_name,
                                    DBUS_TYPE_INVALID);
    if (G_UNLIKELY (!ret)) goto error;

    /* Append the settings */
    dbus_message_iter_init_append (msg, &iter);
    dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY,
                                      "(ssua{sv}as)", &i_serv);
    if (changes->services)
    {
        GHashTableIter iter;
        AgServiceChanges *sc;
        gchar *service_name;

        g_hash_table_iter_init (&iter, changes->services);
        while (g_hash_table_iter_next (&iter,
                                       (gpointer)&service_name, (gpointer)&sc))
        {
            GSList *removed_keys = NULL;
            GHashTableIter si;
            gchar *key;
            GValue *value;
            guint service_id;

            dbus_message_iter_open_container (&i_serv, DBUS_TYPE_STRUCT,
                                              NULL, &i_struct);
            /* Append the service name */
            dbus_message_iter_append_basic (&i_struct, DBUS_TYPE_STRING,
                                            &service_name);
            /* Append the service type */
            dbus_message_iter_append_basic (&i_struct, DBUS_TYPE_STRING,
                                            &sc->service_type);
            /* Append the service id */
            if (sc->service == NULL)
                service_id = 0;
            else
                service_id = sc->service->id;

            dbus_message_iter_append_basic (&i_struct, DBUS_TYPE_UINT32, &service_id);
            /* Append the dictionary of service settings */
            dbus_message_iter_open_container (&i_struct, DBUS_TYPE_ARRAY,
                                              "{sv}", &dict);

            g_hash_table_iter_init (&si, sc->settings);
            while (g_hash_table_iter_next (&si,
                                           (gpointer)&key, (gpointer)&value))
            {
                if (value)
                    _ag_iter_append_dict_entry (&dict, key, value);
                else
                    removed_keys = g_slist_prepend (removed_keys, key);
            }
            dbus_message_iter_close_container (&i_struct, &dict);

            /* append the list of removed keys */
            dbus_message_iter_open_container (&i_struct, DBUS_TYPE_ARRAY,
                                              "s", &i_list);
            while (removed_keys)
            {
                dbus_message_iter_append_basic (&i_list, DBUS_TYPE_STRING,
                                                &removed_keys->data);
                removed_keys = g_slist_delete_link (removed_keys, removed_keys);
            }
            dbus_message_iter_close_container (&i_struct, &i_list);
            dbus_message_iter_close_container (&i_serv, &i_struct);
        }
    }
    dbus_message_iter_close_container (&iter, &i_serv);
    return msg;

error:
    dbus_message_unref (msg);
    return NULL;
}

static void
ag_account_watch_free (AgAccountWatch watch)
{
    g_return_if_fail (watch != NULL);
    g_free (watch->key);
    g_free (watch->prefix);
    g_slice_free (struct _AgAccountWatch, watch);
}

static AgService *
ag_service_ref_null (AgService *service)
{
    if (service)
        ag_service_ref (service);
    return service;
}

static void
ag_service_unref_null (AgService *service)
{
    if (service)
        ag_service_unref (service);
}

static AgAccountWatch
ag_account_watch_int (AgAccount *account, gchar *key, gchar *prefix,
                      AgAccountNotifyCb callback, gpointer user_data)
{
    AgAccountPrivate *priv = account->priv;
    AgAccountWatch watch;
    GHashTable *service_watches;

    if (!priv->watches)
    {
        priv->watches =
            g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                   (GDestroyNotify)ag_service_unref_null,
                                   (GDestroyNotify)g_hash_table_destroy);
    }

    service_watches = g_hash_table_lookup (priv->watches, priv->service);
    if (!service_watches)
    {
        service_watches =
            g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                   NULL,
                                   (GDestroyNotify)ag_account_watch_free);
        g_hash_table_insert (priv->watches,
                             ag_service_ref_null (priv->service),
                             service_watches);
    }

    watch = g_slice_new (struct _AgAccountWatch);
    watch->service = priv->service;
    watch->key = key;
    watch->prefix = prefix;
    watch->callback = callback;
    watch->user_data = user_data;

    g_hash_table_insert (service_watches, watch, watch);

    return watch;
}

static gboolean
got_account_setting (sqlite3_stmt *stmt, GHashTable *settings)
{
    gchar *key;
    GValue *value;

    key = g_strdup ((gchar *)sqlite3_column_text (stmt, 0));
    g_return_val_if_fail (key != NULL, FALSE);

    value = _ag_value_from_db (stmt, 1, 2);

    g_hash_table_insert (settings, key, value);
    return TRUE;
}

static void
ag_service_settings_free (AgServiceSettings *ss)
{
    if (ss->service)
        ag_service_unref (ss->service);
    g_hash_table_unref (ss->settings);
    g_slice_free (AgServiceSettings, ss);
}

static AgServiceSettings *
get_service_settings (AgAccountPrivate *priv, AgService *service,
                      gboolean create)
{
    AgServiceSettings *ss;
    const gchar *service_name;

    if (G_UNLIKELY (!priv->services))
    {
        priv->services = g_hash_table_new_full
            (g_str_hash, g_str_equal,
             NULL, (GDestroyNotify) ag_service_settings_free);
    }

    service_name = service ? service->name : SERVICE_GLOBAL;
    ss = g_hash_table_lookup (priv->services, service_name);
    if (!ss && create)
    {
        ss = g_slice_new (AgServiceSettings);
        ss->service = service ? ag_service_ref (service) : NULL;
        ss->settings = g_hash_table_new_full
            (g_str_hash, g_str_equal,
             g_free, (GDestroyNotify)_ag_value_slice_free);
        g_hash_table_insert (priv->services, (gchar *)service_name, ss);
    }

    return ss;
}

static gboolean
ag_account_changes_get_enabled (AgAccountChanges *changes, gboolean *enabled)
{
    AgServiceChanges *sc;
    const GValue *value;

    sc = g_hash_table_lookup (changes->services, SERVICE_GLOBAL);
    if (sc)
    {
        value = g_hash_table_lookup (sc->settings, "enabled");
        if (value)
        {
            *enabled = g_value_get_boolean (value);
            return TRUE;
        }
    }
    *enabled = FALSE;
    return FALSE;
}

static gboolean
ag_account_changes_get_display_name (AgAccountChanges *changes,
                                     const gchar **display_name)
{
    AgServiceChanges *sc;
    const GValue *value;

    sc = g_hash_table_lookup (changes->services, SERVICE_GLOBAL);
    if (sc)
    {
        value = g_hash_table_lookup (sc->settings, "name");
        if (value)
        {
            *display_name = g_value_get_string (value);
            return TRUE;
        }
    }
    *display_name = NULL;
    return FALSE;
}

static void
ag_service_changes_free (AgServiceChanges *sc)
{
    g_free (sc->service_type);

    if (sc->settings)
        g_hash_table_unref (sc->settings);

    if (sc->signatures)
        g_hash_table_unref (sc->signatures);

    g_slice_free (AgServiceChanges, sc);
}

void
_ag_account_changes_free (AgAccountChanges *changes)
{
    if (G_LIKELY (changes))
    {
        g_hash_table_unref (changes->services);
        g_slice_free (AgAccountChanges, changes);
    }
}

static GList *
match_watch_with_key (AgAccount *account, GHashTable *watches,
                      const gchar *key, GList *watch_list)
{
    GHashTableIter iter;
    AgAccountWatch watch;

    g_hash_table_iter_init (&iter, watches);
    while (g_hash_table_iter_next (&iter, NULL, (gpointer)&watch))
    {
        if (watch->key)
        {
            if (strcmp (key, watch->key) == 0)
            {
                watch_list = g_list_prepend (watch_list, watch);
            }
        }
        else /* match on the prefix */
        {
            if (g_str_has_prefix (key, watch->prefix))
            {
                /* before addind the watch to the list, make sure it's not
                 * already there */
                if (!g_list_find (watch_list, watch))
                    watch_list = g_list_prepend (watch_list, watch);
            }
        }
    }
    return watch_list;
}

static void
update_settings (AgAccount *account, GHashTable *services)
{
    AgAccountPrivate *priv = account->priv;
    GHashTableIter iter;
    AgServiceChanges *sc;
    gchar *service_name;
    GList *watch_list = NULL;

    g_hash_table_iter_init (&iter, services);
    while (g_hash_table_iter_next (&iter,
                                   (gpointer)&service_name, (gpointer)&sc))
    {
        AgServiceSettings *ss;
        GHashTableIter si;
        gchar *key;
        GValue *value;
        GHashTable *watches = NULL;

        if (priv->foreign)
        {
            /* If the account has been created from another instance
             * (which might be in another process), the "changes" structure
             * contains all the account settings for all services.
             *
             * Instead of discarding this precious information, we store all
             * the settings in memory, to minimize future disk accesses.
             */
            ss = get_service_settings (priv, sc->service, TRUE);
        }
        else
        {
            /* if the changed service doesn't have a AgServiceSettings entry it
             * means that the service was never selected on this account, so we
             * don't need to update its settings. */
            if (!priv->services) continue;
            ss = g_hash_table_lookup (priv->services, service_name);
        }
        if (!ss) continue;

        /* get the watches associated to this service */
        if (priv->watches)
            watches = g_hash_table_lookup (priv->watches, ss->service);

        g_hash_table_iter_init (&si, sc->settings);
        while (g_hash_table_iter_next (&si,
                                       (gpointer)&key, (gpointer)&value))
        {
            if (ss->service == NULL)
            {
                if (strcmp (key, "name") == 0)
                {
                    g_free (priv->display_name);
                    priv->display_name =
                        value ? g_value_dup_string (value) : NULL;
                    g_signal_emit (account, signals[DISPLAY_NAME_CHANGED], 0);
                    continue;
                }
                else if (strcmp (key, "enabled") == 0)
                {
                    priv->enabled =
                        value ? g_value_get_boolean (value) : FALSE;
                    g_signal_emit (account, signals[ENABLED], 0,
                                   service_name, priv->enabled);
                    continue;
                }
            }

            if (value)
                g_hash_table_replace (ss->settings,
                                      g_strdup (key),
                                      _ag_value_slice_dup (value));
            else
                g_hash_table_remove (ss->settings, key);

            /* check for installed watches to be invoked */
            if (watches)
                watch_list = match_watch_with_key (account, watches, key,
                                                   watch_list);

            if (strcmp (key, "enabled") == 0)
            {
                gboolean enabled =
                    value ? g_value_get_boolean (value) : FALSE;
                g_signal_emit (account, signals[ENABLED], 0,
                               service_name, enabled);
            }
        }
    }

    /* Invoke all watches
     * While whatches are running, let the receivers retrieve the changes
     * table with _ag_account_get_service_changes(): set it into the
     * changes_for_watches field. */
    priv->changes_for_watches = services;
    while (watch_list)
    {
        AgAccountWatch watch = watch_list->data;

        if (watch->key)
            watch->callback (account, watch->key, watch->user_data);
        else
            watch->callback (account, watch->prefix, watch->user_data);
        watch_list = g_list_delete_link (watch_list, watch_list);
    }
    priv->changes_for_watches = NULL;
}

void
_ag_account_store_completed (AgAccount *account, AgAccountChanges *changes,
                             AgAccountStoreCb callback, const GError *error,
                             gpointer user_data)
{
    if (callback)
        callback (account, error, user_data);

    _ag_account_changes_free (changes);
}

/*
 * _ag_account_done_changes:
 *
 * This function is called after a successful execution of a transaction, and
 * must update the account data as with the contents of the AgAccountChanges
 * structure.
 */
void
_ag_account_done_changes (AgAccount *account, AgAccountChanges *changes)
{
    AgAccountPrivate *priv = account->priv;

    g_return_if_fail (changes != NULL);

    if (changes->services)
        update_settings (account, changes->services);

    if (changes->deleted)
    {
        priv->deleted = TRUE;
        priv->enabled = FALSE;
        g_signal_emit (account, signals[ENABLED], 0, NULL, FALSE);
        g_signal_emit (account, signals[DELETED], 0);
    }
}

static AgAccountChanges *
account_changes_get (AgAccountPrivate *priv)
{
    if (!priv->changes)
    {
        priv->changes = g_slice_new0 (AgAccountChanges);
        priv->changes->services =
            g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
                                   (GDestroyNotify)ag_service_changes_free);
    }

    return priv->changes;
}

static void
_ag_signatures_slice_free (AgSignature *sgn)
{
    g_free (sgn->signature);
    g_free (sgn->token);
    g_slice_free (AgSignature, sgn);
}

static AgServiceChanges*
account_service_changes_get (AgAccountPrivate *priv, AgService *service,
                             gboolean create_signatures)
{
    AgAccountChanges *changes;
    AgServiceChanges *sc;
    gchar *service_name;
    gchar *service_type;

    changes = account_changes_get (priv);

    service_name = service ? service->name : SERVICE_GLOBAL;
    service_type = service ? service->type : SERVICE_GLOBAL_TYPE;

    sc = g_hash_table_lookup (changes->services, service_name);
    if (!sc)
    {
        sc = g_slice_new0 (AgServiceChanges);
        sc->service = service;
        sc->service_type = g_strdup (service_type);

        sc->settings = g_hash_table_new_full
            (g_str_hash, g_str_equal,
            g_free, (GDestroyNotify)_ag_value_slice_free);
        g_hash_table_insert (changes->services, service_name, sc);
    }

    if (create_signatures && !sc->signatures)
        sc->signatures = g_hash_table_new_full
            (g_str_hash, g_str_equal,
             g_free, (GDestroyNotify)_ag_signatures_slice_free);

    return sc;
}

GHashTable *
_ag_account_get_service_changes (AgAccount *account, AgService *service)
{
    GHashTable *services;
    AgServiceChanges *sc;

    services = account->priv->changes_for_watches;
    if (!services) return NULL;

    sc = g_hash_table_lookup (services,
                              service ? service->name : SERVICE_GLOBAL);
    if (!sc) return NULL;
    return sc->settings;
}

static void
change_service_value (AgAccountPrivate *priv, AgService *service,
                      const gchar *key, const GValue *value)
{
    AgServiceChanges *sc;
    sc = account_service_changes_get (priv, service, FALSE);
    g_hash_table_insert (sc->settings,
                         g_strdup (key), _ag_value_slice_dup (value));
}

static inline void
change_selected_service_value (AgAccountPrivate *priv,
                               const gchar *key, const GValue *value)
{
    change_service_value(priv, priv->service, key, value);
}

static void
ag_account_init (AgAccount *account)
{
    account->priv = G_TYPE_INSTANCE_GET_PRIVATE (account, AG_TYPE_ACCOUNT,
                                                 AgAccountPrivate);
}

static gboolean
got_account (sqlite3_stmt *stmt, AgAccountPrivate *priv)
{
    g_assert (priv->display_name == NULL);
    g_assert (priv->provider_name == NULL);
    priv->display_name = g_strdup ((gchar *)sqlite3_column_text (stmt, 0));
    priv->provider_name = g_strdup ((gchar *)sqlite3_column_text (stmt, 1));
    priv->enabled = sqlite3_column_int (stmt, 2);
    return TRUE;
}

static gboolean
ag_account_load (AgAccount *account)
{
    AgAccountPrivate *priv = account->priv;
    gchar sql[128];
    gint rows;

    g_snprintf (sql, sizeof (sql),
                "SELECT name, provider, enabled "
                "FROM Accounts WHERE id = %u", account->id);
    rows = _ag_manager_exec_query (priv->manager,
                                   (AgQueryCallback)got_account, priv, sql);
    /* if the query succeeded but we didn't get a row, we must set the
     * NOT_FOUND error */
    if (_ag_manager_get_last_error (priv->manager) == NULL && rows != 1)
    {
        GError *error = g_error_new (AG_ACCOUNTS_ERROR,
                                     AG_ACCOUNTS_ERROR_ACCOUNT_NOT_FOUND,
                                     "Account %u not found in DB", account->id);
        _ag_manager_take_error (priv->manager, error);
    }

    return rows == 1;
}

static GObject *
ag_account_constructor (GType type, guint n_params,
                        GObjectConstructParam *params)
{
    GObjectClass *object_class = (GObjectClass *)ag_account_parent_class;
    GObject *object;
    AgAccount *account;

    object = object_class->constructor (type, n_params, params);
    g_return_val_if_fail (AG_IS_ACCOUNT (object), NULL);

    account = AG_ACCOUNT (object);
    if (account->id)
    {
        if (account->priv->changes && account->priv->changes->created)
        {
            /* this is a new account and we should not load it */
            _ag_account_changes_free (account->priv->changes);
            account->priv->changes = NULL;
        }
        else if (!ag_account_load (account))
        {
            g_warning ("Unable to load account %u", account->id);
            g_object_unref (object);
            return NULL;
        }
    }

    if (!account->priv->foreign)
        ag_account_select_service (account, NULL);

    return object;
}

static void
ag_account_get_property (GObject *object, guint property_id,
                         GValue *value, GParamSpec *pspec)
{
    AgAccount *account = AG_ACCOUNT (object);

    switch (property_id)
    {
    case PROP_ID:
        g_value_set_uint (value, account->id);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
ag_account_set_property (GObject *object, guint property_id,
                         const GValue *value, GParamSpec *pspec)
{
    AgAccount *account = AG_ACCOUNT (object);
    AgAccountPrivate *priv = account->priv;

    switch (property_id)
    {
    case PROP_ID:
        g_assert (account->id == 0);
        account->id = g_value_get_uint (value);
        break;
    case PROP_MANAGER:
        g_assert (priv->manager == NULL);
        priv->manager = g_value_dup_object (value);
        break;
    case PROP_PROVIDER:
        g_assert (priv->provider_name == NULL);
        priv->provider_name = g_value_dup_string (value);
        /* if this property is given, it means we are creating a new account */
        if (priv->provider_name)
        {
            AgAccountChanges *changes = account_changes_get (priv);
            changes->created = TRUE;
        }
        break;
    case PROP_FOREIGN:
        priv->foreign = g_value_get_boolean (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
ag_account_dispose (GObject *object)
{
    AgAccount *account = AG_ACCOUNT (object);
    AgAccountPrivate *priv = account->priv;

    DEBUG_REFS ("Disposing account %p", object);

    if (priv->watches)
    {
        g_hash_table_destroy (priv->watches);
        priv->watches = NULL;
    }

    if (priv->manager)
    {
        g_object_unref (priv->manager);
        priv->manager = NULL;
    }

    G_OBJECT_CLASS (ag_account_parent_class)->dispose (object);
}

static void
ag_account_finalize (GObject *object)
{
    AgAccountPrivate *priv = AG_ACCOUNT_PRIV (object);

    g_free (priv->provider_name);
    g_free (priv->display_name);

    if (priv->services)
        g_hash_table_unref (priv->services);

    if (priv->changes)
    {
        DEBUG_INFO ("Finalizing account with uncommitted changes!");
        _ag_account_changes_free (priv->changes);
    }

    G_OBJECT_CLASS (ag_account_parent_class)->finalize (object);
}

static void
ag_account_class_init (AgAccountClass *klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (AgAccountPrivate));

    object_class->constructor = ag_account_constructor;
    object_class->get_property = ag_account_get_property;
    object_class->set_property = ag_account_set_property;
    object_class->dispose = ag_account_dispose;
    object_class->finalize = ag_account_finalize;

    /**
     * AgAccount:id:
     *
     * The AgAccountId for the account.
     */
    g_object_class_install_property
        (object_class, PROP_ID,
         g_param_spec_uint ("id", "Account ID",
                            "The AgAccountId of the account",
                            0, G_MAXUINT, 0,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property
        (object_class, PROP_MANAGER,
         g_param_spec_object ("manager", "manager", "manager",
                              AG_TYPE_MANAGER,
                              G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property
        (object_class, PROP_PROVIDER,
         g_param_spec_string ("provider", "provider", "provider",
                              NULL,
                              G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY |
                              G_PARAM_STATIC_STRINGS));

    g_object_class_install_property
        (object_class, PROP_FOREIGN,
         g_param_spec_boolean ("foreign", "foreign", "foreign",
                               FALSE,
                               G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY |
                               G_PARAM_STATIC_STRINGS));

    /**
     * AgAccount::enabled:
     * @account: the #AgAccount.
     * @service: the service which was enabled/disabled, or %NULL if the global
     * enabledness of the account changed.
     * @enabled: the new state of the @account.
     *
     * Emitted when the account "enabled" status was changed for one of its
     * services, or for the account globally.
     */
    signals[ENABLED] = g_signal_new ("enabled",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0,
        NULL, NULL,
        ag_marshal_VOID__STRING_BOOLEAN,
        G_TYPE_NONE,
        2, G_TYPE_STRING, G_TYPE_BOOLEAN);

    /**
     * AgAccount::display-name-changed:
     * @account: the #AgAccount.
     *
     * Emitted when the account display name has changed.
     */
    signals[DISPLAY_NAME_CHANGED] = g_signal_new ("display-name-changed",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0,
        NULL, NULL,
        g_cclosure_marshal_VOID__VOID,
        G_TYPE_NONE,
        0);

    /**
     * AgAccount::deleted:
     * @account: the #AgAccount.
     *
     * Emitted when the account has been deleted.
     */
    signals[DELETED] = g_signal_new ("deleted",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        0,
        NULL, NULL,
        g_cclosure_marshal_VOID__VOID,
        G_TYPE_NONE, 0);
}

AgAccountChanges *
_ag_account_changes_from_dbus (AgManager *manager, DBusMessageIter *iter,
                               gboolean created, gboolean deleted)
{
    AgAccountChanges *changes;
    AgServiceChanges *sc;
    DBusMessageIter i_serv, i_struct, i_dict, i_list;
    gchar *service_name;
    gchar *service_type;
    gint service_id;

    changes = g_slice_new0 (AgAccountChanges);
    changes->created = created;
    changes->deleted = deleted;
    changes->services =
        g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
                               (GDestroyNotify)ag_service_changes_free);

    /* TODO: parse the settings */
#define EXPECT_TYPE(i, t) \
    if (G_UNLIKELY (dbus_message_iter_get_arg_type (i) != t)) \
    { \
        DEBUG_INFO ("%s expected (%c), got (%c)", G_STRLOC, \
                    t, dbus_message_iter_get_arg_type (i)); \
        goto error; \
    }

    EXPECT_TYPE (iter, DBUS_TYPE_ARRAY);
    dbus_message_iter_recurse (iter, &i_serv);

    /* iterate the array of "ssa{sv}as", each one holds one service */
    while (dbus_message_iter_get_arg_type (&i_serv) != DBUS_TYPE_INVALID)
    {
        EXPECT_TYPE (&i_serv, DBUS_TYPE_STRUCT);
        dbus_message_iter_recurse (&i_serv, &i_struct);

        EXPECT_TYPE (&i_struct, DBUS_TYPE_STRING);
        dbus_message_iter_get_basic (&i_struct, &service_name);
        dbus_message_iter_next (&i_struct);

        EXPECT_TYPE (&i_struct, DBUS_TYPE_STRING);
        dbus_message_iter_get_basic (&i_struct, &service_type);
        dbus_message_iter_next (&i_struct);

        EXPECT_TYPE (&i_struct, DBUS_TYPE_UINT32);
        dbus_message_iter_get_basic (&i_struct, &service_id);
        dbus_message_iter_next (&i_struct);

        sc = g_slice_new0 (AgServiceChanges);
        if (service_name != NULL && strcmp (service_name, SERVICE_GLOBAL) == 0)
            sc->service = NULL;
        else
            sc->service = _ag_manager_get_service_lazy (manager, service_name,
                                                        service_type,
                                                        service_id);
        sc->service_type = g_strdup (service_type);

        sc->settings = g_hash_table_new_full
            (g_str_hash, g_str_equal,
             g_free, (GDestroyNotify)_ag_value_slice_free);
        g_hash_table_insert (changes->services, service_name, sc);

        EXPECT_TYPE (&i_struct, DBUS_TYPE_ARRAY);
        dbus_message_iter_recurse (&i_struct, &i_dict);

        /* iterate the "a{sv}" of settings */
        while (dbus_message_iter_get_arg_type (&i_dict) != DBUS_TYPE_INVALID)
        {
            const gchar *key;
            GValue value = { 0 };

            EXPECT_TYPE (&i_dict, DBUS_TYPE_DICT_ENTRY);
            if (_ag_iter_get_dict_entry (&i_dict, &key, &value))
            {
                g_hash_table_insert (sc->settings, g_strdup (key),
                                     g_slice_dup (GValue, &value));
            }
            dbus_message_iter_next (&i_dict);
        }
        dbus_message_iter_next (&i_struct);

        EXPECT_TYPE (&i_struct, DBUS_TYPE_ARRAY);
        dbus_message_iter_recurse (&i_struct, &i_list);

        /* iterate the "as" of removed settings */
        while (dbus_message_iter_get_arg_type (&i_list) != DBUS_TYPE_INVALID)
        {
            const gchar *key;
            EXPECT_TYPE (&i_list, DBUS_TYPE_STRING);
            dbus_message_iter_get_basic (&i_list, &key);
            g_hash_table_insert (sc->settings, g_strdup (key), NULL);
            dbus_message_iter_next (&i_list);
        }

        dbus_message_iter_next (&i_serv);
    }

#undef EXPECT_TYPE
    return changes;

error:
    g_warning ("Wrong format of D-Bus message");
    g_slice_free(AgAccountChanges, changes);
    return NULL;
}

static void
add_service_type (GPtrArray *types, const gchar *service_type)
{
    gboolean found = FALSE;
    gint i;

    /* if the service type is not yet in the list, add it */
    for (i = 0; i < types->len; i++)
    {
        if (strcmp (service_type, g_ptr_array_index (types, i)) == 0)
        {
            found = TRUE;
            break;
        }
    }

    if (!found)
        g_ptr_array_add (types, (gchar *)service_type);
}

/**
 * _ag_account_changes_get_service_types:
 * @changes: the #AgAccountChanges structure.
 *
 * Gets the list of service types involved in the change. The list does not
 * contain duplicates.
 *
 * Returns: a newly-allocated GPtrArray (this must be freed, but not the
 * strings it holds!).
 */
GPtrArray *
_ag_account_changes_get_service_types (AgAccountChanges *changes)
{
    GPtrArray *ret = g_ptr_array_sized_new (8);

    if (changes->services)
    {
        GHashTableIter iter;
        AgServiceChanges *sc;

        g_hash_table_iter_init (&iter, changes->services);
        while (g_hash_table_iter_next (&iter, NULL, (gpointer)&sc))
        {
            if (!sc->service_type) continue;

            add_service_type (ret, sc->service_type);
        }
    }

    /* if the account has been created or deleted, make sure that the global
     * service type is in the list */
    if (changes->created || changes->deleted)
        add_service_type (ret, SERVICE_GLOBAL_TYPE);

    return ret;
}

gboolean
_ag_account_changes_have_service_type (AgAccountChanges *changes, gchar *service_type)
{
    if (changes->services)
    {
        GHashTableIter iter;
        AgServiceChanges *sc;

        g_hash_table_iter_init (&iter, changes->services);
        while (g_hash_table_iter_next (&iter,
                                       NULL, (gpointer)&sc))
        {
            if (g_strcmp0(sc->service_type, service_type) == 0)
                return TRUE;
        }
    }

    return FALSE;
}

gboolean
_ag_account_changes_have_enabled (AgAccountChanges *changes)
{
    if (changes->services)
    {
        GHashTableIter iter;
        AgServiceChanges *sc;

        g_hash_table_iter_init (&iter, changes->services);
        while (g_hash_table_iter_next (&iter,
                                       NULL, (gpointer)&sc))
        {
            const gchar *key = "enabled";

            if (g_hash_table_lookup (sc->settings, (gpointer)key))
                return TRUE;
        }
    }

    return FALSE;
}

static void
ag_account_store_signature (AgAccount *account, AgServiceChanges *sc, GString *sql)
{
    AgAccountId account_id;
    GHashTableIter i_signatures;
    gint service_id;
    gpointer ht_key, ht_value;

    account_id = account->id;
    service_id = (sc->service != NULL) ? sc->service->id : 0;

    g_hash_table_iter_init (&i_signatures, sc->signatures);
    while (g_hash_table_iter_next (&i_signatures, &ht_key, &ht_value))
    {
        const gchar *key = ht_key;
        AgSignature *sgn = ht_value;

        if (sgn)
        {
            _ag_string_append_printf
                (sql,
                 "INSERT OR REPLACE INTO Signatures"
                 "(account, service, key, signature, token)"
                 "VALUES (%d, %d, %Q, %Q, %Q);",
                 account_id, service_id, key, sgn->signature, sgn->token);
        }
    }
}

static gchar *
ag_account_get_store_sql (AgAccount *account, GError **error)
{
    AgAccountPrivate *priv;
    AgAccountChanges *changes;
    GString *sql;
    gchar account_id_buffer[16];
    const gchar *account_id_str;

    priv = account->priv;

    if (G_UNLIKELY (priv->deleted))
    {
        *error = g_error_new (AG_ACCOUNTS_ERROR, AG_ACCOUNTS_ERROR_DELETED,
                              "Account %s (id = %d) has been deleted",
                              priv->display_name, account->id);
        return NULL;
    }

    changes = priv->changes;

    if (G_UNLIKELY (!changes))
    {
        /* Nothing to do: return no SQL, and no error */
        return NULL;
    }

    sql = g_string_sized_new (512);
    if (changes->deleted)
    {
        if (account->id != 0)
        {
            _ag_string_append_printf
                (sql, "DELETE FROM Accounts WHERE id = %d;", account->id);
            _ag_string_append_printf
                (sql, "DELETE FROM Settings WHERE account = %d;", account->id);
        }
        account_id_str = NULL; /* make the compiler happy */
    }
    else if (account->id == 0)
    {
        gboolean enabled;
        const gchar *display_name;

        ag_account_changes_get_enabled (changes, &enabled);
        ag_account_changes_get_display_name (changes, &display_name);
        _ag_string_append_printf
            (sql,
             "INSERT INTO Accounts (name, provider, enabled) "
             "VALUES (%Q, %Q, %d);",
             display_name,
             priv->provider_name,
             enabled);

        g_string_append (sql, "SELECT set_last_rowid_as_account_id();");
        account_id_str = "account_id()";
    }
    else
    {
        gboolean enabled, enabled_changed, display_name_changed;
        const gchar *display_name;

        g_snprintf (account_id_buffer, sizeof (account_id_buffer),
                    "%u", account->id);
        account_id_str = account_id_buffer;

        enabled_changed = ag_account_changes_get_enabled (changes, &enabled);
        display_name_changed =
            ag_account_changes_get_display_name (changes, &display_name);

        if (display_name_changed || enabled_changed)
        {
            gboolean comma = FALSE;
            g_string_append (sql, "UPDATE Accounts SET ");
            if (display_name_changed)
            {
                _ag_string_append_printf
                    (sql, "name = %Q", display_name);
                comma = TRUE;
            }

            if (enabled_changed)
            {
                _ag_string_append_printf
                    (sql, "%cenabled = %d",
                     comma ? ',' : ' ', enabled);
            }

            _ag_string_append_printf (sql, " WHERE id = %d;", account->id);
        }
    }

    if (!changes->deleted)
    {
        GHashTableIter i_services;
        gpointer ht_key, ht_value;

        g_hash_table_iter_init (&i_services, changes->services);
        while (g_hash_table_iter_next (&i_services, &ht_key, &ht_value))
        {
            AgServiceChanges *sc = ht_value;
            GHashTableIter i_settings;
            const gchar *service_id_str;
            gchar service_id_buffer[16];

            if (sc->service)
            {
                g_snprintf (service_id_buffer, sizeof (service_id_buffer),
                            "%d", sc->service->id);
                service_id_str = service_id_buffer;
            }
            else
                service_id_str = "0";

            g_hash_table_iter_init (&i_settings, sc->settings);
            while (g_hash_table_iter_next (&i_settings, &ht_key, &ht_value))
            {
                const gchar *key = ht_key;
                const GValue *value = ht_value;

                if (value)
                {
                    const gchar *type_str;
                    gchar *value_str;

                    value_str = _ag_value_to_db (value, FALSE);
                    type_str = _ag_type_from_g_type (G_VALUE_TYPE (value));
                    _ag_string_append_printf
                        (sql,
                         "INSERT OR REPLACE INTO Settings (account, service,"
                                                          "key, type, value) "
                         "VALUES (%s, %s, %Q, %Q, %Q);",
                         account_id_str, service_id_str, key,
                         type_str, value_str);
                    g_free (value_str);
                }
                else if (account->id != 0)
                {
                    _ag_string_append_printf
                        (sql,
                         "DELETE FROM Settings WHERE "
                         "account = %d AND "
                         "service = %Q AND "
                         "key = %Q;",
                         account->id, service_id_str, key);
                }
            }

            if (sc->signatures)
                ag_account_store_signature (account, sc, sql);
        }
    }

    return g_string_free (sql, FALSE);
}

/**
 * ag_account_supports_service:
 * @account: the #AgAccount.
 * @service_type: the name of the service type to check for
 *
 * Get whether @service_type is supported on @account.
 *
 * Returns: %TRUE if @account supports the service type @service_type, %FALSE
 * otherwise.
 */
gboolean
ag_account_supports_service (AgAccount *account, const gchar *service_type)
{
    GList *services;
    gboolean ret = FALSE;

    services = ag_account_list_services_by_type (account, service_type);
    if (services)
    {
        ag_service_list_free (services);
        ret = TRUE;
    }
    return ret;
}

/**
 * ag_account_list_services:
 * @account: the #AgAccount.
 *
 * Get the list of services for @account. If the #AgManager was created with
 * specified service_type this will return only services with this service_type.
 *
 * Returns: (transfer full) (element-type AgService): a #GList of #AgService
 * items representing all the services supported by this account. Must be
 * free'd with ag_service_list_free().
 */
GList *
ag_account_list_services (AgAccount *account)
{
    AgAccountPrivate *priv;
    GList *all_services, *list;
    GList *services = NULL;

    g_return_val_if_fail (AG_IS_ACCOUNT (account), NULL);
    priv = account->priv;

    if (!priv->provider_name)
        return NULL;

    all_services = ag_manager_list_services (priv->manager);
    for (list = all_services; list != NULL; list = list->next)
    {
        AgService *service = list->data;

        const gchar *provider = ag_service_get_provider (service);
        if (provider &&
            strcmp (provider, priv->provider_name) == 0)
        {
            services = g_list_prepend (services, service);
        }
        else
            ag_service_unref (service);
    }
    g_list_free (all_services);
    return services;
}

/**
 * ag_account_list_services_by_type:
 * @account: the #AgAccount.
 * @service_type: the service type which all the returned services should
 * provide.
 *
 * Get the list of services supported by @account, filtered by @service_type.
 *
 * Returns: (transfer full) (element-type AgService): a #GList of #AgService
 * items representing all the services supported by this account which provide
 * @service_type. Must be free'd with ag_service_list_free().
 */
GList *
ag_account_list_services_by_type (AgAccount *account,
                                  const gchar *service_type)
{
    AgAccountPrivate *priv;
    GList *all_services, *list;
    GList *services = NULL;

    g_return_val_if_fail (AG_IS_ACCOUNT (account), NULL);
    g_return_val_if_fail (service_type != NULL, NULL);
    priv = account->priv;

    if (!priv->provider_name)
        return NULL;

    all_services = ag_manager_list_services_by_type (priv->manager, service_type);
    for (list = all_services; list != NULL; list = list->next)
    {
        AgService *service = list->data;
        const gchar *provider = ag_service_get_provider (service);
        if (provider &&
            strcmp (provider, priv->provider_name) == 0)
        {
            services = g_list_prepend (services, service);
        }
        else
            ag_service_unref (service);
    }
    g_list_free (all_services);
    return services;
}

static gboolean
add_name_to_list (sqlite3_stmt *stmt, GList **plist)
{
    gchar *name;
    name = g_strdup ((gchar *)sqlite3_column_text (stmt, 0));

    *plist = g_list_prepend(*plist, name);

    return TRUE;
}

static inline GList *
list_enabled_services_from_memory (AgAccountPrivate *priv,
                                   const gchar *service_type)
{
    GHashTableIter iter;
    AgServiceSettings *ss;
    GList *list = NULL;

    g_hash_table_iter_init (&iter, priv->services);
    while (g_hash_table_iter_next (&iter, NULL, (gpointer)&ss))
    {
        GValue *value;

        if (ss->service == NULL) continue;

        if (service_type != NULL &&
            g_strcmp0 (ag_service_get_service_type (ss->service), service_type) != 0)
                continue;

        value = g_hash_table_lookup (ss->settings, "enabled");
        if (value != NULL && g_value_get_boolean (value))
            list = g_list_prepend (list, ag_service_ref(ss->service));
    }
    return list;
}

static AgAccountSettingIter *
ag_account_settings_iter_copy(const AgAccountSettingIter *orig)
{
    return g_slice_dup (AgAccountSettingIter, orig);
}

G_DEFINE_BOXED_TYPE (AgAccountSettingIter, ag_account_settings_iter,
                     (GBoxedCopyFunc)ag_account_settings_iter_copy,
                     (GBoxedFreeFunc)ag_account_settings_iter_free);

void
_ag_account_settings_iter_init (AgAccount *account,
                                AgAccountSettingIter *iter,
                                const gchar *key_prefix,
                                gboolean copy_string)
{
    AgAccountPrivate *priv;
    AgServiceSettings *ss;
    RealIter *ri = (RealIter *)iter;

    g_return_if_fail (AG_IS_ACCOUNT (account));
    g_return_if_fail (iter != NULL);
    priv = account->priv;

    ri->account = account;
    if (copy_string)
    {
        ri->key_prefix = g_strdup (key_prefix);
        ri->must_free_prefix = TRUE;
    }
    else
    {
        ri->key_prefix = (gchar *)key_prefix;
        ri->must_free_prefix = FALSE;
    }
    ri->stage = AG_ITER_STAGE_UNSET;

    ss = get_service_settings (priv, priv->service, FALSE);
    if (ss)
    {
        g_hash_table_iter_init (&ri->iter, ss->settings);
        ri->stage = AG_ITER_STAGE_ACCOUNT;
    }
}

/**
 * ag_account_list_enabled_services:
 * @account: the #AgAccount.
 *
 * Gets a list of services that are enabled for @account.
 *
 * Returns: (transfer full) (element-type AgService): a #GList of #AgService
 * items representing all the services which are enabled. Must be free'd with
 * ag_service_list_free().
 */
GList *
ag_account_list_enabled_services (AgAccount *account)
{
    AgAccountPrivate *priv;
    GList *list = NULL;
    GList *iter;
    GList *services = NULL;
    const gchar *service_type;
    char sql[512];

    g_return_val_if_fail (AG_IS_ACCOUNT (account), NULL);
    priv = account->priv;

    service_type = ag_manager_get_service_type (priv->manager);

    /* avoid accessing the DB, if possible */
    if (priv->foreign)
        return list_enabled_services_from_memory (priv, service_type);

    if (service_type != NULL)
        sqlite3_snprintf (sizeof (sql), sql,
                          "SELECT DISTINCT Services.name FROM Services "
                          "JOIN Settings ON Settings.service = Services.id "
                          "WHERE Settings.key='enabled' "
                          "AND Settings.value='true' "
                          "AND Settings.account='%d' "
                          "AND Services.type = '%s';",
                           account->id,
                           service_type);
    else
        sqlite3_snprintf (sizeof (sql), sql,
                          "SELECT DISTINCT Services.name FROM Services "
                          "JOIN Settings ON Settings.service = Services.id "
                          "WHERE Settings.key='enabled' "
                          "AND Settings.value='true' "
                          "AND Settings.account='%d';",
                           account->id);

    _ag_manager_exec_query (priv->manager, (AgQueryCallback)add_name_to_list,
                            &list, sql);

    for (iter = list; iter != NULL; iter = iter->next)
    {
        gchar *service_name;
        AgService *service;

        service_name = (gchar*)iter->data;
        service = ag_manager_get_service (priv->manager, service_name);

        services = g_list_prepend (services, service);
        g_free (service_name);
    }

    g_list_free (list);

    return services;
}

/**
 * ag_account_get_manager:
 * @account: the #AgAccount.
 *
 * Get the #AgManager for @account.
 *
 * Returns: (transfer none): the #AgManager.
 */
AgManager *
ag_account_get_manager (AgAccount *account)
{
    g_return_val_if_fail (AG_IS_ACCOUNT (account), NULL);
    return account->priv->manager;
}

/**
 * ag_account_get_provider_name:
 * @account: the #AgAccount.
 *
 * Get the name of the provider of @account.
 *
 * Returns: the name of the provider.
 */
const gchar *
ag_account_get_provider_name (AgAccount *account)
{
    g_return_val_if_fail (AG_IS_ACCOUNT (account), NULL);
    return account->priv->provider_name;
}

/**
 * ag_account_get_display_name:
 * @account: the #AgAccount.
 *
 * Get the display name of @account.
 *
 * Returns: the display name.
 */
const gchar *
ag_account_get_display_name (AgAccount *account)
{
    g_return_val_if_fail (AG_IS_ACCOUNT (account), NULL);
    return account->priv->display_name;
}

/**
 * ag_account_set_display_name:
 * @account: the #AgAccount.
 * @display_name: the display name to set.
 *
 * Changes the display name for @account to @display_name.
 */
void
ag_account_set_display_name (AgAccount *account, const gchar *display_name)
{
    GValue value = { 0 };

    g_return_if_fail (AG_IS_ACCOUNT (account));

    g_value_init (&value, G_TYPE_STRING);
    g_value_set_static_string (&value, display_name);
    change_service_value (account->priv, NULL, "name", &value);
}

/**
 * ag_account_select_service:
 * @account: the #AgAccount.
 * @service: (allow-none): the #AgService to select.
 *
 * Selects the configuration of service @service: from now on, all the
 * subsequent calls on the #AgAccount configuration will act on the @service.
 * If @service is %NULL, the global account configuration is selected.
 *
 * Note that if @account is being shared with other code one must take special
 * care to make sure the desired service is always selected.
 */
void
ag_account_select_service (AgAccount *account, AgService *service)
{
    AgAccountPrivate *priv;
    gboolean load_settings = FALSE;
    AgServiceSettings *ss;

    g_return_if_fail (AG_IS_ACCOUNT (account));
    priv = account->priv;

    priv->service = service;

    if (account->id != 0 &&
        !get_service_settings (priv, service, FALSE))
    {
        /* the settings for this service are not yet loaded: do it now */
        load_settings = TRUE;
    }

    ss = get_service_settings (priv, service, TRUE);

    if (load_settings)
    {
        guint service_id;
        gchar sql[128];

        service_id = _ag_manager_get_service_id (priv->manager, service);
        g_snprintf (sql, sizeof (sql),
                    "SELECT key, type, value FROM Settings "
                    "WHERE account = %u AND service = %u",
                    account->id, service_id);
        _ag_manager_exec_query (priv->manager,
                                (AgQueryCallback)got_account_setting,
                                ss->settings, sql);
    }
}

/**
 * ag_account_get_selected_service:
 * @account: the #AgAccount.
 *
 * Gets the selected #AgService for @account.
 *
 * Returns: the selected service, or %NULL if no service is selected.
 */
AgService *
ag_account_get_selected_service (AgAccount *account)
{
    g_return_val_if_fail (AG_IS_ACCOUNT (account), NULL);
    return account->priv->service;
}

/**
 * ag_account_get_enabled:
 * @account: the #AgAccount.
 *
 * Gets whether the selected service is enabled for @account.
 *
 * Returns: %TRUE if the selected service for @account is enabled, %FALSE
 * otherwise.
 */
gboolean
ag_account_get_enabled (AgAccount *account)
{
    AgAccountPrivate *priv;
    gboolean ret = FALSE;
    AgServiceSettings *ss;
    GValue *val;

    g_return_val_if_fail (AG_IS_ACCOUNT (account), FALSE);
    priv = account->priv;

    if (priv->service == NULL)
    {
        ret = priv->enabled;
    }
    else
    {
        ss = get_service_settings (priv, priv->service, FALSE);
        if (ss)
        {
            val = g_hash_table_lookup (ss->settings, "enabled");
            ret = val ? g_value_get_boolean (val) : FALSE;
        }
    }
    return ret;
}

/**
 * ag_account_set_enabled:
 * @account: the #AgAccount.
 * @enabled: whether @account should be enabled.
 *
 * Sets the "enabled" flag on the selected service for @account.
 */
void
ag_account_set_enabled (AgAccount *account, gboolean enabled)
{
    GValue value = { 0 };

    g_return_if_fail (AG_IS_ACCOUNT (account));

    g_value_init (&value, G_TYPE_BOOLEAN);
    g_value_set_boolean (&value, enabled);
    change_selected_service_value (account->priv, "enabled", &value);
}

/**
 * ag_account_delete:
 * @account: the #AgAccount.
 *
 * Deletes the account. Call ag_account_store() in order to record the change
 * in the storage.
 */
void
ag_account_delete (AgAccount *account)
{
    AgAccountChanges *changes;

    g_return_if_fail (AG_IS_ACCOUNT (account));

    changes = account_changes_get (account->priv);
    changes->deleted = TRUE;
}

/**
 * ag_account_get_value:
 * @account: the #AgAccount.
 * @key: the name of the setting to retrieve.
 * @value: (inout): an initialized #GValue to receive the setting's value.
 *
 * Gets the value of the configuration setting @key: @value must be a
 * #GValue initialized to the type of the setting.
 *
 * Returns: one of #AgSettingSource: %AG_SETTING_SOURCE_NONE if the setting is
 * not present, %AG_SETTING_SOURCE_ACCOUNT if the setting comes from the
 * account configuration, or %AG_SETTING_SOURCE_PROFILE if the value comes as
 * predefined in the profile.
 */
AgSettingSource
ag_account_get_value (AgAccount *account, const gchar *key,
                      GValue *value)
{
    AgAccountPrivate *priv;
    AgServiceSettings *ss;
    AgSettingSource source;
    const GValue *val = NULL;

    g_return_val_if_fail (AG_IS_ACCOUNT (account), AG_SETTING_SOURCE_NONE);
    priv = account->priv;

    ss = get_service_settings (priv, priv->service, FALSE);
    if (ss)
    {
        val = g_hash_table_lookup (ss->settings, key);
        source = AG_SETTING_SOURCE_ACCOUNT;
    }

    if (!val && priv->service)
    {
        val = _ag_service_get_default_setting (priv->service, key);
        source = AG_SETTING_SOURCE_PROFILE;
    }

    if (val)
    {
        if (G_VALUE_TYPE (val) == G_VALUE_TYPE (value))
            g_value_copy (val, value);
        else
            g_value_transform (val, value);
        return source;
    }

    return AG_SETTING_SOURCE_NONE;
}

/**
 * ag_account_set_value:
 * @account: the #AgAccount.
 * @key: the name of the setting to change.
 * @value: (allow-none): a #GValue holding the new setting's value.
 *
 * Sets the value of the configuration setting @key to the value @value.
 * If @value is %NULL, then the setting is unset.
 */
void
ag_account_set_value (AgAccount *account, const gchar *key,
                      const GValue *value)
{
    AgAccountPrivate *priv;

    g_return_if_fail (AG_IS_ACCOUNT (account));
    priv = account->priv;

    change_selected_service_value (priv, key, value);
}

/**
 * ag_account_get_settings_iter:
 * @account: the #AgAccount.
 * @key_prefix: (allow-none): enumerate only the settings whose key starts with
 * @key_prefix.
 *
 * Creates a new iterator. This method is useful for language bindings only.
 *
 * Returns: (transfer full): an #AgAccountSettingIter.
 */
AgAccountSettingIter *
ag_account_get_settings_iter (AgAccount *account,
                              const gchar *key_prefix)
{
    AgAccountSettingIter *iter = g_slice_new (AgAccountSettingIter);
    _ag_account_settings_iter_init (account, iter, key_prefix, TRUE);
    return iter;
}

/**
 * ag_account_settings_iter_free:
 * @iter: a #AgAccountSettingIter.
 *
 * Frees the memory associated with an #AgAccountSettingIter.
 */
void
ag_account_settings_iter_free (AgAccountSettingIter *iter)
{
    if (iter == NULL) return;

    RealIter *ri = (RealIter *)iter;
    if (ri->must_free_prefix)
        g_free (ri->key_prefix);
    g_slice_free (AgAccountSettingIter, iter);
}

/**
 * ag_account_settings_iter_init:
 * @account: the #AgAccount.
 * @iter: an uninitialized #AgAccountSettingIter structure.
 * @key_prefix: (allow-none): enumerate only the settings whose key starts with
 * @key_prefix.
 *
 * Initializes @iter to iterate over the account settings. If @key_prefix is
 * not %NULL, only keys whose names start with @key_prefix will be iterated
 * over.
 */
void
ag_account_settings_iter_init (AgAccount *account,
                               AgAccountSettingIter *iter,
                               const gchar *key_prefix)
{
    _ag_account_settings_iter_init (account, iter, key_prefix, FALSE);
}

/**
 * ag_account_settings_iter_next:
 * @iter: an initialized #AgAccountSettingIter structure.
 * @key: (out callee-allocates) (transfer none): a pointer to a string
 * receiving the key name.
 * @value: (out callee-allocates) (transfer none): a pointer to a pointer to a
 * #GValue, to receive the key value.
 *
 * Iterates over the account keys. @iter must be an iterator previously
 * initialized with ag_account_settings_iter_init().
 *
 * Returns: %TRUE if @key and @value have been set, %FALSE if we there are no
 * more account settings to iterate over.
 */
gboolean
ag_account_settings_iter_next (AgAccountSettingIter *iter,
                               const gchar **key, const GValue **value)
{
    RealIter *ri = (RealIter *)iter;
    AgServiceSettings *ss;
    AgAccountPrivate *priv;
    gint prefix_length;

    g_return_val_if_fail (iter != NULL, FALSE);
    g_return_val_if_fail (AG_IS_ACCOUNT (iter->account), FALSE);
    g_return_val_if_fail (key != NULL && value != NULL, FALSE);
    priv = iter->account->priv;

    prefix_length = ri->key_prefix ? strlen(ri->key_prefix) : 0;

    if (ri->stage == AG_ITER_STAGE_ACCOUNT)
    {
        while (g_hash_table_iter_next (&ri->iter,
                                       (gpointer *)key, (gpointer *)value))
        {
            if (ri->key_prefix && !g_str_has_prefix (*key, ri->key_prefix))
                continue;

            *key = *key + prefix_length;
            return TRUE;
        }
        ri->stage = AG_ITER_STAGE_UNSET;
    }

    if (!priv->service)
    {
        *key = NULL;
        *value = NULL;
        return FALSE;
    }

    if (ri->stage == AG_ITER_STAGE_UNSET)
    {
        GHashTable *settings;

        settings = _ag_service_load_default_settings (priv->service);
        if (!settings) return FALSE;

        g_hash_table_iter_init (&ri->iter, settings);
        ri->stage = AG_ITER_STAGE_SERVICE;
    }

    ss = get_service_settings (priv, priv->service, FALSE);
    while (g_hash_table_iter_next (&ri->iter,
                                   (gpointer *)key, (gpointer *)value))
    {
        if (ri->key_prefix && !g_str_has_prefix (*key, ri->key_prefix))
            continue;

        /* if the setting is also on the account, it is overriden and we must
         * not return it here */
        if (ss && g_hash_table_lookup (ss->settings, *key) != NULL)
            continue;

        *key = *key + prefix_length;
        return TRUE;
    }

    *key = NULL;
    *value = NULL;
    return FALSE;
}

/**
 * AgAccountNotifyCb:
 * @account: the #AgAccount.
 * @key: the name of the key whose value has changed.
 * @user_data: the user data that was passed when installing this callback.
 *
 * This callback is invoked when the value of an account configuration setting
 * changes. If the callback was installed with ag_account_watch_key() then @key
 * is the name of the configuration setting which changed; if it was installed
 * with ag_account_watch_dir() then @key is the same key prefix that was used
 * when installing this callback.
 */

/**
 * ag_account_watch_key:
 * @account: the #AgAccount.
 * @key: the name of the key to watch.
 * @callback: (scope async): a #AgAccountNotifyCb callback to be called.
 * @user_data: pointer to user data, to be passed to @callback.
 *
 * Installs a watch on @key: @callback will be invoked whenever the value of
 * @key changes (or the key is removed).
 *
 * Returns: (transfer none): a #AgAccountWatch, which can then be used to
 * remove this watch.
 */
AgAccountWatch
ag_account_watch_key (AgAccount *account, const gchar *key,
                      AgAccountNotifyCb callback, gpointer user_data)
{
    g_return_val_if_fail (AG_IS_ACCOUNT (account), NULL);
    g_return_val_if_fail (key != NULL, NULL);
    g_return_val_if_fail (callback != NULL, NULL);

    return ag_account_watch_int (account, g_strdup (key), NULL,
                                 callback, user_data);
}

/**
 * ag_account_watch_dir:
 * @account: the #AgAccount.
 * @key_prefix: the prefix of the keys to watch.
 * @callback: (scope async): a #AgAccountNotifyCb callback to be called.
 * @user_data: pointer to user data, to be passed to @callback.
 *
 * Installs a watch on all the keys under @key_prefix: @callback will be
 * invoked whenever the value of any of these keys changes (or a key is
 * removed).
 *
 * Returns: (transfer none): a #AgAccountWatch, which can then be used to
 * remove this watch.
 */
AgAccountWatch
ag_account_watch_dir (AgAccount *account, const gchar *key_prefix,
                      AgAccountNotifyCb callback, gpointer user_data)
{
    g_return_val_if_fail (AG_IS_ACCOUNT (account), NULL);
    g_return_val_if_fail (key_prefix != NULL, NULL);
    g_return_val_if_fail (callback != NULL, NULL);

    return ag_account_watch_int (account, NULL, g_strdup (key_prefix),
                                 callback, user_data);
}

/**
 * ag_account_remove_watch:
 * @account: the #AgAccount.
 * @watch: the watch to remove.
 *
 * Removes the notification callback identified by @watch.
 */
void
ag_account_remove_watch (AgAccount *account, AgAccountWatch watch)
{
    AgAccountPrivate *priv;
    GHashTable *service_watches;

    g_return_if_fail (AG_IS_ACCOUNT (account));
    g_return_if_fail (watch != NULL);
    priv = account->priv;

    if (G_LIKELY (priv->watches))
    {
        service_watches = g_hash_table_lookup (priv->watches, watch->service);
        if (G_LIKELY (service_watches &&
                      g_hash_table_remove (service_watches, watch)))
            return; /* success */
    }

    g_warning ("Watch %p not found", watch);
}

/**
 * AgAccountStoreCb:
 * @account: the #AgAccount.
 * @error: a #GError, or %NULL.
 * @user_data: the user data that was passed to ag_account_store().
 *
 * This callback is invoked when storing the account settings is completed. If
 * @error is not %NULL, then some error occurred and the data has most likely
 * not been written.
 */

/**
 * ag_account_store:
 * @account: the #AgAccount.
 * @callback: (scope async): function to be called when the settings have been
 * written.
 * @user_data: pointer to user data, to be passed to @callback.
 *
 * Store the account settings which have been changed into the account
 * database, and invoke @callback when the operation has been completed.
 */
void
ag_account_store (AgAccount *account, AgAccountStoreCb callback,
                  gpointer user_data)
{
    AgAccountPrivate *priv;
    AgAccountChanges *changes;
    GError *error = NULL;
    gchar *sql;

    g_return_if_fail (AG_IS_ACCOUNT (account));
    priv = account->priv;

    sql = ag_account_get_store_sql (account, &error);
    if (G_UNLIKELY (error))
    {
        if (callback)
            callback (account, error, user_data);
        else
            g_warning ("%s: %s", G_STRFUNC, error->message);

        g_error_free (error);
        return;
    }

    changes = priv->changes;
    priv->changes = NULL;

    if (G_UNLIKELY (!sql))
    {
        /* Nothing to do: invoke the callback immediately */
        if (callback)
            callback (account, NULL, user_data);
        return;
    }

    _ag_manager_exec_transaction (priv->manager, sql, changes, account,
                                  callback, user_data);
    g_free (sql);
}

/**
 * ag_account_store_blocking:
 * @account: the #AgAccount.
 * @error: pointer to receive the #GError, or %NULL.
 *
 * Store the account settings which have been changed into the account
 * database. This function does not return until the operation has completed.
 *
 * Returns: %TRUE on success, %FALSE on failure.
 */
gboolean
ag_account_store_blocking (AgAccount *account, GError **error)
{
    AgAccountPrivate *priv;
    AgAccountChanges *changes;
    GError *error_int = NULL;
    gchar *sql;

    g_return_val_if_fail (AG_IS_ACCOUNT (account), FALSE);
    priv = account->priv;

    sql = ag_account_get_store_sql (account, &error_int);
    if (G_UNLIKELY (error_int))
    {
        g_warning ("%s: %s", G_STRFUNC, error_int->message);
        g_propagate_error (error, error_int);
        return FALSE;
    }

    changes = priv->changes;
    priv->changes = NULL;

    if (G_UNLIKELY (!sql))
    {
        /* Nothing to do: return immediately */
        return TRUE;
    }

    _ag_manager_exec_transaction_blocking (priv->manager, sql,
                                           changes, account,
                                           &error_int);
    g_free (sql);
    _ag_account_changes_free (changes);

    if (G_UNLIKELY (error_int))
    {
        g_warning ("%s: %s", G_STRFUNC, error_int->message);
        g_propagate_error (error, error_int);
        return FALSE;
    }

    return TRUE;
}

/**
 * ag_account_sign:
 * @account: the #AgAccount.
 * @key: the name of the key or prefix of the keys to be signed.
 * @token: a signing token (%NULL-terminated string) for creating the
 * signature. The application must possess (request) the token.
 *
 * Creates signature of the @key with given @token. The account must be
 * stored prior to calling this function.
 */
void
ag_account_sign (AgAccount *account, const gchar *key, const gchar *token)
{
    g_warning ("ag_account_sign: no encryptor supported.");
}

/**
 * ag_account_verify:
 * @account: the #AgAccount.
 * @key: the name of the key or prefix of the keys to be verified.
 * @token: location to receive the pointer to aegis token.
 *
 * Verify if the key is signed and the signature matches the value
 * and provides the aegis token which was used for signing the @key.
 *
 * Returns: %TRUE if the key is signed and the signature matches the value,
 * %FALSE otherwise.
 */
gboolean
ag_account_verify (AgAccount *account, const gchar *key, const gchar **token)
{
    g_warning ("ag_account_verify: no encryptor supported.");
    return FALSE;
}

/**
 * ag_account_verify_with_tokens:
 * @account: the #AgAccount.
 * @key: the name of the key or prefix of the keys to be verified.
 * @tokens: array of aegis tokens.
 *
 * Verify if the @key is signed with any of the tokens from the @tokens
 * and the signature is valid.
 *
 * Returns: %TRUE if the key is signed with any of the given tokens and the
 * signature is valid, %FALSE otherwise.
 */
gboolean
ag_account_verify_with_tokens (AgAccount *account, const gchar *key, const gchar **tokens)
{
    g_return_val_if_fail (AG_IS_ACCOUNT (account), FALSE);

    const gchar *tmp_token = NULL;

    g_return_val_if_fail (tokens != NULL, FALSE);

    if (ag_account_verify (account, key, &tmp_token))
    {
        g_return_val_if_fail (tmp_token != NULL, FALSE);

        while (*tokens != NULL)
        {
            if (strcmp (tmp_token, *tokens) == 0)
            {
                return TRUE;
            }
            tokens++;
        }
    }

    return FALSE;
}

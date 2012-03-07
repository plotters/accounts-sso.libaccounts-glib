/* vi: set et sw=4 ts=4 cino=t0,(0: */
/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of libaccounts-glib
 *
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
 * SECTION:ag-auth-data
 * @short_description: information for account authentication.
 * @include: libaccounts-glib/ag-auth-data.h
 *
 * The #AgAuthData structure holds information on the authentication
 * parameters used by an account. It is created by
 * ag_account_service_get_auth_data(), and can be destroyed with
 * ag_auth_data_unref().
 */

#include "ag-auth-data.h"
#include "ag-internals.h"
#include "ag-util.h"

struct _AgAuthData {
    /*< private >*/
    gint ref_count;
    guint credentials_id;
    gchar *method;
    gchar *mechanism;
    GHashTable *parameters;
};

G_DEFINE_BOXED_TYPE (AgAuthData, ag_auth_data,
                     (GBoxedCopyFunc)ag_auth_data_ref,
                     (GBoxedFreeFunc)ag_auth_data_unref);

static gboolean
get_value_with_fallback (AgAccount *account, AgService *service,
                         const gchar *key, GValue *value)
{
    ag_account_select_service (account, service);
    if (ag_account_get_value (account, key, value) == AG_SETTING_SOURCE_NONE)
    {
        /* fallback to the global account */
        ag_account_select_service (account, NULL);
        if (ag_account_get_value (account, key, value) ==
            AG_SETTING_SOURCE_NONE)
            return FALSE;
    }

    return TRUE;
}

static gchar *
get_string_with_fallback (AgAccount *account, AgService *service,
                          const gchar *key)
{
    GValue value = {0, };
    gchar *ret;

    g_value_init(&value, G_TYPE_STRING);
    if (!get_value_with_fallback (account, service, key, &value))
        return NULL;

    ret = g_value_dup_string (&value);
    g_value_unset (&value);
    return ret;
}

static guint
get_uint_with_fallback (AgAccount *account, AgService *service,
                        const gchar *key)
{
    GValue value = {0, };

    g_value_init(&value, G_TYPE_UINT);
    if (!get_value_with_fallback (account, service, key, &value))
        return 0;

    return g_value_get_uint (&value);
}

static void
read_auth_settings (AgAccount *account, const gchar *key_prefix,
                    GHashTable *out)
{
    AgAccountSettingIter iter;
    const gchar *key;
    const GValue *value;

    ag_account_settings_iter_init (account, &iter, key_prefix);
    while (ag_account_settings_iter_next (&iter, &key, &value))
    {
        g_hash_table_insert (out, g_strdup (key), _ag_value_slice_dup (value));
    }
}

AgAuthData *
_ag_auth_data_new (AgAccount *account, AgService *service)
{
    guint credentials_id;
    gchar *method, *mechanism;
    gchar *key_prefix;
    GHashTable *parameters;
    AgAuthData *data = NULL;

    g_return_val_if_fail (account != NULL, NULL);
    g_return_val_if_fail (service != NULL, NULL);

    credentials_id = get_uint_with_fallback (account, service, "CredentialsId");

    method = get_string_with_fallback (account, service, "auth/method");
    if (method == NULL)
        return NULL;

    mechanism = get_string_with_fallback (account, service, "auth/mechanism");
    if (mechanism == NULL)
    {
        g_free (method);
        return NULL;
    }

    parameters = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        g_free,
                                        (GDestroyNotify)_ag_value_slice_free);
    key_prefix = g_strdup_printf ("auth/%s/%s/", method, mechanism);

    /* first, take the values from the global account */
    ag_account_select_service (account, NULL);
    read_auth_settings (account, key_prefix, parameters);

    /* next, the service-specific authentication settings */
    ag_account_select_service (account, service);
    read_auth_settings (account, key_prefix, parameters);

    g_free (key_prefix);

    data = g_slice_new (AgAuthData);
    data->credentials_id = credentials_id;
    data->method = method;
    data->mechanism = mechanism;
    data->parameters = parameters;
    return data;
}

/**
 * ag_auth_data_ref:
 * @self: the #AgAuthData.
 *
 * Increment the reference count of @self.
 *
 * Returns: @self.
 */
AgAuthData *
ag_auth_data_ref (AgAuthData *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    g_atomic_int_inc (&self->ref_count);
    return self;
}

/**
 * ag_auth_data_unref:
 * @self: the #AgAuthData.
 *
 * Decrements the reference count of @self. The item is destroyed when the
 * count gets to 0.
 */
void
ag_auth_data_unref (AgAuthData *self)
{
    g_return_if_fail (self != NULL);
    if (g_atomic_int_dec_and_test (&self->ref_count))
    {
        g_free (self->method);
        g_free (self->mechanism);
        g_hash_table_unref (self->parameters);
        g_slice_free (AgAuthData, self);
    }
}

/**
 * ag_auth_data_get_credentials_id:
 * @self: the #AgAuthData.
 *
 * Gets the ID of the credentials associated with this account.
 *
 * Returns: the credentials ID.
 */
guint
ag_auth_data_get_credentials_id (AgAuthData *self)
{
    g_return_val_if_fail (self != NULL, 0);
    return self->credentials_id;
}

/**
 * ag_auth_data_get_method:
 * @self: the #AgAuthData.
 *
 * Gets the authentication method.
 *
 * Returns: the authentication method.
 */
const gchar *
ag_auth_data_get_method (AgAuthData *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->method;
}

/**
 * ag_auth_data_get_mechanism:
 * @self: the #AgAuthData.
 *
 * Gets the authentication mechanism.
 *
 * Returns: the authentication mechanism.
 */
const gchar *
ag_auth_data_get_mechanism (AgAuthData *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->mechanism;
}

/**
 * ag_auth_data_get_parameters:
 * @self: the #AgAuthData.
 *
 * Gets the authentication parameters.
 *
 * Returns: (transfer none) (element-type utf8 GValue): a #GHashTable
 * containing all the authentication parameters.
 */
GHashTable *
ag_auth_data_get_parameters (AgAuthData *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->parameters;
}

/**
 * ag_auth_data_insert_parameters:
 * @self: the #AgAuthData.
 * @parameters: (transfer none) (element-type utf8 GValue): a #GHashTable
 * containing the authentication parameters to be added.
 *
 * Insert the given authentication parameters into the authentication data. If
 * some parameters were already present, the parameters passed with this method
 * take precedence.
 */
void
ag_auth_data_insert_parameters (AgAuthData *self, GHashTable *parameters)
{
    GHashTableIter iter;
    const gchar *key;
    const GValue *value;

    g_return_if_fail (self != NULL);
    g_return_if_fail (parameters != NULL);

    g_hash_table_iter_init (&iter, parameters);
    while (g_hash_table_iter_next (&iter, (gpointer)&key, (gpointer)&value))
    {
        g_hash_table_insert (self->parameters,
                             g_strdup (key),
                             _ag_value_slice_dup (value));
    }
}

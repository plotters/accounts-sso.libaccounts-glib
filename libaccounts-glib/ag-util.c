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

#include "ag-util.h"
#include "ag-debug.h"
#include "ag-errors.h"

#include <dbus/dbus.h>
#include <gio/gio.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GString *
_ag_string_append_printf (GString *string, const gchar *format, ...)
{
    va_list ap;
    char *sql;

    va_start (ap, format);
    sql = sqlite3_vmprintf (format, ap);
    va_end (ap);

    if (sql)
    {
        g_string_append (string, sql);
        sqlite3_free (sql);
    }

    return string;
}

GValue *
_ag_value_slice_dup (const GValue *value)
{
    GValue *copy;

    if (!value) return NULL;
    copy = g_slice_new0 (GValue);
    g_value_init (copy, G_VALUE_TYPE (value));
    g_value_copy (value, copy);
    return copy;
}

void
_ag_value_slice_free (GValue *value)
{
    if (!value) return;
    g_value_unset (value);
    g_slice_free (GValue, value);
}

gchar *
_ag_value_to_db (const GValue *in_value, gboolean type_annotate)
{
    GVariant *variant;
    const gchar *type;
    gchar *string;
    GValue transformed_value = { 0, };
    const GValue *value;

    g_return_val_if_fail (in_value != NULL, NULL);

    /* transform some GValues which g_dbus_gvalue_to_gvariant() cannot handle */
    if (G_VALUE_TYPE (in_value) == G_TYPE_CHAR)
    {
        g_value_init (&transformed_value, G_TYPE_INT);
        if (G_UNLIKELY (!g_value_transform (in_value, &transformed_value)))
        {
            g_warning ("%s: could not transform %s to %s", G_STRFUNC,
                       G_VALUE_TYPE_NAME (in_value),
                       G_VALUE_TYPE_NAME (&transformed_value));
            return NULL;
        }

        value = &transformed_value;
    }
    else
    {
        value = in_value;
    }

    type = _ag_type_from_g_type (G_VALUE_TYPE (value));
    variant = g_dbus_gvalue_to_gvariant (value, (GVariantType *)type);
    if (G_UNLIKELY (variant == NULL))
    {
        g_warning ("%s: unsupported type ``%s''", G_STRFUNC, type);
        return NULL;
    }

    string = g_variant_print (variant, type_annotate);
    g_variant_unref (variant);
    return string;
}

const gchar *
_ag_type_from_g_type (GType type)
{
    switch (type)
    {
    case G_TYPE_STRING:
        return DBUS_TYPE_STRING_AS_STRING;
    case G_TYPE_INT:
    case G_TYPE_CHAR:
        return DBUS_TYPE_INT32_AS_STRING;
    case G_TYPE_UINT:
        return DBUS_TYPE_UINT32_AS_STRING;
    case G_TYPE_BOOLEAN:
        return DBUS_TYPE_BOOLEAN_AS_STRING;
    case G_TYPE_UCHAR:
        return DBUS_TYPE_BYTE_AS_STRING;
    case G_TYPE_INT64:
        return DBUS_TYPE_INT64_AS_STRING;
    case G_TYPE_UINT64:
        return DBUS_TYPE_UINT64_AS_STRING;
    default:
        /* handle dynamic types here */
        if (type == G_TYPE_STRV)
            return DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_STRING_AS_STRING;

        g_warning ("%s: unsupported type ``%s''", G_STRFUNC,
                   g_type_name (type));
        return NULL;
    }
}

GType
_ag_type_to_g_type (const gchar *type_str)
{
    g_return_val_if_fail (type_str != NULL, G_TYPE_INVALID);

    switch (type_str[0])
    {
    case DBUS_TYPE_STRING:
        return G_TYPE_STRING;
    case DBUS_TYPE_INT32:
        return G_TYPE_INT;
    case DBUS_TYPE_UINT32:
        return G_TYPE_UINT;
    case DBUS_TYPE_INT64:
        return G_TYPE_INT64;
    case DBUS_TYPE_UINT64:
        return G_TYPE_UINT64;
    case DBUS_TYPE_BOOLEAN:
        return G_TYPE_BOOLEAN;
    case DBUS_TYPE_BYTE:
        return G_TYPE_UCHAR;
    case DBUS_TYPE_ARRAY:
        if (type_str[1] == DBUS_TYPE_STRING)
            return G_TYPE_STRV;
        // fall through
    default:
        g_warning ("%s: unsupported type ``%s''", G_STRFUNC, type_str);
        return G_TYPE_INVALID;
    }
}

static gboolean
_ag_value_set_from_string (GValue *value, const gchar *type,
                           const gchar *string)
{
    GVariant *variant;
    GError *error = NULL;

    if (G_UNLIKELY (!string)) return FALSE;

    /* g_variant_parse() expects all strings to be enclosed in quotes, which we
     * wouldn't like to enforce in the XML files. So, if we know that we are
     * reading a string, just build the GValue right away */
    if (type != NULL && type[0] == 's' && type[1] == '\0' &&
        string[0] != '"' && string[0] != '\'')
    {
        g_value_init (value, G_TYPE_STRING);
        g_value_set_string (value, string);
        return TRUE;
    }

    variant = g_variant_parse ((GVariantType *)type, string,
                               NULL, NULL, &error);
    if (error != 0)
    {
        g_warning ("%s: error parsing type \"%s\" ``%s'': %s",
                   G_STRFUNC, type, string, error->message);
        g_error_free (error);
        return FALSE;
    }

    g_dbus_gvariant_to_gvalue (variant, value);
    g_variant_unref (variant);

    return TRUE;
}

GValue *
_ag_value_from_db (sqlite3_stmt *stmt, gint col_type, gint col_value)
{
    GValue *value;
    gchar *string_value;
    gchar *type;
    gboolean ok;

    type = (gchar *)sqlite3_column_text (stmt, col_type);
    string_value = (gchar *)sqlite3_column_text (stmt, col_value);

    value = g_slice_new0 (GValue);
    ok = _ag_value_set_from_string (value, type, string_value);
    if (G_UNLIKELY (!ok))
    {
        g_slice_free (GValue, value);
        return NULL;
    }

    return value;
}

/**
 * ag_errors_quark:
 *
 * Return the libaccounts-glib error domain.
 *
 * Returns: the libaccounts-glib error domain.
 */
GQuark
ag_errors_quark (void)
{
    static gsize quark = 0;

    if (g_once_init_enter (&quark))
    {
        GQuark domain = g_quark_from_static_string ("ag_errors");

        g_assert (sizeof (GQuark) <= sizeof (gsize));

        g_once_init_leave (&quark, domain);
    }

    return (GQuark) quark;
}

static void
ag_value_append (DBusMessageIter *iter, const GValue *value)
{
    DBusMessageIter var;
    gchar *val_str;

    val_str = _ag_value_to_db (value, TRUE);

    dbus_message_iter_open_container (iter, DBUS_TYPE_VARIANT,
                                      DBUS_TYPE_STRING_AS_STRING, &var);
    dbus_message_iter_append_basic (&var, DBUS_TYPE_STRING, &val_str);
    dbus_message_iter_close_container (iter, &var);

    g_free (val_str);
}

void
_ag_iter_append_dict_entry (DBusMessageIter *iter, const gchar *key,
                            const GValue *value)
{
    DBusMessageIter args;

    dbus_message_iter_open_container (iter, DBUS_TYPE_DICT_ENTRY, NULL, &args);
    dbus_message_iter_append_basic (&args, DBUS_TYPE_STRING, &key);

    ag_value_append (&args, value);
    dbus_message_iter_close_container (iter, &args);
}

static gboolean
_ag_iter_get_value (DBusMessageIter *iter, GValue *value)
{
    DBusMessageIter var;
    const gchar *val_str;

    dbus_message_iter_recurse (iter, &var);

    /* the values are always passed as strings, in the GVariant text format. */
    if (dbus_message_iter_get_arg_type (&var) != DBUS_TYPE_STRING)
        return FALSE;

    dbus_message_iter_get_basic (&var, &val_str);
    return _ag_value_set_from_string (value, NULL, val_str);
}

gboolean
_ag_iter_get_dict_entry (DBusMessageIter *iter, const gchar **key,
                         GValue *value)
{
    DBusMessageIter args;

    dbus_message_iter_recurse (iter, &args);
    if (G_UNLIKELY (dbus_message_iter_get_arg_type (&args) !=
                    DBUS_TYPE_STRING))
        return FALSE;

    dbus_message_iter_get_basic (&args, key);
    dbus_message_iter_next (&args);

    if (G_UNLIKELY (dbus_message_iter_get_arg_type (&args) !=
                    DBUS_TYPE_VARIANT))
        return FALSE;

    return _ag_iter_get_value (&args, value);
}

gboolean
_ag_xml_get_element_data (xmlTextReaderPtr reader, const gchar **dest_ptr)
{
    gint node_type;

    if (dest_ptr) *dest_ptr = NULL;

    if (xmlTextReaderIsEmptyElement (reader))
        return TRUE;

    if (xmlTextReaderRead (reader) != 1)
        return FALSE;

    node_type = xmlTextReaderNodeType (reader);
    if (node_type != XML_READER_TYPE_TEXT)
        return (node_type == XML_READER_TYPE_END_ELEMENT) ? TRUE : FALSE;

    if (dest_ptr)
        *dest_ptr = (const gchar *)xmlTextReaderConstValue (reader);

    return TRUE;
}

static gboolean
close_element (xmlTextReaderPtr reader)
{
    if (xmlTextReaderRead (reader) != 1 ||
        xmlTextReaderNodeType (reader) != XML_READER_TYPE_END_ELEMENT)
        return FALSE;

    return TRUE;
}

gboolean
_ag_xml_dup_element_data (xmlTextReaderPtr reader, gchar **dest_ptr)
{
    const gchar *data;
    gboolean ret;

    ret = _ag_xml_get_element_data (reader, &data);
    if (dest_ptr)
        *dest_ptr = g_strdup (data);

    close_element (reader);
    return ret;
}

static gboolean
parse_param (xmlTextReaderPtr reader, GValue *value)
{
    const gchar *str_value;
    xmlChar *str_type = NULL;
    gboolean ok;
    const gchar *type;

    str_type = xmlTextReaderGetAttribute (reader,
                                          (xmlChar *) "type");
    if (!str_type)
        type = "s";
    else
    {
        type = (const gchar*)str_type;
    }

    ok = _ag_xml_get_element_data (reader, &str_value);
    if (G_UNLIKELY (!ok)) goto error;

    /* Empty value is not an error, but simply ignored */
    if (G_UNLIKELY (!str_value)) goto finish;

    ok = _ag_value_set_from_string (value, type, str_value);
    if (G_UNLIKELY (!ok)) goto error;

    ok = close_element (reader);
    if (G_UNLIKELY (!ok)) goto error;

finish:
    ok = TRUE;
error:
    if (str_type != NULL)
        xmlFree(str_type);
    return TRUE;
}

gboolean
_ag_xml_parse_settings (xmlTextReaderPtr reader, const gchar *group,
                        GHashTable *settings)
{
    const gchar *name;
    int ret, type;

    ret = xmlTextReaderRead (reader);
    while (ret == 1)
    {
        name = (const gchar *)xmlTextReaderConstName (reader);
        if (G_UNLIKELY (!name)) return FALSE;

        type = xmlTextReaderNodeType (reader);
        if (type == XML_READER_TYPE_END_ELEMENT)
            break;

        if (type == XML_READER_TYPE_ELEMENT)
        {
            gboolean ok;

            DEBUG_INFO ("found name %s", name);
            if (strcmp (name, "setting") == 0)
            {
                GValue value = { 0 }, *pval;
                xmlChar *key_name;
                gchar *key;

                key_name = xmlTextReaderGetAttribute (reader, (xmlChar *)"name");
                key = g_strdup_printf ("%s%s", group, (const gchar*)key_name);

                if (key_name) xmlFree (key_name);

                ok = parse_param (reader, &value);
                if (ok && G_VALUE_TYPE (&value) != G_TYPE_INVALID)
                {
                    pval = g_slice_new0 (GValue);
                    g_value_init (pval, G_VALUE_TYPE (&value));
                    g_value_copy (&value, pval);

                    g_hash_table_insert (settings, key, pval);
                }
                else
                    g_free (key);

                if (G_IS_VALUE(&value))
                    g_value_unset (&value);
            }
            else if (strcmp (name, "group") == 0 &&
                     xmlTextReaderHasAttributes (reader))
            {
                /* it's a subgroup */
                if (!xmlTextReaderIsEmptyElement (reader))
                {
                    xmlChar *group_name;
                    gchar *subgroup;

                    group_name = xmlTextReaderGetAttribute (reader,
                                                            (xmlChar *)"name");
                    subgroup = g_strdup_printf ("%s%s/", group,
                                                (const gchar *)group_name);
                    if (group_name) xmlFree (group_name);

                    ok = _ag_xml_parse_settings (reader, subgroup, settings);
                    g_free (subgroup);
                }
                else
                    ok = TRUE;
            }
            else
            {
                g_warning ("%s: using wrong XML for groups; "
                           "please change to <group name=\"%s\">",
                           xmlTextReaderConstBaseUri (reader), name);
                /* it's a subgroup */
                if (!xmlTextReaderIsEmptyElement (reader))
                {
                    gchar *subgroup;

                    subgroup = g_strdup_printf ("%s%s/", group, name);
                    ok = _ag_xml_parse_settings (reader, subgroup, settings);
                    g_free (subgroup);
                }
                else
                    ok = TRUE;
            }

            if (G_UNLIKELY (!ok)) return FALSE;
        }

        ret = xmlTextReaderNext (reader);
    }
    return TRUE;
}

static inline gboolean
_esc_ident_bad (gchar c, gboolean is_first)
{
  return ((c < 'a' || c > 'z') &&
          (c < 'A' || c > 'Z') &&
          (c < '0' || c > '9' || is_first));
}

/**
 * _ag_dbus_escape_as_identifier:
 * @name: The string to be escaped
 *
 * Taken from telepathy-glib's tp_escape_as_identifier().
 *
 * Escape an arbitrary string so it follows the rules for a C identifier,
 * and hence an object path component, interface element component,
 * bus name component or member name in D-Bus.
 *
 * Unlike g_strcanon this is a reversible encoding, so it preserves
 * distinctness.
 *
 * The escaping consists of replacing all non-alphanumerics, and the first
 * character if it's a digit, with an underscore and two lower-case hex
 * digits:
 *
 *    "0123abc_xyz\x01\xff" -> _30123abc_5fxyz_01_ff
 *
 * i.e. similar to URI encoding, but with _ taking the role of %, and a
 * smaller allowed set. As a special case, "" is escaped to "_" (just for
 * completeness, really).
 *
 * Returns: the escaped string, which must be freed by the caller with #g_free
 */
gchar *
_ag_dbus_escape_as_identifier (const gchar *name)
{
    gboolean bad = FALSE;
    size_t len = 0;
    GString *op;
    const gchar *ptr, *first_ok;

    g_return_val_if_fail (name != NULL, NULL);

    /* fast path for empty name */
    if (name[0] == '\0')
        return g_strdup ("_");

    for (ptr = name; *ptr; ptr++)
    {
        if (_esc_ident_bad (*ptr, ptr == name))
        {
            bad = TRUE;
            len += 3;
        }
        else
            len++;
    }

    /* fast path if it's clean */
    if (!bad)
        return g_strdup (name);

    /* If strictly less than ptr, first_ok is the first uncopied safe
     * character. */
    first_ok = name;
    op = g_string_sized_new (len);
    for (ptr = name; *ptr; ptr++)
    {
        if (_esc_ident_bad (*ptr, ptr == name))
        {
            /* copy preceding safe characters if any */
            if (first_ok < ptr)
            {
                g_string_append_len (op, first_ok, ptr - first_ok);
            }
            /* escape the unsafe character */
            g_string_append_printf (op, "_%02x", (unsigned char)(*ptr));
            /* restart after it */
            first_ok = ptr + 1;
        }
    }
    /* copy trailing safe characters if any */
    if (first_ok < ptr)
    {
        g_string_append_len (op, first_ok, ptr - first_ok);
    }
    return g_string_free (op, FALSE);
}

/**
 * _ag_find_libaccounts_file:
 * @file_id: the base name of the file, without suffix.
 * @suffix: the file suffix.
 * @env_var: name of the environment variable which could specify an override
 * path.
 * @subdir: file will be searched in $XDG_DATA_DIRS/<subdir>/
 *
 * Search for the libaccounts file @file_id.
 *
 * Returns: the path of the file, if found, %NULL otherwise.
 */
gchar *
_ag_find_libaccounts_file (const gchar *file_id,
                           const gchar *suffix,
                           const gchar *env_var,
                           const gchar *subdir)
{
    const gchar * const *dirs;
    const gchar *dirname;
    const gchar *env_dirname;
    gchar *filename, *filepath;

    filename = g_strconcat (file_id, suffix, NULL);
    env_dirname = g_getenv (env_var);
    if (env_dirname)
    {
        filepath = g_build_filename (env_dirname, filename, NULL);
        if (g_file_test (filepath, G_FILE_TEST_IS_REGULAR))
            goto found;
        g_free (filepath);
    }

    dirname = g_get_user_data_dir ();
    if (G_LIKELY (dirname))
    {
        filepath = g_build_filename (dirname, subdir, filename, NULL);
        if (g_file_test (filepath, G_FILE_TEST_IS_REGULAR))
            goto found;
        g_free (filepath);
    }

    dirs = g_get_system_data_dirs ();
    for (dirname = *dirs; dirname != NULL; dirs++, dirname = *dirs)
    {
        filepath = g_build_filename (dirname, subdir, filename, NULL);
        if (g_file_test (filepath, G_FILE_TEST_IS_REGULAR))
            goto found;
        g_free (filepath);
    }

    filepath = NULL;
found:
    g_free (filename);
    return filepath;
}


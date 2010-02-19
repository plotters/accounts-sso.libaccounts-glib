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

#ifndef _AG_PROVIDER_H_
#define _AG_PROVIDER_H_

#include <glib.h>

G_BEGIN_DECLS

typedef struct _AgProvider AgProvider;

const gchar *ag_provider_get_name (AgProvider *provider);
const gchar *ag_provider_get_display_name (AgProvider *provider);
void ag_provider_get_file_contents (AgProvider *provider,
                                    const gchar **contents);
AgProvider *ag_provider_ref (AgProvider *provider);
void ag_provider_unref (AgProvider *provider);
void ag_provider_list_free (GList *list);

G_END_DECLS

#endif /* _AG_PROVIDER_H_ */
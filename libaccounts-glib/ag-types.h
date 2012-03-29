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

#ifndef _AG_TYPES_H_
#define _AG_TYPES_H_

G_BEGIN_DECLS

typedef struct _AgAccount AgAccount;
typedef struct _AgManager AgManager;

/**
 * AgAccountId:
 *
 * ID of an account. Often used when retrieving lists of accounts from
 * #AgManager.
 */
typedef guint AgAccountId;

/* guards to avoid bumping up the GLib dependency */
#ifndef G_DEPRECATED
#define G_DEPRECATED            G_GNUC_DEPRECATED
#define G_DEPRECATED_FOR(x)     G_GNUC_DEPRECATED_FOR(x)
#endif

#ifdef AG_DISABLE_DEPRECATION_WARNINGS
#define AG_DEPRECATED
#define AG_DEPRECATED_FOR(x)
#else
#define AG_DEPRECATED           G_DEPRECATED
#define AG_DEPRECATED_FOR(x)    G_DEPRECATED_FOR(x)
#endif

G_END_DECLS

#endif /* _AG_TYPES_H_ */

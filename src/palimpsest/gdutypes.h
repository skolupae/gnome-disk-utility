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

#ifndef __GDU_TYPES_H__
#define __GDU_TYPES_H__

#include <gtk/gtk.h>
#define UDISKS_API_IS_SUBJECT_TO_CHANGE
#include <udisks/udisks.h>

#include "gduenums.h"

G_BEGIN_DECLS

struct _GduApplication;
typedef struct _GduApplication GduApplication;

struct _GduDeviceTreeModel;
typedef struct _GduDeviceTreeModel GduDeviceTreeModel;

struct _GduWindow;
typedef struct _GduWindow GduWindow;

struct _GduVolumeGrid;
typedef struct _GduVolumeGrid GduVolumeGrid;

struct _GduCreateFilesystemWidget;
typedef struct _GduCreateFilesystemWidget GduCreateFilesystemWidget;

struct _GduPasswordStrengthWidget;
typedef struct _GduPasswordStrengthWidget GduPasswordStrengthWidget;

G_END_DECLS

#endif /* __GDU_TYPES_H__ */

/* sysprof-memprof-source.c
 *
 * Copyright 2020 Christian Hergert <chergert@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "sysprof-memprof-source"

#include "config.h"

#include "sysprof-memprof-source.h"

struct _SysprofMemprofSource
{
  GObject parent_instance;
};

static void
sysprof_memprof_source_modify_spawn (SysprofSource    *source,
                                     SysprofSpawnable *spawnable)
{
  g_assert (SYSPROF_IS_SOURCE (source));
  g_assert (SYSPROF_IS_SPAWNABLE (spawnable));

#ifdef __linux__
  sysprof_spawnable_setenv (spawnable, "G_SLICE", "always-malloc");
  sysprof_spawnable_setenv (spawnable,
                            "LD_PRELOAD",
                            PACKAGE_LIBEXECDIR"/libsysprof-memory-"API_VERSION_S".so");
#endif
}

static void
source_iface_init (SysprofSourceInterface *iface)
{
  iface->modify_spawn = sysprof_memprof_source_modify_spawn;
}

G_DEFINE_TYPE_WITH_CODE (SysprofMemprofSource, sysprof_memprof_source, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (SYSPROF_TYPE_SOURCE, source_iface_init))

static void
sysprof_memprof_source_class_init (SysprofMemprofSourceClass *klass)
{
}

static void
sysprof_memprof_source_init (SysprofMemprofSource *self)
{
}

SysprofSource *
sysprof_memprof_source_new (void)
{
  return g_object_new (SYSPROF_TYPE_MEMPROF_SOURCE, NULL);
}
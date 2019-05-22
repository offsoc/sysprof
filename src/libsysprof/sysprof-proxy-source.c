/* sysprof-proxy-source.c
 *
 * Copyright 2019 Christian Hergert <chergert@redhat.com>
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

#define G_LOG_DOMAIN "sysprof-proxy-source"

#include "config.h"

#include <gio/gunixfdlist.h>
#include <sysprof.h>

#include "sysprof-platform.h"

#include "sysprof-proxy-source.h"

struct _SysprofProxySource
{
  GObject               parent_instance;
  GCancellable         *cancellable;
  SysprofCaptureWriter *writer;
  gchar                *bus_name;
  gchar                *object_path;
  GArray               *pids;
  GPtrArray            *monitors;
  GBusType              bus_type;
  guint                 stopping_count;
  guint                 is_ready : 1;
  guint                 has_started : 1;
  guint                 is_whole_system : 1;
};

typedef struct
{
  SysprofProxySource *self;
  gchar              *name;
} Peer;

typedef struct
{
  SysprofProxySource   *self;
  GDBusConnection      *bus;
  gchar                *name;
  gchar                *object_path;
  gint                  fd;
  guint                 needs_stop : 1;
} Monitor;

static inline gint
steal_fd (gint *fd)
{
  gint r = *fd;
  *fd = -1;
  return r;
}

static void
peer_free (Peer *peer)
{
  g_assert (peer != NULL);

  g_clear_object (&peer->self);
  g_clear_pointer (&peer->name, g_free);
  g_slice_free (Peer, peer);
}

static void
monitor_free (Monitor *monitor)
{
  if (monitor == NULL)
    return;

  if (monitor->needs_stop)
    g_dbus_connection_call (monitor->bus,
                            monitor->name,
                            monitor->object_path,
                            "org.gnome.Sysprof3.Profiler",
                            "Stop",
                            g_variant_new ("()"),
                            G_VARIANT_TYPE ("()"),
                            G_DBUS_CALL_FLAGS_NO_AUTO_START,
                            -1,
                            NULL, NULL, NULL);

  if (monitor->fd != -1)
    {
      close (monitor->fd);
      monitor->fd = -1;
    }

  g_clear_object (&monitor->self);
  g_clear_object (&monitor->bus);
  g_clear_pointer (&monitor->name, g_free);
  g_clear_pointer (&monitor->object_path, g_free);
  g_slice_free (Monitor, monitor);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Peer, peer_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Monitor, monitor_free);

static void
sysprof_proxy_source_prepare (SysprofSource *source)
{
  g_assert (SYSPROF_IS_PROXY_SOURCE (source));

  sysprof_source_emit_ready (source);
}

static gboolean
sysprof_proxy_source_get_is_ready (SysprofSource *source)
{
  g_assert (SYSPROF_IS_PROXY_SOURCE (source));

  return TRUE;
}

static void
sysprof_proxy_source_set_writer (SysprofSource        *source,
                                 SysprofCaptureWriter *writer)
{
  SysprofProxySource *self = (SysprofProxySource *)source;

  g_assert (SYSPROF_IS_PROXY_SOURCE (self));
  g_assert (writer != NULL);

  g_clear_pointer (&self->writer, sysprof_capture_writer_unref);
  self->writer = sysprof_capture_writer_ref (writer);
}

static void
sysprof_proxy_source_take_monitor (SysprofProxySource *self,
                                   Monitor            *monitor)
{
  g_assert (SYSPROF_IS_PROXY_SOURCE (self));
  g_assert (monitor != NULL);
  g_assert (monitor->self == self);
  g_assert (monitor->bus == NULL || G_IS_DBUS_CONNECTION (monitor->bus));

  if (g_cancellable_is_cancelled (self->cancellable))
    monitor_free (monitor);
  else
    g_ptr_array_add (self->monitors, g_steal_pointer (&monitor));
}

static void
sysprof_proxy_source_start_cb (GObject      *object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  GDBusConnection *bus = (GDBusConnection *)object;
  g_autoptr(Monitor) monitor = user_data;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GError) error = NULL;
  SysprofProxySource *self;

  g_assert (G_IS_DBUS_CONNECTION (bus));
  g_assert (monitor != NULL);
  g_assert (SYSPROF_IS_PROXY_SOURCE (monitor->self));
  g_assert (G_IS_ASYNC_RESULT (result));

  if (!(reply = g_dbus_connection_call_with_unix_fd_list_finish (bus, NULL, result, &error)))
    {
      g_dbus_error_strip_remote_error (error);
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        monitor->needs_stop = TRUE;
      g_message ("Failure or no profiler available on peer %s: %s",
                 monitor->name, error->message);
      return;
    }

  self = monitor->self;
  monitor->needs_stop = TRUE;
  sysprof_proxy_source_take_monitor (self, g_steal_pointer (&monitor));
}

static void
sysprof_proxy_source_monitor (SysprofProxySource *self,
                              GDBusConnection    *bus,
                              const gchar        *bus_name)
{
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GError) error = NULL;
  Monitor *monitor;
  gint fd;
  gint handle;

  g_assert (SYSPROF_IS_PROXY_SOURCE (self));
  g_assert (G_IS_DBUS_CONNECTION (bus));
  g_assert (bus_name != NULL);

  if (g_cancellable_is_cancelled (self->cancellable))
    return;

  fd_list = g_unix_fd_list_new ();

  if (-1 == (fd = sysprof_memfd_create ("[sysprof-proxy-capture]")) ||
      -1 == (handle = g_unix_fd_list_append (fd_list, fd, &error)))
    {
      if (fd != -1)
        close (fd);
      g_warning ("Failed to create memfd for peer: %s", error->message);
      return;
    }

  monitor = g_slice_new0 (Monitor);
  monitor->self = g_object_ref (self);
  monitor->bus = g_object_ref (bus);
  monitor->name = g_strdup (bus_name);
  monitor->object_path = g_strdup (self->object_path);
  monitor->fd = fd;

  g_dbus_connection_call_with_unix_fd_list (bus,
                                            bus_name,
                                            self->object_path,
                                            "org.gnome.Sysprof3.Profiler",
                                            "Start",
                                            g_variant_new ("(h)", handle),
                                            G_VARIANT_TYPE ("()"),
                                            G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                            -1,
                                            fd_list,
                                            self->cancellable,
                                            sysprof_proxy_source_start_cb,
                                            g_steal_pointer (&monitor));
}

static void
sysprof_proxy_source_get_pid_cb (GObject      *object,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
  GDBusConnection *bus = (GDBusConnection *)object;
  g_autoptr(Peer) peer = user_data;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GError) error = NULL;
  guint32 pid = 0;

  g_assert (G_IS_DBUS_CONNECTION (bus));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (peer != NULL);
  g_assert (SYSPROF_IS_PROXY_SOURCE (peer->self));

  if (!(reply = g_dbus_connection_call_finish (bus, result, &error)))
    return;

  g_variant_get (reply, "(u)", &pid);

  /* If we don't care about this PID, then ignore it */
  for (guint i = 0; i < peer->self->pids->len; i++)
    {
      if ((GPid)pid == g_array_index (peer->self->pids, GPid, i))
        {
          sysprof_proxy_source_monitor (peer->self, bus, peer->name);
          return;
        }
    }
}

static void
sysprof_proxy_source_list_names_cb (GObject      *object,
                                    GAsyncResult *result,
                                    gpointer      user_data)
{
  GDBusConnection *bus = (GDBusConnection *)object;
  g_autofree const gchar **names = NULL;
  g_autoptr(SysprofProxySource) self = user_data;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (G_IS_DBUS_CONNECTION (bus));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (SYSPROF_IS_PROXY_SOURCE (self));

  if (!(reply = g_dbus_connection_call_finish (bus, result, &error)))
    {
      g_warning ("Failed to list D-Bus peer names: %s", error->message);
      return;
    }

  g_variant_get (reply, "(^a&s)", &names);

  for (guint i = 0; names[i] != NULL; i++)
    {
      Peer *peer;

      peer = g_slice_new (Peer);
      peer->self = g_object_ref (self);
      peer->name = g_strdup (names[i]);

      g_dbus_connection_call (bus,
                              "org.freedesktop.DBus",
                              "/org/freedesktop/DBus",
                              "org.freedesktop.DBus",
                              "GetConnectionUnixProcessID",
                              g_variant_new ("(s)", names[i]),
                              G_VARIANT_TYPE ("(u)"),
                              G_DBUS_CALL_FLAGS_NO_AUTO_START,
                              -1,
                              self->cancellable,
                              sysprof_proxy_source_get_pid_cb,
                              g_steal_pointer (&peer));
    }
}

static void
sysprof_proxy_source_get_bus_cb (GObject      *object,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
  g_autoptr(SysprofProxySource) self = user_data;
  g_autoptr(GDBusConnection) bus = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (SYSPROF_IS_PROXY_SOURCE (self));

  if (!(bus = g_bus_get_finish (result, &error)))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        sysprof_source_emit_failed (SYSPROF_SOURCE (self), error);
      return;
    }

  if (self->bus_name != NULL)
    {
      sysprof_proxy_source_monitor (self, bus, self->bus_name);
      return;
    }

  if (self->pids->len > 0)
    {
      /* We need to query the processes that have been spawned to see
       * if they have our proxy address associated with them. But first,
       * we need to find what pids own what connection.
       */
      g_dbus_connection_call (bus,
                              "org.freedesktop.DBus",
                              "/org/freedesktop/DBus",
                              "org.freedesktop.DBus",
                              "ListNames",
                              g_variant_new ("()"),
                              G_VARIANT_TYPE ("(as)"),
                              G_DBUS_CALL_FLAGS_NO_AUTO_START,
                              -1,
                              self->cancellable,
                              sysprof_proxy_source_list_names_cb,
                              g_object_ref (self));
      return;
    }

  g_warning ("Improperly configured %s", G_OBJECT_TYPE_NAME (self));
}

static void
sysprof_proxy_source_start (SysprofSource *source)
{
  SysprofProxySource *self = (SysprofProxySource *)source;

  g_assert (SYSPROF_IS_PROXY_SOURCE (self));

  self->has_started = TRUE;

  g_bus_get (self->bus_type,
             self->cancellable,
             sysprof_proxy_source_get_bus_cb,
             g_object_ref (self));
}

static void
sysprof_proxy_source_cat (SysprofProxySource   *self,
                          SysprofCaptureReader *reader)
{
  g_assert (SYSPROF_IS_PROXY_SOURCE (self));
  g_assert (reader != NULL);

  if (self->writer != NULL)
    {
      g_autoptr(GError) error = NULL;

      if (!sysprof_capture_writer_cat (self->writer, reader, &error))
        g_warning ("Failed to join reader: %s\n", error->message);
    }
}

static void
sysprof_proxy_source_complete_monitor (SysprofProxySource *self,
                                       Monitor            *monitor)
{
  g_autoptr(SysprofCaptureReader) reader = NULL;
  g_autoptr(GError) error = NULL;

  g_assert (SYSPROF_IS_PROXY_SOURCE (self));
  g_assert (monitor != NULL);
  g_assert (monitor->self == self);

  g_print ("completing with FD: %d\n", monitor->fd);

  if (!(reader = sysprof_capture_reader_new_from_fd (steal_fd (&monitor->fd), &error)))
    g_warning ("Failed to load reader from peer FD: %s", error->message);
  else
    sysprof_proxy_source_cat (self, reader);
}

static void
sysprof_proxy_source_stop_cb (GObject      *object,
                              GAsyncResult *result,
                              gpointer      user_data)
{
  GDBusConnection *bus = (GDBusConnection *)object;
  g_autoptr(Monitor) monitor = user_data;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GError) error = NULL;
  SysprofProxySource *self;

  g_assert (G_IS_DBUS_CONNECTION (bus));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (monitor != NULL);

  self = monitor->self;
  reply = g_dbus_connection_call_finish (bus, result, &error);
  monitor->needs_stop = FALSE;

  sysprof_proxy_source_complete_monitor (self, monitor);

  self->stopping_count--;

  if (self->stopping_count == 0)
    sysprof_source_emit_finished (SYSPROF_SOURCE (self));
}

static void
sysprof_proxy_source_stop (SysprofSource *source)
{
  SysprofProxySource *self = (SysprofProxySource *)source;

  g_assert (SYSPROF_IS_PROXY_SOURCE (self));

  g_cancellable_cancel (self->cancellable);

  for (guint i = 0; i < self->monitors->len; i++)
    {
      g_autoptr(Monitor) monitor = g_ptr_array_index (self->monitors, i);

      g_ptr_array_index (self->monitors, i) = NULL;

      if (monitor->needs_stop)
        {
          self->stopping_count++;
          g_dbus_connection_call (monitor->bus,
                                  monitor->name,
                                  monitor->object_path,
                                  "org.gnome.Sysprof3.Profiler",
                                  "Stop",
                                  g_variant_new ("()"),
                                  G_VARIANT_TYPE ("()"),
                                  G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                  -1,
                                  NULL,
                                  sysprof_proxy_source_stop_cb,
                                  g_steal_pointer (&monitor));
        }
      else
        {
          sysprof_proxy_source_complete_monitor (self, monitor);
        }
    }

  if (self->stopping_count == 0)
    sysprof_source_emit_finished (source);
}

static void
sysprof_proxy_source_add_pid (SysprofSource *source,
                              GPid           pid)
{
  SysprofProxySource *self = (SysprofProxySource *)source;

  g_assert (SYSPROF_IS_PROXY_SOURCE (self));
  g_assert (pid > 0);

  if (!self->has_started)
    self->is_whole_system = FALSE;

  g_array_append_val (self->pids, pid);
}

static void
sysprof_proxy_source_modify_spawn (SysprofSource       *source,
                                   GSubprocessLauncher *launcher,
                                   GPtrArray           *argv)
{
  SysprofProxySource *self = (SysprofProxySource *)source;
  gchar fdstr[12];
  Monitor *monitor;
  gint fd;

  g_assert (SYSPROF_IS_PROXY_SOURCE (self));
  g_assert (G_IS_SUBPROCESS_LAUNCHER (launcher));
  g_assert (argv != NULL);

  /* We need to create a new FD for the peer process to write
   * to and notify it via SYSPROF_TRACE_FD. We will largely
   * ignore things until the capture has finished.
   */

  if (-1 == (fd = sysprof_memfd_create ("[sysprof-proxy-capture]")))
    return;

  monitor = g_slice_new0 (Monitor);
  monitor->self = g_object_ref (self);
  monitor->fd = dup (fd);

  g_snprintf (fdstr, sizeof fdstr, "%d", fd);
  g_subprocess_launcher_setenv (launcher, "SYSPROF_TRACE_FD", fdstr, TRUE);
  g_subprocess_launcher_take_fd (launcher, fd, fd);

  sysprof_proxy_source_take_monitor (self, g_steal_pointer (&monitor));
}

static void
source_iface_init (SysprofSourceInterface *iface)
{
  iface->add_pid = sysprof_proxy_source_add_pid;
  iface->prepare = sysprof_proxy_source_prepare;
  iface->set_writer = sysprof_proxy_source_set_writer;
  iface->get_is_ready = sysprof_proxy_source_get_is_ready;
  iface->stop = sysprof_proxy_source_stop;
  iface->start = sysprof_proxy_source_start;
  iface->modify_spawn = sysprof_proxy_source_modify_spawn;
}

G_DEFINE_TYPE_WITH_CODE (SysprofProxySource, sysprof_proxy_source, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (SYSPROF_TYPE_SOURCE, source_iface_init))

static void
sysprof_proxy_source_finalize (GObject *object)
{
  SysprofProxySource *self = (SysprofProxySource *)object;

  g_clear_pointer (&self->monitors, g_ptr_array_unref);
  g_clear_pointer (&self->writer, sysprof_capture_writer_unref);
  g_clear_pointer (&self->bus_name, g_free);
  g_clear_pointer (&self->object_path, g_free);
  g_clear_pointer (&self->pids, g_array_unref);
  g_clear_object (&self->cancellable);

  G_OBJECT_CLASS (sysprof_proxy_source_parent_class)->finalize (object);
}

static void
sysprof_proxy_source_class_init (SysprofProxySourceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = sysprof_proxy_source_finalize;
}

static void
sysprof_proxy_source_init (SysprofProxySource *self)
{
  self->cancellable = g_cancellable_new ();
  self->pids = g_array_new (FALSE, FALSE, sizeof (GPid));
  self->monitors = g_ptr_array_new_with_free_func ((GDestroyNotify) monitor_free);
  self->is_whole_system = TRUE;
}

SysprofSource *
sysprof_proxy_source_new (GBusType     bus_type,
                          const gchar *bus_name,
                          const gchar *object_path)
{
  SysprofProxySource *self;

  g_return_val_if_fail (bus_type == G_BUS_TYPE_SESSION || bus_type == G_BUS_TYPE_SYSTEM, NULL);
  g_return_val_if_fail (bus_name != NULL, NULL);
  g_return_val_if_fail (object_path != NULL, NULL);

  if (bus_name && !*bus_name)
    bus_name = NULL;

  self = g_object_new (SYSPROF_TYPE_PROXY_SOURCE, NULL);
  self->bus_type = bus_type;
  self->bus_name = g_strdup (bus_name);
  self->object_path = g_strdup (object_path);

  return SYSPROF_SOURCE (g_steal_pointer (&self));
}
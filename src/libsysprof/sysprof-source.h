/* sysprof-source.h
 *
 * Copyright 2016-2019 Christian Hergert <chergert@redhat.com>
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

#pragma once

#include <glib-object.h>

#include "sysprof-capture-writer.h"

G_BEGIN_DECLS

#define SYSPROF_TYPE_SOURCE (sysprof_source_get_type())

G_DECLARE_INTERFACE (SysprofSource, sysprof_source, SYSPROF, SOURCE, GObject)

struct _SysprofSourceInterface
{
  GTypeInterface parent_iface;

  /**
   * SysprofSource::get_is_ready:
   * @self: A SysprofSource.
   *
   * This function should return %TRUE if the source is ready to start
   * profiling. If the source is not ready until after sysprof_source_start() has
   * been called, use sysprof_source_emit_ready() to notify the profiler that the
   * source is ready for profiling.
   *
   * Returns: %TRUE if the source is ready to start profiling.
   */
  gboolean (*get_is_ready) (SysprofSource *self);

  /**
   * SysprofSource::set_writer:
   * @self: A #SysprofSource.
   * @writer: A #SysprofCaptureWriter
   *
   * Sets the #SysprofCaptureWriter to use when profiling. @writer is only safe to
   * use from the main thread. If you need to capture from a thread, you should
   * create a memory-based #SysprofCaptureWriter and then splice that into this
   * writer from the main thread when profiling completes.
   *
   * See sysprof_capture_writer_splice() for information on splicing writers.
   */
  void (*set_writer) (SysprofSource        *self,
                      SysprofCaptureWriter *writer);

  /**
   * SysprofSource::prepare:
   *
   * This function is called before profiling has started. The source should
   * prepare any pre-profiling setup here. It may perform this work
   * asynchronously, but must g_object_notify() the SysprofSource::is-ready
   * property once that asynchronous work has been performed. Until it
   * is ready, #SysprofSource::is-ready must return FALSE.
   */
  void (*prepare) (SysprofSource *self);

  /**
   * SysprofSource::add_pid:
   * @self: A #SysprofSource
   * @pid: A pid_t > -1
   *
   * This function is used to notify the #SysprofSource that a new process,
   * identified by @pid, should be profiled. By default, sources should
   * assume all processes, and only restrict to a given set of pids if
   * this function is called.
   */
  void (*add_pid) (SysprofSource *self,
                   GPid      pid);

  /**
   * SysprofSource::start:
   * @self: A #SysprofSource.
   *
   * Start profiling as configured.
   *
   * If a failure occurs while processing, the source should notify the
   * profiling session via sysprof_source_emit_failed() from the main thread.
   */
  void (*start) (SysprofSource *self);

  /**
   * SysprofSource::stop:
   * @self: A #SysprofSource.
   *
   * Stop capturing a profile. The source should immediately stop
   * profiling and perform any cleanup tasks required. If doing
   * off-main-thread capturing, this is a good time to splice your
   * capture into the capture file set with sysprof_source_set_writer().
   *
   * If you need to perform asynchronous cleanup, call
   * sysprof_source_emit_finished() once that work has completed. If you do
   * not need to perform asynchronous cleanup, call
   * sysprof_source_emit_finished() from this function.
   *
   * sysprof_source_emit_finished() must be called from the main-thread.
   */
  void (*stop) (SysprofSource *self);
};

void     sysprof_source_add_pid       (SysprofSource        *self,
                                  GPid             pid);
void     sysprof_source_emit_ready    (SysprofSource        *self);
void     sysprof_source_emit_finished (SysprofSource        *self);
void     sysprof_source_emit_failed   (SysprofSource        *self,
                                  const GError    *error);
gboolean sysprof_source_get_is_ready  (SysprofSource        *self);
void     sysprof_source_prepare       (SysprofSource        *self);
void     sysprof_source_set_writer    (SysprofSource        *self,
                                  SysprofCaptureWriter *writer);
void     sysprof_source_start         (SysprofSource        *self);
void     sysprof_source_stop          (SysprofSource        *self);

G_END_DECLS
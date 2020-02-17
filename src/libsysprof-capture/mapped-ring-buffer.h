/* mapped-ring-buffer.h
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

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _MappedRingBuffer MappedRingBuffer;

/**
 * MappedRingBufferCallback:
 * @data: a pointer into the mapped buffer containing the data frame
 * @length: (inout): the number of bytes to advance
 * @user_data: closure data provided to mapped_ring_buffer_drain()
 *
 * Functions matching this prototype will be called from the
 * mapped_ring_buffer_drain() function for each data frame read from the
 * underlying memory mapping.
 *
 * @length is initially set to the max bytes that @data could contain, but
 * should be set by the caller to the amount of bytes known to have been
 * consumed. This allows MappedRingBuffer to avoid storing length data or
 * framing information as that can come from the buffer content.
 *
 * The callback should shorten @length if it knows the frame is less than
 * what was provided.
 *
 * This function can also be used with mapped_ring_buffer_create_source()
 * to automatically drain the ring buffer as part of the main loop progress.
 *
 * Returns: %TRUE to coninue draining, otherwise %FALSE and draining stops
 */
typedef gboolean (*MappedRingBufferCallback) (gconstpointer  data,
                                              gsize         *length,
                                              gpointer       user_data);

G_GNUC_INTERNAL
MappedRingBuffer *mapped_ring_buffer_new_reader         (gsize                     buffer_size);
G_GNUC_INTERNAL
MappedRingBuffer *mapped_ring_buffer_new_readwrite      (gsize                     buffer_size);
G_GNUC_INTERNAL
MappedRingBuffer *mapped_ring_buffer_new_writer         (gint                      fd);
G_GNUC_INTERNAL
gint              mapped_ring_buffer_get_fd             (MappedRingBuffer         *self);
G_GNUC_INTERNAL
MappedRingBuffer *mapped_ring_buffer_ref                (MappedRingBuffer         *self);
G_GNUC_INTERNAL
void              mapped_ring_buffer_unref              (MappedRingBuffer         *self);
G_GNUC_INTERNAL
void              mapped_ring_buffer_clear              (MappedRingBuffer         *self);
G_GNUC_INTERNAL
gpointer          mapped_ring_buffer_allocate           (MappedRingBuffer         *self,
                                                         gsize                     length);
G_GNUC_INTERNAL
void              mapped_ring_buffer_advance            (MappedRingBuffer         *self,
                                                         gsize                     length);
G_GNUC_INTERNAL
gboolean          mapped_ring_buffer_drain              (MappedRingBuffer         *self,
                                                         MappedRingBufferCallback  callback,
                                                         gpointer                  user_data);
G_GNUC_INTERNAL
guint             mapped_ring_buffer_create_source      (MappedRingBuffer         *self,
                                                         MappedRingBufferCallback  callback,
                                                         gpointer                  user_data);
G_GNUC_INTERNAL
guint             mapped_ring_buffer_create_source_full (MappedRingBuffer         *self,
                                                         MappedRingBufferCallback  callback,
                                                         gpointer                  user_data,
                                                         GDestroyNotify            destroy);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MappedRingBuffer, mapped_ring_buffer_unref)

G_END_DECLS
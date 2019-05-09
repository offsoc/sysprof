/* sysprof-cpu-visualizer-row.c
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

#define G_LOG_DOMAIN "sysprof-cpu-visualizer-row"

#include "config.h"

#include <sysprof.h>

#include "sysprof-color-cycle.h"
#include "sysprof-cpu-visualizer-row.h"

struct _SysprofCpuVisualizerRow
{
  SysprofLineVisualizerRow parent_instance;
  SysprofColorCycle *colors;
};

G_DEFINE_TYPE (SysprofCpuVisualizerRow, sysprof_cpu_visualizer_row, SYSPROF_TYPE_LINE_VISUALIZER_ROW)

static gboolean
sysprof_cpu_visualizer_counter_found (const SysprofCaptureFrame *frame,
                                 gpointer              user_data)
{
  const SysprofCaptureFrameCounterDefine *def = (SysprofCaptureFrameCounterDefine *)frame;
  GArray *counters = user_data;
  gboolean found = FALSE;

  g_assert (frame->type == SYSPROF_CAPTURE_FRAME_CTRDEF);

  /*
   * In practice, all the CPU counters are defined at once, so we can avoid
   * walking the rest of the capture by returning after we find our CTRDEF.
   */

  for (guint i = 0; i < def->n_counters; i++)
    {
      if (g_str_equal (def->counters[i].category, "CPU Percent"))
        {
          guint id = def->counters[i].id;
          g_array_append_val (counters, id);
          found = TRUE;
        }
    }

  return !found;
}

static void
sysprof_cpu_visualizer_row_discover_counters (GTask        *task,
                                         gpointer      source_object,
                                         gpointer      task_data,
                                         GCancellable *canellable)
{
  const SysprofCaptureFrameType types[] = { SYSPROF_CAPTURE_FRAME_CTRDEF };
  SysprofCaptureReader *reader = task_data;
  g_autoptr(SysprofCaptureCursor) cursor = NULL;
  g_autoptr(GArray) counters = NULL;

  g_assert (G_IS_TASK (task));
  g_assert (SYSPROF_IS_CPU_VISUALIZER_ROW (source_object));
  g_assert (reader != NULL);

  counters = g_array_new (FALSE, FALSE, sizeof (guint));
  cursor = sysprof_capture_cursor_new (reader);
  sysprof_capture_cursor_add_condition (cursor, sysprof_capture_condition_new_where_type_in (G_N_ELEMENTS (types), types));
  sysprof_capture_cursor_foreach (cursor, sysprof_cpu_visualizer_counter_found, counters);
  g_task_return_pointer (task, g_steal_pointer (&counters), (GDestroyNotify)g_array_unref);
}

static void
complete_counters (GObject      *object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  SysprofCpuVisualizerRow *self = (SysprofCpuVisualizerRow *)object;
  g_autoptr(GArray) counters = NULL;

  g_assert (SYSPROF_IS_CPU_VISUALIZER_ROW (self));
  g_assert (G_IS_TASK (result));

  counters = g_task_propagate_pointer (G_TASK (result), NULL);

  if (counters != NULL)
    {
      for (guint i = 0; i < counters->len; i++)
        {
          guint counter_id = g_array_index (counters, guint, i);
          GdkRGBA color;

          sysprof_color_cycle_next (self->colors, &color);
          sysprof_line_visualizer_row_add_counter (SYSPROF_LINE_VISUALIZER_ROW (self), counter_id, &color);
        }
    }

  /* Hide ourself if we failed to locate counters */
  gtk_widget_set_visible (GTK_WIDGET (self), counters != NULL && counters->len > 0);
}

static void
sysprof_cpu_visualizer_row_set_reader (SysprofVisualizerRow *row,
                                  SysprofCaptureReader *reader)
{
  SysprofCpuVisualizerRow *self = (SysprofCpuVisualizerRow *)row;
  g_autoptr(GTask) task = NULL;

  g_assert (SYSPROF_IS_CPU_VISUALIZER_ROW (row));

  sysprof_color_cycle_reset (self->colors);

  sysprof_line_visualizer_row_clear (SYSPROF_LINE_VISUALIZER_ROW (row));

  SYSPROF_VISUALIZER_ROW_CLASS (sysprof_cpu_visualizer_row_parent_class)->set_reader (row, reader);

  if (reader != NULL)
    {
      task = g_task_new (self, NULL, complete_counters, NULL);
      g_task_set_source_tag (task, sysprof_cpu_visualizer_row_set_reader);
      g_task_set_task_data (task, sysprof_capture_reader_copy (reader),
                            (GDestroyNotify)sysprof_capture_reader_unref);
      g_task_run_in_thread (task, sysprof_cpu_visualizer_row_discover_counters);
    }
}

static void
sysprof_cpu_visualizer_row_finalize (GObject *object)
{
  SysprofCpuVisualizerRow *self = (SysprofCpuVisualizerRow *)object;

  g_clear_pointer (&self->colors, sysprof_color_cycle_unref);

  G_OBJECT_CLASS (sysprof_cpu_visualizer_row_parent_class)->finalize (object);
}

static void
sysprof_cpu_visualizer_row_class_init (SysprofCpuVisualizerRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  SysprofVisualizerRowClass *row_class = SYSPROF_VISUALIZER_ROW_CLASS (klass);

  object_class->finalize = sysprof_cpu_visualizer_row_finalize;

  row_class->set_reader = sysprof_cpu_visualizer_row_set_reader;
}

static void
sysprof_cpu_visualizer_row_init (SysprofCpuVisualizerRow *self)
{
  self->colors = sysprof_color_cycle_new ();
}

GtkWidget *
sysprof_cpu_visualizer_row_new (void)
{
  return g_object_new (SYSPROF_TYPE_CPU_VISUALIZER_ROW, NULL);
}
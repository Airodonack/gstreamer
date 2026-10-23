/* GStreamer
 *
 * SPDX-License-Identifier: LGPL-2.1
 *
 * Copyright (C) 2023 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/base/gstdataqueue.h>
#include <gst/mse/mse-prelude.h>

#include "gstmseeventqueue-private.h"

struct _GstMseEventQueue
{
  GstObject parent_instance;

  GstMseEventQueueCallback callback;
  gpointer user_data;

  GstTask *task;
  /* The thread running task_func, so finalize can tell it is on it. */
  GThread *thread;
  GstDataQueue *queue;
};

G_DEFINE_TYPE (GstMseEventQueue, gst_mse_event_queue, GST_TYPE_OBJECT);

GstMseEventQueue *
gst_mse_event_queue_new (GstMseEventQueueCallback callback, gpointer user_data)
{
  g_return_val_if_fail (callback != NULL, NULL);

  GstMseEventQueue *self = g_object_new (GST_TYPE_MSE_EVENT_QUEUE, NULL);

  self->callback = callback;
  self->user_data = user_data;

  gst_task_start (self->task);

  return g_object_ref_sink (self);
}

static void
gst_mse_background_event_queue_dispose (GObject * obj)
{
  GstMseEventQueue *self = GST_MSE_EVENT_QUEUE (obj);

  gst_data_queue_set_flushing (self->queue, TRUE);
  gst_data_queue_flush (self->queue);

  G_OBJECT_CLASS (gst_mse_event_queue_parent_class)->dispose (obj);
}

static void
gst_mse_background_event_queue_finalize (GObject * obj)
{
  GstMseEventQueue *self = GST_MSE_EVENT_QUEUE (obj);

  /* The callback can drop the last reference, for example a source buffer
   * whose owner let go of it while one of its events was being dispatched, and
   * then this runs on the task's own thread, which cannot join itself. Stopping
   * is enough there: the task holds its own reference, its lock belongs to the
   * task (see gst_mse_event_queue_init), and task_func does not touch the queue
   * after the callback returns. */
  gst_task_stop (self->task);
  if (g_atomic_pointer_get (&self->thread) != g_thread_self ()) {
    gst_task_join (self->task);
  }
  gst_clear_object (&self->task);
  gst_clear_object (&self->queue);

  G_OBJECT_CLASS (gst_mse_event_queue_parent_class)->finalize (obj);
}

void
gst_mse_event_queue_class_init (GstMseEventQueueClass * klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);

  oclass->dispose = GST_DEBUG_FUNCPTR (gst_mse_background_event_queue_dispose);
  oclass->finalize =
      GST_DEBUG_FUNCPTR (gst_mse_background_event_queue_finalize);
}

static void
task_func (GstMseEventQueue * self)
{
  GstTask *task = self->task;
  GstDataQueue *queue = self->queue;
  GstDataQueueItem *item = NULL;
  g_atomic_pointer_set (&self->thread, g_thread_self ());
  if (!gst_data_queue_pop (queue, &item)) {
    gst_task_stop (task);
    return;
  }
  self->callback (item, self->user_data);
  if (item->destroy) {
    item->destroy (item);
  }
}

static gboolean
never_full (GstDataQueue * queue, guint visible, guint bytes, guint64 time,
    gpointer user_data)
{
  return FALSE;
}

static void
free_task_lock (GRecMutex * lock)
{
  g_rec_mutex_clear (lock);
  g_free (lock);
}

void
gst_mse_event_queue_init (GstMseEventQueue * self)
{
  self->queue = gst_data_queue_new (never_full, NULL, NULL, NULL);
  self->task = gst_task_new ((GstTaskFunction) task_func, self, NULL);
  /* The task's thread holds this lock for as long as it runs, which can
   * outlast the queue when the queue is finalized on that thread, so the lock
   * lives as long as the task object, which the thread keeps a reference to
   * until it has released the lock. */
  GRecMutex *lock = g_new (GRecMutex, 1);
  g_rec_mutex_init (lock);
  gst_task_set_lock (self->task, lock);
  g_object_set_data_full (G_OBJECT (self->task), "gst-mse-event-queue-lock",
      lock, (GDestroyNotify) free_task_lock);
}

gboolean
gst_mse_event_queue_push (GstMseEventQueue * self, GstDataQueueItem * item)
{
  g_return_val_if_fail (GST_IS_MSE_EVENT_QUEUE (self), FALSE);
  g_return_val_if_fail (item != NULL, FALSE);
  return gst_data_queue_push (self->queue, item);
}

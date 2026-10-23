/* GStreamer
 *
 * SPDX-License-Identifier: LGPL-2.1
 *
 * Copyright (C) 2022, 2023 Collabora Ltd.
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

#ifdef HAVE_VALGRIND
# include <valgrind/valgrind.h>
#endif

#include <gst/check/gstcheck.h>

#include <gst/mse/mse.h>
#include <gst/mse/gstappendpipeline-private.h>
#include <gst/mse/gstmediasource-private.h>
#include <gst/mse/gstmediasourcetrack-private.h>
#include <gst/mse/gstmediasourcetrackbuffer-private.h>
#include <gst/mse/gstmediasourcesamplemap-private.h>

GST_UNUSED_CHECKS static GstCheckLogFilter *
add_log_filter (GLogLevelFlags level, const gchar * regex)
{
  GRegex *gregex = g_regex_new (regex, 0, 0, NULL);
  return gst_check_add_log_filter ("GStreamer-MSE", level, gregex, NULL, NULL,
      NULL);
}

static gchar *
test_mp4_path (void)
{
  return g_build_filename (GST_TEST_FILES_PATH, "mse.mp4", NULL);
}

static gchar *
test_webm_path (void)
{
  return g_build_filename (GST_TEST_FILES_PATH, "mse.webm", NULL);
}

static GstMediaSource *
opened_media_source (void)
{
  GstMediaSource *media_source = gst_media_source_new ();
  media_source->ready_state = GST_MEDIA_SOURCE_READY_STATE_OPEN;
  return media_source;
}

static GstClockTime
sample_dts (GstSample * sample)
{
  GstBuffer *buffer = gst_sample_get_buffer (sample);
  return GST_BUFFER_DTS (buffer);
}

static GstSample *
new_empty_sample_full (GstClockTime dts, GstClockTime pts,
    GstClockTime duration, GstBufferFlags flags, GstCaps * caps,
    GstSegment * segment, GstStructure * info)
{
  GstBuffer *buffer = gst_buffer_new ();
  GST_BUFFER_DTS (buffer) = dts;
  GST_BUFFER_PTS (buffer) = pts;
  GST_BUFFER_DURATION (buffer) = duration;
  GST_BUFFER_FLAGS (buffer) = flags;
  GstSample *sample = gst_sample_new (buffer, caps, segment, info);
  gst_buffer_unref (buffer);
  return sample;
}

static GstSample *
new_empty_sample_with_timing_and_flags (GstClockTime dts, GstClockTime pts,
    GstClockTime duration, GstBufferFlags flags)
{
  return new_empty_sample_full (dts, pts, duration, flags, NULL, NULL, NULL);
}

static GstSample *
new_empty_sample_with_timing (GstClockTime dts, GstClockTime pts,
    GstClockTime duration)
{
  return new_empty_sample_full (dts, pts, duration, 0, NULL, NULL, NULL);
}

static GstSample *
new_sample_with_bytes_and_timing (GBytes * bytes, GstClockTime dts,
    GstClockTime pts, GstClockTime duration)
{
  GstBuffer *buffer = gst_buffer_new_wrapped_bytes (bytes);
  GST_BUFFER_DTS (buffer) = dts;
  GST_BUFFER_PTS (buffer) = pts;
  GST_BUFFER_DURATION (buffer) = duration;
  GstSample *sample = gst_sample_new (buffer, NULL, NULL, NULL);
  gst_buffer_unref (buffer);
  g_bytes_unref (bytes);
  return sample;
}

/* How long a test waits for an event from a source buffer's event thread. */
#define EVENT_TIMEOUT (5 * G_TIME_SPAN_SECOND)

/* A whole test file, for a source buffer to take ownership of. */
static GstBuffer *
test_file_buffer (const gchar * name)
{
  GError *error = NULL;
  gchar *filename = g_build_filename (GST_TEST_FILES_PATH, name, NULL);
  gchar *data;
  gsize length;
  g_file_get_contents (filename, &data, &length, &error);
  g_assert_no_error (error);
  g_free (filename);
  return gst_buffer_new_wrapped (data, length);
}

/* The whole of mse.mp4, for a source buffer to take ownership of. */
static GstBuffer *
test_mp4_buffer (void)
{
  return test_file_buffer ("mse.mp4");
}

/* The offset of the first box of the given type in an MP4 buffer. */
static gsize
mp4_box_offset (GstBuffer * buffer, const gchar * type)
{
  GstMapInfo map;
  fail_unless (gst_buffer_map (buffer, &map, GST_MAP_READ));
  gsize offset = 0;
  while (offset + 8 <= map.size
      && memcmp (map.data + offset + 4, type, 4) != 0) {
    offset += GST_READ_UINT32_BE (map.data + offset);
  }
  gst_buffer_unmap (buffer, &map);
  fail_unless (offset > 0 && offset < gst_buffer_get_size (buffer));
  return offset;
}

/* The bytes of mse.mp4 before its first box of the given type. */
static GstBuffer *
test_mp4_before_box (const gchar * type)
{
  GstBuffer *whole = test_mp4_buffer ();
  GstBuffer *prefix = gst_buffer_copy_region (whole, GST_BUFFER_COPY_MEMORY,
      0, mp4_box_offset (whole, type));
  gst_buffer_unref (whole);
  return prefix;
}

/* The bytes of mse.mp4 from its first box of the given type on. */
static GstBuffer *
test_mp4_from_box (const gchar * type)
{
  GstBuffer *whole = test_mp4_buffer ();
  gsize offset = mp4_box_offset (whole, type);
  GstBuffer *suffix = gst_buffer_copy_region (whole, GST_BUFFER_COPY_MEMORY,
      offset, gst_buffer_get_size (whole) - offset);
  gst_buffer_unref (whole);
  return suffix;
}

/* The file type box of mse.mp4 followed by a box whose size field is
 * smaller than a box header, which the demuxer rejects. The padding gives
 * typefinding the 2 KiB it wants before it looks at the bytes. */
static GstBuffer *
test_mp4_invalid_buffer (void)
{
  static const guint8 bogus_box[4096] = { 0, 0, 0, 4, 'm', 'o', 'o', 'v' };
  GstBuffer *buffer = test_mp4_before_box ("moov");
  return gst_buffer_append (buffer, gst_buffer_new_memdup (bogus_box,
          sizeof (bogus_box)));
}

/* Counts the update-end, error and abort events a source buffer emits, on
 * its event thread, so that a test can wait for them. */
typedef struct
{
  GMutex mutex;
  GCond cond;
  guint update_end;
  guint error;
  guint abort;
} SourceBufferEvents;

static void
count_update_end (GstSourceBuffer * source_buffer, SourceBufferEvents * events)
{
  g_mutex_lock (&events->mutex);
  events->update_end++;
  g_cond_broadcast (&events->cond);
  g_mutex_unlock (&events->mutex);
}

static void
count_error (GstSourceBuffer * source_buffer, SourceBufferEvents * events)
{
  g_mutex_lock (&events->mutex);
  events->error++;
  g_cond_broadcast (&events->cond);
  g_mutex_unlock (&events->mutex);
}

static void
count_abort (GstSourceBuffer * source_buffer, SourceBufferEvents * events)
{
  g_mutex_lock (&events->mutex);
  events->abort++;
  g_cond_broadcast (&events->cond);
  g_mutex_unlock (&events->mutex);
}

static void
source_buffer_events_init (SourceBufferEvents * events,
    GstSourceBuffer * source_buffer)
{
  g_mutex_init (&events->mutex);
  g_cond_init (&events->cond);
  events->update_end = 0;
  events->error = 0;
  events->abort = 0;
  g_signal_connect (source_buffer, "on-update-end",
      G_CALLBACK (count_update_end), events);
  g_signal_connect (source_buffer, "on-error", G_CALLBACK (count_error),
      events);
  g_signal_connect (source_buffer, "on-abort", G_CALLBACK (count_abort),
      events);
}

static void
source_buffer_events_clear (SourceBufferEvents * events,
    GstSourceBuffer * source_buffer)
{
  g_signal_handlers_disconnect_by_data (source_buffer, events);
  g_mutex_clear (&events->mutex);
  g_cond_clear (&events->cond);
}

/* Waits until the source buffer has emitted update-end count times. */
static void
await_update_end (SourceBufferEvents * events, guint count)
{
  gint64 deadline = g_get_monotonic_time () + EVENT_TIMEOUT;
  g_mutex_lock (&events->mutex);
  while (events->update_end < count) {
    if (!g_cond_wait_until (&events->cond, &events->mutex, deadline)) {
      guint have = events->update_end;
      g_mutex_unlock (&events->mutex);
      ck_abort_msg ("timed out waiting for update-end %u, have %u", count,
          have);
    }
  }
  g_mutex_unlock (&events->mutex);
}

static guint
n_active_source_buffers (GstMediaSource * media_source)
{
  GstSourceBufferList *active_buffers =
      gst_media_source_get_active_source_buffers (media_source);
  guint n = gst_source_buffer_list_get_length (active_buffers);
  gst_object_unref (active_buffers);
  return n;
}

/* Waits until the source buffer has emitted error count times. */
static void
await_error (SourceBufferEvents * events, guint count)
{
  gint64 deadline = g_get_monotonic_time () + EVENT_TIMEOUT;
  g_mutex_lock (&events->mutex);
  while (events->error < count) {
    if (!g_cond_wait_until (&events->cond, &events->mutex, deadline)) {
      guint have = events->error;
      g_mutex_unlock (&events->mutex);
      ck_abort_msg ("timed out waiting for error %u, have %u", count, have);
    }
  }
  g_mutex_unlock (&events->mutex);
}

/* An open media source with one MP4 source buffer, the usual fixture for
 * the source buffer tests. */
static GstSourceBuffer *
mp4_source_buffer (GstMediaSource ** media_source)
{
  *media_source = opened_media_source ();
  GstSourceBuffer *source_buffer =
      gst_media_source_add_source_buffer (*media_source, "video/mp4", NULL);
  fail_unless (GST_IS_SOURCE_BUFFER (source_buffer));
  return source_buffer;
}

/* Releases the fixture of mp4_source_buffer() once media was appended.
 * Each track feed holds a reference to the source buffer while it runs
 * and drops it on its own thread, which must not be the one finalizing
 * the buffer: it still holds the feed's task lock, which finalization
 * frees. So the stream is ended, which lets the feeds push EOS and stop,
 * and the fixture's references go only once theirs are gone. */
static void
finish_mp4_source_buffer (GstMediaSource * media_source,
    GstSourceBuffer * source_buffer)
{
  GError *error = NULL;
  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);

  /* The parent, the buffer lists and this test. */
  GstSourceBufferList *active_buffers =
      gst_media_source_get_active_source_buffers (media_source);
  guint fixture_refs = 3;
  for (guint i = 0; i < gst_source_buffer_list_get_length (active_buffers);
      i++) {
    GstSourceBuffer *active = gst_source_buffer_list_index (active_buffers, i);
    if (active == source_buffer) {
      fixture_refs++;
    }
    gst_object_unref (active);
  }
  gst_object_unref (active_buffers);
  gint64 deadline = g_get_monotonic_time () + EVENT_TIMEOUT;
  while (GST_OBJECT_REFCOUNT_VALUE (source_buffer) > fixture_refs
      && g_get_monotonic_time () < deadline) {
    g_usleep (G_TIME_SPAN_MILLISECOND);
  }
  fail_unless_equals_int (GST_OBJECT_REFCOUNT_VALUE (source_buffer),
      fixture_refs);

  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
}

GST_START_TEST (test_create_and_free)
{
  GstMediaSource *media_source = gst_media_source_new ();
  fail_unless (GST_IS_MEDIA_SOURCE (media_source));
  gst_check_object_destroyed_on_unref (media_source);
}

GST_END_TEST;

GST_START_TEST (test_create_initial_state)
{
  GstMediaSource *media_source = gst_media_source_new ();

  GstSourceBufferList *buffers =
      gst_media_source_get_source_buffers (media_source);
  GstSourceBufferList *active_buffers =
      gst_media_source_get_active_source_buffers (media_source);

  fail_unless (gst_media_source_get_ready_state (media_source) ==
      GST_MEDIA_SOURCE_READY_STATE_CLOSED);
  fail_unless (gst_source_buffer_list_get_length (buffers) == 0);
  fail_unless (gst_source_buffer_list_get_length (active_buffers) == 0);
  fail_unless (gst_media_source_get_position (media_source) ==
      GST_CLOCK_TIME_NONE);

  gst_object_unref (media_source);
  gst_object_unref (buffers);
  gst_object_unref (active_buffers);
}

GST_END_TEST;

GST_START_TEST (test_add_source_buffer_with_content_type_null)
{
#ifndef G_DISABLE_CHECKS
  add_log_filter (G_LOG_LEVEL_CRITICAL,
      "^.*_add_source_buffer: assertion 'type != NULL' failed");

  GstMediaSource *media_source = gst_media_source_new ();

  g_assert_null (gst_media_source_add_source_buffer (media_source, NULL, NULL));

  gst_object_unref (media_source);
#endif
}

GST_END_TEST;

GST_START_TEST (test_add_source_buffer_with_content_type_empty)
{
  GError *error = NULL;
  GstMediaSource *media_source = gst_media_source_new ();
  GstSourceBuffer *source_buffer =
      gst_media_source_add_source_buffer (media_source, "", &error);

  g_assert_null (source_buffer);
  g_assert_error (error, GST_MEDIA_SOURCE_ERROR, GST_MEDIA_SOURCE_ERROR_TYPE);

  gst_object_unref (media_source);
  g_clear_error (&error);
}

GST_END_TEST;

GST_START_TEST (test_add_source_buffer_with_content_type_fake)
{
  GError *error = NULL;
  GstMediaSource *media_source = gst_media_source_new ();
  GstSourceBuffer *source_buffer =
      gst_media_source_add_source_buffer (media_source, "fake/type", &error);

  g_assert_null (source_buffer);
  g_assert_error (error, GST_MEDIA_SOURCE_ERROR,
      GST_MEDIA_SOURCE_ERROR_NOT_SUPPORTED);

  gst_object_unref (media_source);
  g_clear_error (&error);
}

GST_END_TEST;

GST_START_TEST (test_add_source_buffer_to_unopened_media_source)
{
  GError *error = NULL;
  GstMediaSource *media_source = gst_media_source_new ();
  GstSourceBuffer *source_buffer =
      gst_media_source_add_source_buffer (media_source, "video/webm", &error);

  g_assert_null (source_buffer);
  g_assert_error (error, GST_MEDIA_SOURCE_ERROR,
      GST_MEDIA_SOURCE_ERROR_INVALID_STATE);

  gst_object_unref (media_source);
  g_clear_error (&error);
}

GST_END_TEST;

GST_START_TEST (test_add_source_buffer_to_opened_media_source)
{
  GError *error = NULL;
  GstMediaSource *media_source = opened_media_source ();
  GstSourceBufferList *buffers =
      gst_media_source_get_source_buffers (media_source);
  guint n_buffers_before = gst_source_buffer_list_get_length (buffers);
  GstSourceBuffer *source_buffer =
      gst_media_source_add_source_buffer (media_source, "video/webm", &error);
  guint n_buffers_after = gst_source_buffer_list_get_length (buffers);

  fail_unless (GST_IS_SOURCE_BUFFER (source_buffer));
  g_assert_no_error (error);
  fail_unless (n_buffers_before < n_buffers_after);

  g_object_unref (media_source);
  g_object_unref (buffers);
  g_object_unref (source_buffer);
  g_clear_error (&error);
}

GST_END_TEST;

GST_START_TEST (test_remove_source_buffer_from_unrelated_media_source)
{
  GError *error = NULL;
  GstMediaSource *a = opened_media_source ();
  GstMediaSource *b = opened_media_source ();
  GstSourceBuffer *buffer_in_b =
      gst_media_source_add_source_buffer (b, "video/webm", &error);

  gst_media_source_remove_source_buffer (a, buffer_in_b, &error);
  g_assert_error (error, GST_MEDIA_SOURCE_ERROR,
      GST_MEDIA_SOURCE_ERROR_NOT_FOUND);

  gst_object_unref (a);
  gst_object_unref (b);
  gst_object_unref (buffer_in_b);
  g_clear_error (&error);
}

GST_END_TEST;

GST_START_TEST (test_remove_source_buffer_from_parent_media_source)
{
  GError *error = NULL;
  GstMediaSource *media_source = opened_media_source ();
  GstSourceBufferList *buffers =
      gst_media_source_get_source_buffers (media_source);
  GstSourceBuffer *buffer =
      gst_media_source_add_source_buffer (media_source, "video/webm", &error);

  guint n_buffers_before = gst_source_buffer_list_get_length (buffers);
  gst_media_source_remove_source_buffer (media_source, buffer, &error);
  guint n_buffers_after = gst_source_buffer_list_get_length (buffers);

  g_assert_no_error (error);
  fail_unless (n_buffers_before > n_buffers_after);

  gst_object_unref (media_source);
  gst_object_unref (buffers);
  gst_object_unref (buffer);
  g_clear_error (&error);
}

GST_END_TEST;

GST_START_TEST (test_set_live_seekable_range_on_unopened_media_source)
{
  GError *error = NULL;
  GstMediaSource *media_source = gst_media_source_new ();

  gst_media_source_set_live_seekable_range (media_source, 0, 1, &error);

  g_assert_error (error, GST_MEDIA_SOURCE_ERROR,
      GST_MEDIA_SOURCE_ERROR_INVALID_STATE);

  gst_object_unref (media_source);
  g_clear_error (&error);
}

GST_END_TEST;

GST_START_TEST (test_set_backwards_live_seekable_range_on_opened_media_source)
{
  GError *error = NULL;
  GstMediaSource *media_source = opened_media_source ();

  gst_media_source_set_live_seekable_range (media_source, 2, 1, &error);

  GstMediaSourceRange range = {
    .start = GST_CLOCK_TIME_NONE,
    .end = GST_CLOCK_TIME_NONE,
  };
  gst_media_source_get_live_seekable_range (media_source, &range);

  g_assert_error (error, GST_MEDIA_SOURCE_ERROR, GST_MEDIA_SOURCE_ERROR_TYPE);
  fail_unless (range.start == 0);
  fail_unless (range.end == 0);

  gst_object_unref (media_source);
  g_clear_error (&error);
}

GST_END_TEST;

GST_START_TEST (test_set_live_seekable_range_on_opened_media_source)
{
  GError *error = NULL;
  GstClockTime start = 1, end = 2;
  GstMediaSource *media_source = opened_media_source ();

  gst_media_source_set_live_seekable_range (media_source, start, end, &error);

  GstMediaSourceRange range = {
    .start = GST_CLOCK_TIME_NONE,
    .end = GST_CLOCK_TIME_NONE,
  };
  gst_media_source_get_live_seekable_range (media_source, &range);

  g_assert_no_error (error);
  fail_unless (range.start == start);
  fail_unless (range.end == end);

  gst_object_unref (media_source);
  g_clear_error (&error);
}

GST_END_TEST;

GST_START_TEST (test_clear_live_seekable_range_on_unopened_media_source)
{
  GError *error = NULL;
  GstMediaSource *media_source = gst_media_source_new ();

  gst_media_source_clear_live_seekable_range (media_source, &error);

  g_assert_error (error, GST_MEDIA_SOURCE_ERROR,
      GST_MEDIA_SOURCE_ERROR_INVALID_STATE);

  gst_object_unref (media_source);
  g_clear_error (&error);
}

GST_END_TEST;

GST_START_TEST (test_clear_live_seekable_range_on_opened_media_source)
{
  GError *error = NULL;
  GstMediaSource *media_source = opened_media_source ();
  gst_media_source_set_live_seekable_range (media_source, 1, 2, NULL);

  gst_media_source_clear_live_seekable_range (media_source, &error);
  GstMediaSourceRange range = {
    .start = GST_CLOCK_TIME_NONE,
    .end = GST_CLOCK_TIME_NONE,
  };
  gst_media_source_get_live_seekable_range (media_source, &range);

  g_assert_no_error (error);
  fail_unless (range.start == 0);
  fail_unless (range.end == 0);

  gst_object_unref (media_source);
  g_clear_error (&error);
}

GST_END_TEST;

GST_START_TEST (test_append_pipeline_create_and_free)
{
  GError *error = NULL;
  GstAppendPipeline *pipeline = gst_append_pipeline_new (NULL, NULL, &error);
  g_assert_no_error (error);
  fail_unless (GST_IS_APPEND_PIPELINE (pipeline));
  gst_check_object_destroyed_on_unref (pipeline);
  g_clear_error (&error);
}

GST_END_TEST;

typedef struct
{
  GMutex mutex;
  GCond eos_cond;
  GCond error_cond;
} AppendPipelineTestContext;

static void
test_append_pipeline_eos (GstAppendPipeline * pipeline,
    GstMediaSourceTrack * track, gpointer user_data)
{
  AppendPipelineTestContext *context = user_data;
  g_mutex_lock (&context->mutex);
  g_cond_signal (&context->eos_cond);
  g_mutex_unlock (&context->mutex);
  GST_DEBUG_OBJECT (pipeline, "signalled eos");
}

static void
test_append_pipeline_error (GstAppendPipeline * pipeline, gpointer user_data)
{
  AppendPipelineTestContext *context = user_data;
  g_mutex_lock (&context->mutex);
  g_cond_signal (&context->error_cond);
  g_mutex_unlock (&context->mutex);
  GST_DEBUG_OBJECT (pipeline, "signalled error");
}

static void
test_append_pipeline_await_eos (GstAppendPipeline * pipeline,
    AppendPipelineTestContext * context)
{
  GST_DEBUG_OBJECT (pipeline, "waiting for eos");
  g_mutex_lock (&context->mutex);
  while (!gst_append_pipeline_get_eos (pipeline)) {
    g_cond_wait (&context->eos_cond, &context->mutex);
  }
  g_mutex_unlock (&context->mutex);
  GST_DEBUG_OBJECT (pipeline, "received eos");
}

static void
test_append_pipeline_await_error (GstAppendPipeline * pipeline,
    AppendPipelineTestContext * context)
{
  GST_DEBUG_OBJECT (pipeline, "waiting for error");
  g_mutex_lock (&context->mutex);
  while (!gst_append_pipeline_get_failed (pipeline)) {
    g_cond_wait (&context->error_cond, &context->mutex);
  }
  g_mutex_unlock (&context->mutex);
  GST_DEBUG_OBJECT (pipeline, "received error");
}

static void
test_append_pipeline (const gchar * filename)
{
  AppendPipelineTestContext context = { 0 };
  GstAppendPipelineCallbacks callbacks = {
    .eos = test_append_pipeline_eos,
  };
  GstAppendPipeline *pipeline =
      gst_append_pipeline_new (&callbacks, &context, NULL);
  GError *error = NULL;

  gchar *data;
  gsize length;

  g_file_get_contents (filename, &data, &length, &error);
  g_assert_no_error (error);

  fail_unless (gst_append_pipeline_append (pipeline,
          gst_buffer_new_wrapped (data, length)) == GST_FLOW_OK);

  gst_append_pipeline_eos (pipeline);

  test_append_pipeline_await_eos (pipeline, &context);

  fail_if (gst_append_pipeline_get_failed (pipeline));

  gst_object_unref (pipeline);
  g_clear_error (&error);
}

GST_START_TEST (test_append_pipeline_mp4)
{
  gchar *filename = test_mp4_path ();
  test_append_pipeline (filename);
  g_free (filename);
}

GST_END_TEST;

GST_START_TEST (test_append_pipeline_webm)
{
  gchar *filename = test_webm_path ();
  test_append_pipeline (filename);
  g_free (filename);
}

GST_END_TEST;

static GstAppendPipeline *
failed_append_pipeline (GstAppendPipelineCallbacks * callbacks,
    AppendPipelineTestContext * context)
{
  GstAppendPipeline *pipeline =
      gst_append_pipeline_new (callbacks, context, NULL);

  gst_append_pipeline_fail (pipeline);

  return pipeline;
}

GST_START_TEST (test_append_pipeline_invalid_data_triggers_error)
{
  AppendPipelineTestContext context = { 0 };
  GstAppendPipelineCallbacks callbacks = {
    .eos = test_append_pipeline_eos,
    .error = test_append_pipeline_error,
  };
  GstAppendPipeline *pipeline = failed_append_pipeline (&callbacks, &context);

  test_append_pipeline_await_error (pipeline, &context);

  gst_object_unref (pipeline);
}

GST_END_TEST;

GST_START_TEST (test_append_pipeline_invalid_data_triggers_eos)
{
  AppendPipelineTestContext context = { 0 };
  GstAppendPipelineCallbacks callbacks = {
    .eos = test_append_pipeline_eos,
    .error = test_append_pipeline_error,
  };
  GstAppendPipeline *pipeline = failed_append_pipeline (&callbacks, &context);

  test_append_pipeline_await_eos (pipeline, &context);

  gst_object_unref (pipeline);
}

GST_END_TEST;

GST_START_TEST (test_append_pipeline_reset_recovery)
{
  AppendPipelineTestContext context = { 0 };
  GstAppendPipelineCallbacks callbacks = {
    .eos = test_append_pipeline_eos,
    .error = test_append_pipeline_error,
  };
  GstAppendPipeline *pipeline = failed_append_pipeline (&callbacks, &context);

  test_append_pipeline_await_error (pipeline, &context);
  fail_unless (gst_append_pipeline_get_failed (pipeline));

  fail_unless (gst_append_pipeline_reset (pipeline));
  fail_if (gst_append_pipeline_get_failed (pipeline));

  gchar *data;
  gsize length;
  {
    GError *error = NULL;
    gchar *filename = test_webm_path ();
    g_file_get_contents (filename, &data, &length, &error);
    g_assert_no_error (error);
    g_clear_error (&error);
    g_free (filename);
  }

  fail_unless (gst_append_pipeline_append (pipeline,
          gst_buffer_new_wrapped (data, length)) == GST_FLOW_OK);

  gst_append_pipeline_eos (pipeline);

  test_append_pipeline_await_eos (pipeline, &context);

  fail_if (gst_append_pipeline_get_failed (pipeline));

  gst_object_unref (pipeline);
}

GST_END_TEST;

GST_START_TEST (test_append_pipeline_reset_after_init_segment)
{
  /* Proves a pipeline that has parsed an initialization segment parses the
   * same one again after a reset: the sinks of its old tracks are gone, so
   * the new tracks, with the same names, can be linked. */
  AppendPipelineTestContext context = { 0 };
  GstAppendPipelineCallbacks callbacks = {
    .eos = test_append_pipeline_eos,
    .error = test_append_pipeline_error,
  };
  GstAppendPipeline *pipeline =
      gst_append_pipeline_new (&callbacks, &context, NULL);

  for (guint round = 0; round < 2; round++) {
    fail_unless (gst_append_pipeline_append (pipeline,
            test_mp4_buffer ()) == GST_FLOW_OK);
    gst_append_pipeline_eos (pipeline);
    test_append_pipeline_await_eos (pipeline, &context);
    fail_if (gst_append_pipeline_get_failed (pipeline));
    fail_unless_equals_int (gst_append_pipeline_n_tracks (pipeline), 3);
    fail_unless (gst_append_pipeline_reset (pipeline));
  }

  gst_object_unref (pipeline);
}

GST_END_TEST;

GST_START_TEST (test_track_create_and_free)
{
  GstMediaSourceTrack *track =
      gst_media_source_track_new (GST_MEDIA_SOURCE_TRACK_TYPE_OTHER, "");
  fail_unless (GST_IS_MEDIA_SOURCE_TRACK (track));
  gst_check_object_destroyed_on_unref (track);
}

GST_END_TEST;

GST_START_TEST (test_track_create_with_invalid_type)
{
#ifndef G_DISABLE_CHECKS
  add_log_filter (G_LOG_LEVEL_CRITICAL,
      "^.*track_new_full: assertion .*type .* failed");

  g_assert_null (gst_media_source_track_new (-1, ""));
  g_assert_null (gst_media_source_track_new (GST_MEDIA_SOURCE_TRACK_TYPE_OTHER +
          1, ""));
#endif
}

GST_END_TEST;

GST_START_TEST (test_track_push_with_adequate_space)
{
  GstMediaSourceTrack *track =
      gst_media_source_track_new_with_size (GST_MEDIA_SOURCE_TRACK_TYPE_OTHER,
      "", 1);
  GstBuffer *buffer = gst_buffer_new ();
  GstSample *sample = gst_sample_new (buffer, NULL, NULL, NULL);
  gboolean result = gst_media_source_track_push (track, sample);
  fail_unless (result);
  gst_sample_unref (sample);
  gst_buffer_unref (buffer);
  gst_object_unref (track);
}

GST_END_TEST;

GST_START_TEST (test_track_buffer_empty)
{
  GstMediaSourceTrackBuffer *buffer = gst_media_source_track_buffer_new ();

  GArray *ranges = gst_media_source_track_buffer_get_ranges (buffer);
  fail_unless_equals_uint64 (ranges->len, 0);

  gst_object_unref (buffer);
  g_array_unref (ranges);
}

GST_END_TEST;

GST_START_TEST (test_track_buffer_single_span)
{
  GstMediaSourceTrackBuffer *buffer = gst_media_source_track_buffer_new ();

  GstSample *sample = new_empty_sample_with_timing (0, 0, 1);
  gst_media_source_track_buffer_add (buffer, sample);

  GArray *ranges = gst_media_source_track_buffer_get_ranges (buffer);
  fail_unless_equals_uint64 (ranges->len, 1);

  GstMediaSourceRange range = g_array_index (ranges, GstMediaSourceRange, 0);
  fail_unless_equals_uint64 (range.start, 0);
  fail_unless_equals_uint64 (range.end, 1);

  gst_sample_unref (sample);
  gst_object_unref (buffer);
  g_array_unref (ranges);
}

GST_END_TEST;

GST_START_TEST (test_track_buffer_continuous_span)
{
  GstMediaSourceTrackBuffer *buffer = gst_media_source_track_buffer_new ();

  GstClockTime a_start = 0;
  GstClockTime a_duration = GST_SECOND;
  GstClockTime b_start = a_start + a_duration;
  GstClockTime b_duration = a_duration;
  GstSample *a = new_empty_sample_with_timing (a_start, a_start, a_duration);
  GstSample *b = new_empty_sample_with_timing (b_start, b_start, b_duration);
  gst_media_source_track_buffer_add (buffer, a);
  gst_media_source_track_buffer_add (buffer, b);

  GArray *ranges = gst_media_source_track_buffer_get_ranges (buffer);
  fail_unless_equals_uint64 (ranges->len, 1);

  GstMediaSourceRange range = g_array_index (ranges, GstMediaSourceRange, 0);
  fail_unless_equals_uint64 (range.start, a_start);
  fail_unless_equals_uint64 (range.end, a_start + a_duration + b_duration);

  gst_sample_unref (a);
  gst_sample_unref (b);
  gst_object_unref (buffer);
  g_array_unref (ranges);
}

GST_END_TEST;

GST_START_TEST (test_track_buffer_discontinuous_span)
{
  GstMediaSourceTrackBuffer *buffer = gst_media_source_track_buffer_new ();

  GstClockTime a_start = 0;
  GstClockTime a_duration = GST_SECOND;
  GstClockTime b_start = a_start + (a_duration * 3) + 1;
  GstClockTime b_duration = a_duration;
  GstSample *a = new_empty_sample_with_timing (a_start, a_start, a_duration);
  GstSample *b = new_empty_sample_with_timing (b_start, b_start, b_duration);
  gst_media_source_track_buffer_add (buffer, a);
  gst_media_source_track_buffer_add (buffer, b);

  GArray *ranges = gst_media_source_track_buffer_get_ranges (buffer);
  fail_unless_equals_uint64 (ranges->len, 2);

  GstMediaSourceRange range_a = g_array_index (ranges, GstMediaSourceRange, 0);
  fail_unless_equals_uint64 (range_a.start, a_start);
  fail_unless_equals_uint64 (range_a.end, a_start + a_duration);

  GstMediaSourceRange range_b = g_array_index (ranges, GstMediaSourceRange, 1);
  fail_unless_equals_uint64 (range_b.start, b_start);
  fail_unless_equals_uint64 (range_b.end, b_start + b_duration);

  gst_sample_unref (a);
  gst_sample_unref (b);
  gst_object_unref (buffer);
  g_array_unref (ranges);
}

GST_END_TEST;

GST_START_TEST (test_source_buffer_generate_timestamps_mp4)
{
  GstMediaSource *media_source = opened_media_source ();
  GstSourceBuffer *source_buffer = gst_media_source_add_source_buffer
      (media_source, "video/mp4", NULL);

  fail_unless_equals_uint64 (gst_source_buffer_get_append_mode (source_buffer),
      GST_SOURCE_BUFFER_APPEND_MODE_SEGMENTS);

  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
}

GST_END_TEST;

GST_START_TEST (test_source_buffer_generate_timestamps_aac)
{
  GstMediaSource *media_source = opened_media_source ();
  GstSourceBuffer *source_buffer = gst_media_source_add_source_buffer
      (media_source, "audio/aac", NULL);

  fail_unless (GST_IS_SOURCE_BUFFER (source_buffer));

  fail_unless_equals_uint64 (gst_source_buffer_get_append_mode (source_buffer),
      GST_SOURCE_BUFFER_APPEND_MODE_SEQUENCE);

  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
}

GST_END_TEST;

GST_START_TEST (test_source_buffer_change_content_type_null)
{
  GstMediaSource *media_source = opened_media_source ();
  GstSourceBuffer *source_buffer = gst_media_source_add_source_buffer
      (media_source, "video/mp4", NULL);

  fail_unless (GST_IS_SOURCE_BUFFER (source_buffer));

  GError *error = NULL;
  gst_source_buffer_change_content_type (source_buffer, NULL, &error);
  g_assert_error (error, GST_MEDIA_SOURCE_ERROR, GST_MEDIA_SOURCE_ERROR_TYPE);

  g_clear_error (&error);
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
}

GST_END_TEST;

GST_START_TEST (test_source_buffer_change_content_type_empty)
{
  GstMediaSource *media_source = opened_media_source ();
  GstSourceBuffer *source_buffer = gst_media_source_add_source_buffer
      (media_source, "video/mp4", NULL);

  fail_unless (GST_IS_SOURCE_BUFFER (source_buffer));

  GError *error = NULL;
  gst_source_buffer_change_content_type (source_buffer, "", &error);
  g_assert_error (error, GST_MEDIA_SOURCE_ERROR, GST_MEDIA_SOURCE_ERROR_TYPE);

  g_clear_error (&error);
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
}

GST_END_TEST;

GST_START_TEST (test_source_buffer_change_content_type)
{
  GstMediaSource *media_source = opened_media_source ();
  GstSourceBuffer *source_buffer = gst_media_source_add_source_buffer
      (media_source, "video/mp4", NULL);

  fail_unless (GST_IS_SOURCE_BUFFER (source_buffer));

  GError *error = NULL;
  gst_source_buffer_change_content_type (source_buffer, "video/webm", &error);
  g_assert_error (error, GST_MEDIA_SOURCE_ERROR,
      GST_MEDIA_SOURCE_ERROR_NOT_SUPPORTED);

  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
  g_clear_error (&error);
}

GST_END_TEST;

typedef struct
{
  GstMediaSource *media_source;
  /* The test's own reference, dropped inside the handler. */
  GstSourceBuffer *source_buffer;
  GMutex mutex;
  GCond cond;
  gboolean disposed;
} RemoveInHandlerContext;

static void
remove_source_buffer_in_handler (GstSourceBuffer * source_buffer,
    RemoveInHandlerContext * context)
{
  gst_media_source_remove_source_buffer (context->media_source,
      source_buffer, NULL);
  gst_clear_object (&context->source_buffer);
}

static void
source_buffer_disposed (RemoveInHandlerContext * context,
    GObject * where_the_object_was)
{
  g_mutex_lock (&context->mutex);
  context->disposed = TRUE;
  g_cond_broadcast (&context->cond);
  g_mutex_unlock (&context->mutex);
}

GST_START_TEST (test_source_buffer_set_append_window_start)
{
  /* Proves the append window start can be moved while the window end is
   * still unbounded, and afterwards to any point before the end. */
  GstMediaSource *media_source;
  GstSourceBuffer *source_buffer = mp4_source_buffer (&media_source);
  GError *error = NULL;

  fail_unless_equals_uint64 (gst_source_buffer_get_append_window_end
      (source_buffer), GST_CLOCK_TIME_NONE);
  fail_unless (gst_source_buffer_set_append_window_start (source_buffer,
          GST_SECOND, &error));
  g_assert_no_error (error);
  fail_unless_equals_uint64 (gst_source_buffer_get_append_window_start
      (source_buffer), GST_SECOND);

  fail_unless (gst_source_buffer_set_append_window_end (source_buffer,
          3 * GST_SECOND, &error));
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_set_append_window_start (source_buffer,
          2 * GST_SECOND, &error));
  g_assert_no_error (error);

  fail_if (gst_source_buffer_set_append_window_start (source_buffer,
          3 * GST_SECOND, &error));
  g_assert_error (error, GST_MEDIA_SOURCE_ERROR, GST_MEDIA_SOURCE_ERROR_TYPE);
  g_clear_error (&error);
  fail_if (gst_source_buffer_set_append_window_start (source_buffer,
          GST_CLOCK_TIME_NONE, &error));
  g_assert_error (error, GST_MEDIA_SOURCE_ERROR, GST_MEDIA_SOURCE_ERROR_TYPE);
  g_clear_error (&error);
  fail_unless_equals_uint64 (gst_source_buffer_get_append_window_start
      (source_buffer), 2 * GST_SECOND);

  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
}

GST_END_TEST;

GST_START_TEST (test_source_buffer_buffered_at_update_end)
{
  /* Proves that when update-end fires for an append, the media it carried
   * is already in the buffered ranges. */
  GstMediaSource *media_source;
  GstSourceBuffer *source_buffer = mp4_source_buffer (&media_source);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  GError *error = NULL;

  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);
  fail_if (gst_source_buffer_get_updating (source_buffer));

  GArray *buffered = gst_source_buffer_get_buffered (source_buffer, &error);
  g_assert_no_error (error);
  fail_unless_equals_int (buffered->len, 1);
  GstMediaSourceRange range = g_array_index (buffered, GstMediaSourceRange, 0);
  fail_unless (range.start < GST_SECOND);
  fail_unless (range.end > range.start);
  g_array_unref (buffered);

  source_buffer_events_clear (&events, source_buffer);
  finish_mp4_source_buffer (media_source, source_buffer);
}

GST_END_TEST;

GST_START_TEST (test_source_buffer_append_after_abort)
{
  /* Proves an append after abort(), which starts over with an
   * initialization segment, lands in the track buffers the source buffer
   * already has: its tracks are the same objects as before, so the feeds
   * keyed by them accept the new media. */
  GstMediaSource *media_source;
  GstSourceBuffer *source_buffer = mp4_source_buffer (&media_source);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  GError *error = NULL;

  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);

  fail_unless (gst_source_buffer_abort (source_buffer, &error));
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 2);
  fail_unless_equals_int (events.error, 0);

  GArray *buffered = gst_source_buffer_get_buffered (source_buffer, &error);
  g_assert_no_error (error);
  fail_unless_equals_int (buffered->len, 1);
  g_array_unref (buffered);

  source_buffer_events_clear (&events, source_buffer);
  finish_mp4_source_buffer (media_source, source_buffer);
}

GST_END_TEST;

GST_START_TEST (test_source_buffer_parse_error_reported_once)
{
  /* Proves an append of bytes the parser rejects ends with one error and
   * one update-end, and that the buffer then accepts an append that starts
   * over with an initialization segment. */
  GstMediaSource *media_source;
  GstSourceBuffer *source_buffer = mp4_source_buffer (&media_source);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  GError *error = NULL;

  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_invalid_buffer (), &error));
  g_assert_no_error (error);
  await_error (&events, 1);
  await_update_end (&events, 1);
  fail_if (gst_source_buffer_get_updating (source_buffer));
  /* A second report of the same failure would follow at once. */
  g_usleep (100 * G_TIME_SPAN_MILLISECOND);
  fail_unless_equals_int (events.error, 1);
  fail_unless_equals_int (events.update_end, 1);
  fail_unless_equals_int (gst_media_source_get_ready_state (media_source),
      GST_MEDIA_SOURCE_READY_STATE_ENDED);

  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 2);
  fail_unless_equals_int (events.error, 1);
  GArray *buffered = gst_source_buffer_get_buffered (source_buffer, &error);
  g_assert_no_error (error);
  fail_unless_equals_int (buffered->len, 1);
  g_array_unref (buffered);

  source_buffer_events_clear (&events, source_buffer);
  finish_mp4_source_buffer (media_source, source_buffer);
}

GST_END_TEST;

GST_START_TEST (test_source_buffer_remove_range)
{
  /* Proves remove() takes the media out of the buffered ranges, as an
   * update with its events. */
  GstMediaSource *media_source;
  GstSourceBuffer *source_buffer = mp4_source_buffer (&media_source);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  GError *error = NULL;

  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);
  GArray *buffered = gst_source_buffer_get_buffered (source_buffer, &error);
  g_assert_no_error (error);
  fail_unless_equals_int (buffered->len, 1);
  g_array_unref (buffered);

  fail_unless (gst_source_buffer_remove (source_buffer, 0,
          GST_CLOCK_TIME_NONE, &error));
  g_assert_no_error (error);
  await_update_end (&events, 2);
  fail_if (gst_source_buffer_get_updating (source_buffer));
  fail_unless_equals_int (events.error, 0);
  buffered = gst_source_buffer_get_buffered (source_buffer, &error);
  g_assert_no_error (error);
  fail_unless_equals_int (buffered->len, 0);
  g_array_unref (buffered);

  source_buffer_events_clear (&events, source_buffer);
  finish_mp4_source_buffer (media_source, source_buffer);
}

GST_END_TEST;

GST_START_TEST (test_source_buffer_remove_reopens_ended_source)
{
  /* Proves remove() on a buffer of an ended media source reopens the
   * source, as appending does. */
  GstMediaSource *media_source;
  GstSourceBuffer *source_buffer = mp4_source_buffer (&media_source);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  GError *error = NULL;

  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);
  fail_unless_equals_int (gst_media_source_get_ready_state (media_source),
      GST_MEDIA_SOURCE_READY_STATE_ENDED);

  fail_unless (gst_source_buffer_remove (source_buffer, 0,
          GST_CLOCK_TIME_NONE, &error));
  g_assert_no_error (error);
  fail_unless_equals_int (gst_media_source_get_ready_state (media_source),
      GST_MEDIA_SOURCE_READY_STATE_OPEN);
  await_update_end (&events, 1);

  source_buffer_events_clear (&events, source_buffer);
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
}

GST_END_TEST;

GST_START_TEST (test_source_buffer_init_segment_alone_activates)
{
  /* Proves an MP4 initialization segment on its own gives the source
   * buffer its tracks, so that it joins the active source buffers by the
   * time its append ends, when every stream's parser takes its caps from
   * the sample description: here H.264 from avcC and AAC from esds. A
   * stream whose parser needs a frame first, MPEG audio in mse.mp4, keeps
   * the buffer inactive until the first media segment. */
  GstMediaSource *media_source;
  GstSourceBuffer *source_buffer = mp4_source_buffer (&media_source);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  GError *error = NULL;

  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_file_buffer ("mse-h264-aac-init.mp4"), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);
  fail_unless_equals_int (events.error, 0);
  fail_unless_equals_int (n_active_source_buffers (media_source), 1);

  source_buffer_events_clear (&events, source_buffer);
  finish_mp4_source_buffer (media_source, source_buffer);

  source_buffer = mp4_source_buffer (&media_source);
  source_buffer_events_init (&events, source_buffer);

  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_before_box ("moof"), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);
  fail_unless_equals_int (n_active_source_buffers (media_source), 0);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_from_box ("moof"), &error));
  g_assert_no_error (error);
  await_update_end (&events, 2);
  fail_unless_equals_int (events.error, 0);
  fail_unless_equals_int (n_active_source_buffers (media_source), 1);

  source_buffer_events_clear (&events, source_buffer);
  finish_mp4_source_buffer (media_source, source_buffer);
}

GST_END_TEST;


GST_START_TEST (test_source_buffer_removed_in_event_handler)
{
  /* Proves a source buffer can be removed, and lose its last reference,
   * inside the handler of one of its own events: the handler runs on the
   * buffer's event thread, which is then the thread freeing the buffer. */
  RemoveInHandlerContext context = { 0 };
  g_mutex_init (&context.mutex);
  g_cond_init (&context.cond);
  context.source_buffer = mp4_source_buffer (&context.media_source);
  g_object_weak_ref (G_OBJECT (context.source_buffer),
      (GWeakNotify) source_buffer_disposed, &context);
  g_signal_connect (context.source_buffer, "on-update-end",
      G_CALLBACK (remove_source_buffer_in_handler), &context);

  /* An append of no bytes still ends with update-end, and leaves no media
   * behind whose track feed would keep the buffer alive. */
  GError *error = NULL;
  fail_unless (gst_source_buffer_append_buffer (context.source_buffer,
          gst_buffer_new (), &error));
  g_assert_no_error (error);

  gint64 deadline = g_get_monotonic_time () + EVENT_TIMEOUT;
  g_mutex_lock (&context.mutex);
  gboolean disposed = context.disposed;
  while (!disposed
      && g_cond_wait_until (&context.cond, &context.mutex, deadline)) {
    disposed = context.disposed;
  }
  g_mutex_unlock (&context.mutex);
  fail_unless (disposed, "the source buffer was not freed by its handler");

  GstSourceBufferList *buffers =
      gst_media_source_get_source_buffers (context.media_source);
  fail_unless_equals_int (gst_source_buffer_list_get_length (buffers), 0);
  gst_object_unref (buffers);

  gst_object_unref (context.media_source);
  g_mutex_clear (&context.mutex);
  g_cond_clear (&context.cond);
}

GST_END_TEST;

static const gchar *unsupported_content_types[] = {
  "xxx",
  "text/html",
  "image/jpeg",
};

GST_START_TEST (test_media_source_unsupported_content_type)
{
  const gchar *content_type = unsupported_content_types[__i__];
  ck_assert_msg (!gst_media_source_is_type_supported (content_type),
      "%s should be rejected as an unsupported MIME type", content_type);
}

GST_END_TEST;

static const gchar *valid_mp4_content_types[] = {
  "video/mp4;codecs=\"avc1.4d001e\"",   // H.264 Main Profile level 3.0
  "video/mp4;codecs=\"avc1.42001e\"",   // H.264 Baseline Profile level 3.0
  "audio/mp4;codecs=\"mp4a.40.2\"",     // MPEG4 AAC-LC
  "audio/mp4;codecs=\"mp4a.40.5\"",     // MPEG4 HE-AAC
  "audio/mp4;codecs=\"mp4a.67\"",       // MPEG2 AAC-LC
  "video/mp4;codecs=\"mp4a.40.2\"",
  "video/mp4;codecs=\"avc1.4d001e,mp4a.40.2\"",
  "video/mp4;codecs=\"mp4a.40.2 , avc1.4d001e \"",
  "video/mp4;codecs=\"avc1.4d001e,mp4a.40.5\"",
  "audio/mp4;codecs=\"Opus\"",
  "video/mp4;codecs=\"Opus\"",
  "audio/mp4;codecs=\"fLaC\"",
  "video/mp4;codecs=\"fLaC\"",
};

GST_START_TEST (test_media_source_supported_mp4_content_type)
{
  const gchar *content_type = valid_mp4_content_types[__i__];
  ck_assert_msg (gst_media_source_is_type_supported (content_type),
      "%s must be a supported MP4 content type", content_type);
}

GST_END_TEST;

static const gchar *valid_webm_content_types[] = {
  "video/webm;codecs=\"vp8\"",
  "video/webm;codecs=\"vorbis\"",
  "video/webm;codecs=\"vp8,vorbis\"",
  "video/webm;codecs=\"vorbis, vp8\"",
  "audio/webm;codecs=\"vorbis\"",
  "AUDIO/WEBM;CODECS=\"vorbis\"",
  "audio/webm;codecs=vorbis;test=\"6\"",
  "audio/webm;codecs=\"opus\"",
  "video/webm;codecs=\"opus\"",
};

GST_START_TEST (test_media_source_supported_webm_content_type)
{
  const gchar *content_type = valid_webm_content_types[__i__];
  ck_assert_msg (gst_media_source_is_type_supported (content_type),
      "%s must be a supported WebM content type", content_type);
}

GST_END_TEST;

GST_START_TEST (test_sample_map_create_and_destroy)
{
  GstMediaSourceSampleMap *map = gst_media_source_sample_map_new ();
  gst_check_object_destroyed_on_unref (map);
}

GST_END_TEST;

GST_START_TEST (test_sample_map_add_valid_sample)
{
  GstMediaSourceSampleMap *map = gst_media_source_sample_map_new ();

  GstSample *sample = new_empty_sample_with_timing (0, 0, 0);

  fail_if (gst_media_source_sample_map_contains (map, sample));

  gst_media_source_sample_map_add (map, sample);

  fail_unless (gst_media_source_sample_map_contains (map, sample));

  gst_object_unref (map);
  gst_sample_unref (sample);
}

GST_END_TEST;

GST_START_TEST (test_sample_map_add_invalid_sample)
{
#ifndef G_DISABLE_CHECKS
  add_log_filter (G_LOG_LEVEL_CRITICAL,
      "^.*_sample_map_add: assertion .* failed");

  GstMediaSourceSampleMap *map = gst_media_source_sample_map_new ();

  GstSample *sample = new_empty_sample_with_timing (GST_CLOCK_TIME_NONE,
      GST_CLOCK_STIME_NONE, GST_CLOCK_TIME_NONE);

  gst_media_source_sample_map_add (map, sample);

  fail_if (gst_media_source_sample_map_contains (map, sample));

  gst_object_unref (map);
  gst_sample_unref (sample);
#endif
}

GST_END_TEST;

GST_START_TEST (test_sample_map_remove_sample)
{
  GstMediaSourceSampleMap *map = gst_media_source_sample_map_new ();

  GstSample *sample = new_empty_sample_with_timing (0, 0, 0);
  gst_media_source_sample_map_add (map, sample);

  gst_media_source_sample_map_remove (map, sample);

  fail_if (gst_media_source_sample_map_contains (map, sample));

  gst_object_unref (map);
  gst_sample_unref (sample);
}

GST_END_TEST;

GST_START_TEST (test_sample_map_remove_range_from_start)
{
  GstMediaSourceSampleMap *map = gst_media_source_sample_map_new ();

  GstSample *samples_to_remove[100] = { NULL };
  for (guint i = 0; i < G_N_ELEMENTS (samples_to_remove); i++) {
    GstClockTime time = i;
    GstSample *sample = new_empty_sample_with_timing (time, time, 1);
    if (i > 0) {
      GstBuffer *head = gst_sample_get_buffer (sample);
      GST_BUFFER_FLAGS (head) = GST_BUFFER_FLAG_DELTA_UNIT;
    }
    gst_media_source_sample_map_add (map, sample);
    samples_to_remove[i] = sample;
  }
  GstSample *samples_to_preserve[100] = { NULL };
  for (guint i = 0; i < G_N_ELEMENTS (samples_to_preserve); i++) {
    GstClockTime time = i + G_N_ELEMENTS (samples_to_remove);
    GstSample *sample = new_empty_sample_with_timing (time, time, 0);
    if (i > 0) {
      GstBuffer *head = gst_sample_get_buffer (sample);
      GST_BUFFER_FLAGS (head) = GST_BUFFER_FLAG_DELTA_UNIT;
    }
    gst_media_source_sample_map_add (map, sample);
    samples_to_preserve[i] = sample;
  }

  gst_media_source_sample_map_remove_range (map, 0, 100);

  for (guint i = 0; i < G_N_ELEMENTS (samples_to_remove); i++) {
    GstSample *sample = samples_to_remove[i];
    fail_if (gst_media_source_sample_map_contains (map, sample));
    gst_sample_unref (sample);
  }
  for (guint i = 0; i < G_N_ELEMENTS (samples_to_preserve); i++) {
    GstSample *sample = samples_to_preserve[i];
    fail_unless (gst_media_source_sample_map_contains (map, sample));
    gst_sample_unref (sample);
  }

  gst_object_unref (map);
}

GST_END_TEST;

GST_START_TEST (test_sample_map_remove_range_from_start_byte_count)
{
  GstMediaSourceSampleMap *map = gst_media_source_sample_map_new ();

  GstSample *samples_to_remove[100] = { NULL };
  const guint8 chunk[1000] = { 0 };
  gsize total_bytes_to_remove = 0;
  for (guint i = 0; i < G_N_ELEMENTS (samples_to_remove); i++) {
    GstClockTime time = i;
    gsize buffer_size = g_random_int_range (0, G_N_ELEMENTS (chunk));
    GBytes *bytes = g_bytes_new_static (chunk, buffer_size);
    total_bytes_to_remove += buffer_size;
    GstSample *sample = new_sample_with_bytes_and_timing (bytes, time, time, 1);
    if (i > 0) {
      GstBuffer *head = gst_sample_get_buffer (sample);
      GST_BUFFER_FLAGS (head) = GST_BUFFER_FLAG_DELTA_UNIT;
    }
    gst_media_source_sample_map_add (map, sample);
    samples_to_remove[i] = sample;
  }
  GstSample *samples_to_preserve[100] = { NULL };
  for (guint i = 0; i < G_N_ELEMENTS (samples_to_remove); i++) {
    GstClockTime time = i + G_N_ELEMENTS (samples_to_remove);
    GBytes *bytes = g_bytes_new_static (chunk, 1);
    GstSample *sample = new_sample_with_bytes_and_timing (bytes, time, time, 0);
    if (i > 0) {
      GstBuffer *head = gst_sample_get_buffer (sample);
      GST_BUFFER_FLAGS (head) = GST_BUFFER_FLAG_DELTA_UNIT;
    }
    gst_media_source_sample_map_add (map, sample);
    samples_to_preserve[i] = sample;
  }

  gsize bytes_removed = gst_media_source_sample_map_remove_range (map, 0,
      G_N_ELEMENTS (samples_to_remove));
  fail_unless_equals_uint64 (bytes_removed, total_bytes_to_remove);

  for (guint i = 0; i < G_N_ELEMENTS (samples_to_remove); i++) {
    gst_sample_unref (samples_to_remove[i]);
  }
  for (guint i = 0; i < G_N_ELEMENTS (samples_to_preserve); i++) {
    gst_sample_unref (samples_to_preserve[i]);
  }

  gst_object_unref (map);
}

GST_END_TEST;

GST_START_TEST (test_sample_map_remove_range_removes_coded_frame_group)
{
  GstMediaSourceSampleMap *map = gst_media_source_sample_map_new ();

  static const GstClockTime duration = 1;
  GstSample *keyframe0 = new_empty_sample_with_timing_and_flags (0, 0,
      duration, 0);
  GstSample *delta0 = new_empty_sample_with_timing_and_flags (1, 1,
      duration, GST_BUFFER_FLAG_DELTA_UNIT);
  GstSample *keyframe1 = new_empty_sample_with_timing_and_flags (2, 2,
      duration, 0);
  GstSample *delta1 = new_empty_sample_with_timing_and_flags (3, 3,
      duration, GST_BUFFER_FLAG_DELTA_UNIT);

  GstSample *timeline[] = { keyframe0, delta0, keyframe1, delta1, NULL };

  gst_media_source_sample_map_add (map, keyframe0);
  gst_media_source_sample_map_add (map, delta0);
  gst_media_source_sample_map_add (map, keyframe1);
  gst_media_source_sample_map_add (map, delta1);

  GstClockTime start = __i__ * 2;
  GstClockTime end = start + duration * 2;

  gst_media_source_sample_map_remove_range (map, start, end);

  for (GstSample ** it = timeline; *it != NULL; it++) {
    GstSample *sample = *it;
    GstClockTime sample_start = sample_dts (sample);
    GstClockTime sample_end = sample_start + duration;
    if (sample_start >= start && sample_end <= end) {
      fail_if (gst_media_source_sample_map_contains (map, sample));
    } else {
      fail_unless (gst_media_source_sample_map_contains (map, sample));
    }
  }

  gst_object_unref (map);
  gst_sample_unref (keyframe0);
  gst_sample_unref (delta0);
  gst_sample_unref (keyframe1);
  gst_sample_unref (delta1);
}

GST_END_TEST;

typedef struct
{
  GstClockTime start;
  GstClockTime end;
  /* Whether each of the three coded frame groups is removed. */
  gboolean removed[3];
} RemoveRangeCase;

static const RemoveRangeCase remove_range_cases[] = {
  /* A group that starts inside the range goes whole, past the range's
   * end. */
  {2, 3, {FALSE, TRUE, FALSE}},
  /* A group that ends exactly at the range's end is inside it. */
  {0, 4, {TRUE, TRUE, FALSE}},
};

GST_START_TEST (test_sample_map_remove_range_group_boundaries)
{
  /* Proves which coded frame groups a range removes: those that start in
   * it, whole, and the ones after them up to the first group that starts
   * at or after the end of the range. */
  GstMediaSourceSampleMap *map = gst_media_source_sample_map_new ();
  const RemoveRangeCase *testcase = &remove_range_cases[__i__];

  /* Three groups of a key frame and a delta frame, one time unit each:
   * [0, 2), [2, 4) and [4, 6). */
  GstSample *samples[6];
  for (guint i = 0; i < G_N_ELEMENTS (samples); i++) {
    samples[i] = new_empty_sample_with_timing_and_flags (i, i, 1,
        i % 2 == 0 ? 0 : GST_BUFFER_FLAG_DELTA_UNIT);
    gst_media_source_sample_map_add (map, samples[i]);
  }

  gst_media_source_sample_map_remove_range (map, testcase->start,
      testcase->end);

  for (guint i = 0; i < G_N_ELEMENTS (samples); i++) {
    if (testcase->removed[i / 2]) {
      fail_if (gst_media_source_sample_map_contains (map, samples[i]),
          "sample %u should have been removed", i);
    } else {
      fail_unless (gst_media_source_sample_map_contains (map, samples[i]),
          "sample %u should have been kept", i);
    }
    gst_sample_unref (samples[i]);
  }

  gst_object_unref (map);
}

GST_END_TEST;

static void
sample_map_iterate_size (const GValue * item, gpointer user_data)
{
  guint *counter = (guint *) user_data;
  *counter = *counter + 1;
}

GST_START_TEST (test_sample_map_empty_iterator_by_dts)
{
  GstMediaSourceSampleMap *map = gst_media_source_sample_map_new ();

  guint counter = 0;
  GMutex mutex = { 0 };
  guint32 cookie = 0;

  GstIterator *it = gst_media_source_sample_map_iter_samples_by_dts (map,
      &mutex, &cookie);
  gst_iterator_foreach (it, sample_map_iterate_size, &counter);
  gst_iterator_free (it);

  assert_equals_int (counter, 0);

  gst_object_unref (map);
}

GST_END_TEST;

GST_START_TEST (test_sample_map_grouped_iterator_by_dts)
{
  GstMediaSourceSampleMap *map = gst_media_source_sample_map_new ();

  guint counter = 0;
  GMutex mutex = { 0 };
  guint32 cookie = 0;

  GstSample *key = new_empty_sample_full (0,
      0,
      GST_SECOND,
      0,
      NULL,
      NULL,
      NULL);
  GstSample *delta = new_empty_sample_full (GST_SECOND,
      GST_SECOND,
      GST_SECOND,
      GST_BUFFER_FLAG_DELTA_UNIT,
      NULL,
      NULL,
      NULL);

  gst_media_source_sample_map_add (map, key);
  gst_media_source_sample_map_add (map, delta);

  GstIterator *it = gst_media_source_sample_map_iter_samples_by_dts (map,
      &mutex, &cookie);
  gst_iterator_foreach (it, sample_map_iterate_size, &counter);
  gst_iterator_free (it);

  assert_equals_int (counter, 1);

  gst_object_unref (map);
  gst_clear_sample (&key);
  gst_clear_sample (&delta);
}

GST_END_TEST;

#define DEFAULT_TCASE_TIMEOUT 15

#ifdef HAVE_VALGRIND
#define TCASE_TIMEOUT (RUNNING_ON_VALGRIND ? (5 * 60) : DEFAULT_TCASE_TIMEOUT)
#else
#define TCASE_TIMEOUT DEFAULT_TCASE_TIMEOUT
#endif

static inline TCase *
new_tcase (const gchar * name)
{
  TCase *tcase = tcase_create (name);
  tcase_set_timeout (tcase, TCASE_TIMEOUT);
  return tcase;
}

static Suite *
mse_suite (void)
{
  Suite *s = suite_create ("GstMse");

  TCase *tc_media_source = new_tcase ("GstMediaSource");
  TCase *tc_source_buffer = new_tcase ("GstSourceBuffer");
  TCase *tc_source_buffer_list = new_tcase ("GstSourceBufferList");
  TCase *tc_append_pipeline = new_tcase ("GstAppendPipeline");
  TCase *tc_track = new_tcase ("GstMediaSourceTrack");
  TCase *tc_track_buffer = new_tcase ("GstMediaSourceTrackBuffer");
  TCase *tc_sample_map = new_tcase ("GstMediaSourceSampleMap");

  /* The tests that parse mse.mp4 need its demuxer, from gst-plugins-good. */
  gboolean have_qtdemux = gst_registry_check_feature_version (gst_registry_get
      (), "qtdemux", GST_VERSION_MAJOR, GST_VERSION_MINOR, 0);
  if (!have_qtdemux) {
    GST_WARNING ("qtdemux element not available, skipping the MP4 tests");
  }

  tcase_add_test (tc_media_source, test_create_and_free);
  tcase_add_test (tc_media_source, test_create_initial_state);
  tcase_add_test (tc_media_source,
      test_add_source_buffer_with_content_type_null);
  tcase_add_test (tc_media_source,
      test_add_source_buffer_with_content_type_empty);
  tcase_add_test (tc_media_source,
      test_add_source_buffer_with_content_type_fake);
  tcase_add_test (tc_media_source,
      test_add_source_buffer_to_unopened_media_source);
  tcase_add_test (tc_media_source,
      test_add_source_buffer_to_opened_media_source);
  tcase_add_test (tc_media_source,
      test_remove_source_buffer_from_unrelated_media_source);
  tcase_add_test (tc_media_source,
      test_remove_source_buffer_from_parent_media_source);
  tcase_add_test (tc_media_source,
      test_set_live_seekable_range_on_unopened_media_source);
  tcase_add_test (tc_media_source,
      test_set_backwards_live_seekable_range_on_opened_media_source);
  tcase_add_test (tc_media_source,
      test_set_live_seekable_range_on_opened_media_source);
  tcase_add_test (tc_media_source,
      test_clear_live_seekable_range_on_unopened_media_source);
  tcase_add_test (tc_media_source,
      test_clear_live_seekable_range_on_opened_media_source);
  tcase_add_loop_test (tc_media_source,
      test_media_source_unsupported_content_type,
      0, G_N_ELEMENTS (unsupported_content_types));
  tcase_add_loop_test (tc_media_source,
      test_media_source_supported_mp4_content_type,
      0, G_N_ELEMENTS (valid_mp4_content_types));
  tcase_add_loop_test (tc_media_source,
      test_media_source_supported_webm_content_type,
      0, G_N_ELEMENTS (valid_webm_content_types));

  tcase_add_test (tc_source_buffer, test_source_buffer_generate_timestamps_mp4);
  tcase_add_test (tc_source_buffer, test_source_buffer_generate_timestamps_aac);

  tcase_add_test (tc_source_buffer,
      test_source_buffer_change_content_type_null);
  tcase_add_test (tc_source_buffer,
      test_source_buffer_change_content_type_empty);
  tcase_add_test (tc_source_buffer, test_source_buffer_change_content_type);
  tcase_add_test (tc_source_buffer, test_source_buffer_set_append_window_start);
  tcase_add_test (tc_source_buffer,
      test_source_buffer_remove_reopens_ended_source);
  if (have_qtdemux) {
    tcase_add_test (tc_source_buffer,
        test_source_buffer_removed_in_event_handler);
    tcase_add_test (tc_source_buffer,
        test_source_buffer_buffered_at_update_end);
    tcase_add_test (tc_source_buffer, test_source_buffer_append_after_abort);
    tcase_add_test (tc_source_buffer,
        test_source_buffer_parse_error_reported_once);
    tcase_add_test (tc_source_buffer, test_source_buffer_remove_range);
    tcase_add_test (tc_source_buffer,
        test_source_buffer_init_segment_alone_activates);
  }

  tcase_add_test (tc_append_pipeline, test_append_pipeline_create_and_free);
  tcase_add_test (tc_append_pipeline, test_append_pipeline_mp4);
  tcase_add_test (tc_append_pipeline, test_append_pipeline_webm);
  tcase_add_test (tc_append_pipeline,
      test_append_pipeline_invalid_data_triggers_eos);
  tcase_add_test (tc_append_pipeline,
      test_append_pipeline_invalid_data_triggers_error);
  tcase_add_test (tc_append_pipeline, test_append_pipeline_reset_recovery);
  if (have_qtdemux) {
    tcase_add_test (tc_append_pipeline,
        test_append_pipeline_reset_after_init_segment);
  }

  tcase_add_test (tc_track, test_track_create_and_free);
  tcase_add_test (tc_track, test_track_create_with_invalid_type);
  tcase_add_test (tc_track, test_track_push_with_adequate_space);

  tcase_add_test (tc_track_buffer, test_track_buffer_empty);
  tcase_add_test (tc_track_buffer, test_track_buffer_single_span);
  tcase_add_test (tc_track_buffer, test_track_buffer_continuous_span);
  tcase_add_test (tc_track_buffer, test_track_buffer_discontinuous_span);

  tcase_add_test (tc_sample_map, test_sample_map_create_and_destroy);
  tcase_add_test (tc_sample_map, test_sample_map_add_valid_sample);
  tcase_add_test (tc_sample_map, test_sample_map_add_invalid_sample);
  tcase_add_test (tc_sample_map, test_sample_map_remove_sample);
  tcase_add_test (tc_sample_map, test_sample_map_remove_range_from_start);
  tcase_add_loop_test (tc_sample_map,
      test_sample_map_remove_range_removes_coded_frame_group, 0, 2);
  tcase_add_test (tc_sample_map,
      test_sample_map_remove_range_from_start_byte_count);
  tcase_add_loop_test (tc_sample_map,
      test_sample_map_remove_range_group_boundaries, 0,
      G_N_ELEMENTS (remove_range_cases));
  tcase_add_test (tc_sample_map, test_sample_map_empty_iterator_by_dts);
  tcase_add_test (tc_sample_map, test_sample_map_grouped_iterator_by_dts);

  suite_add_tcase (s, tc_media_source);
  suite_add_tcase (s, tc_source_buffer);
  suite_add_tcase (s, tc_source_buffer_list);
  suite_add_tcase (s, tc_append_pipeline);
  suite_add_tcase (s, tc_track);
  suite_add_tcase (s, tc_track_buffer);
  suite_add_tcase (s, tc_sample_map);

  return s;
}

GST_CHECK_MAIN (mse)

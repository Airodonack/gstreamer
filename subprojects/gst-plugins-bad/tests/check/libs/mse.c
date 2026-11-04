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
#include <gst/check/gsttestclock.h>

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
  while (offset + 8 <= map.size && memcmp (map.data + offset + 4, type, 4) != 0) {
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

typedef void (*Mp4BoxFunc) (guint8 * box, gpointer user_data);

/* Calls func on every box between start and end, and on the boxes inside
 * the containers on the way to the track IDs and the sample descriptions.
 * mse.mp4 has no 64-bit box sizes. */
static void
mp4_foreach_box (guint8 * data, gsize start, gsize end, Mp4BoxFunc func,
    gpointer user_data)
{
  static const gchar *containers[] = { "moov", "trak", "mdia", "minf",
    "stbl", "mvex", "moof", "traf"
  };
  gsize offset = start;
  while (offset + 8 <= end) {
    guint32 size = GST_READ_UINT32_BE (data + offset);
    fail_unless (size >= 8 && offset + size <= end);
    func (data + offset, user_data);
    for (guint i = 0; i < G_N_ELEMENTS (containers); i++) {
      if (memcmp (data + offset + 4, containers[i], 4) == 0) {
        mp4_foreach_box (data, offset + 8, offset + size, func, user_data);
      }
    }
    /* A sample description box is a full box with an entry count, then
     * one box per sample entry. */
    if (memcmp (data + offset + 4, "stsd", 4) == 0) {
      mp4_foreach_box (data, offset + 16, offset + size, func, user_data);
    }
    offset += size;
  }
}

static void
mp4_edit_boxes (GstBuffer * buffer, Mp4BoxFunc func, gpointer user_data)
{
  GstMapInfo map;
  fail_unless (gst_buffer_map (buffer, &map, GST_MAP_READWRITE));
  mp4_foreach_box (map.data, 0, map.size, func, user_data);
  gst_buffer_unmap (buffer, &map);
}

/* Moves the track with ID 1 to ID 5 where a box names it: the track
 * header, the track extends box and every track fragment header. */
static void
renumber_track_1_to_5 (guint8 * box, gpointer user_data)
{
  guint8 *track_id = NULL;
  if (memcmp (box + 4, "tkhd", 4) == 0) {
    /* After the version and flags, a creation and a modification time of
     * 32 bits each in version 0, 64 bits each in version 1. */
    track_id = box + 12 + (box[8] == 1 ? 16 : 8);
  } else if (memcmp (box + 4, "trex", 4) == 0
      || memcmp (box + 4, "tfhd", 4) == 0) {
    track_id = box + 12;
  }
  if (track_id != NULL && GST_READ_UINT32_BE (track_id) == 1) {
    GST_WRITE_UINT32_BE (track_id, 5);
    (*(guint *) user_data)++;
  }
}

/* The bytes of mse.mp4, or the part of them given, with its video track,
 * track 1, renumbered to track 5. */
static GstBuffer *
with_video_track_5 (GstBuffer * buffer)
{
  guint renumbered = 0;
  mp4_edit_boxes (buffer, renumber_track_1_to_5, &renumbered);
  /* The track header and the track extends box, and a track fragment
   * header when there is media. */
  fail_unless (renumbered >= 2);
  return buffer;
}

/* Turns the 3GPP timed text sample entry into a WebVTT one, which qtdemux
 * describes with application/x-subtitle-vtt. */
static void
make_text_webvtt (guint8 * box, gpointer user_data)
{
  if (memcmp (box + 4, "tx3g", 4) == 0) {
    memcpy (box + 4, "wvtt", 4);
    (*(guint *) user_data)++;
  }
}

/* The initialization segment of mse.mp4 with a WebVTT text track. */
static GstBuffer *
test_mp4_init_segment_with_webvtt (void)
{
  GstBuffer *buffer = test_mp4_before_box ("moof");
  guint changed = 0;
  mp4_edit_boxes (buffer, make_text_webvtt, &changed);
  fail_unless_equals_int (changed, 1);
  return buffer;
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

/* A pipeline on the system clock whose running time is an hour in, past
 * the end of any media the tests append, so that msesrc pushes each EOS as
 * soon as its track has run dry. */
static GstElement *
new_pipeline_past_end (void)
{
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstClock *clock = gst_system_clock_obtain ();
  GstClockTime now = gst_clock_get_time (clock);
  gst_pipeline_use_clock (GST_PIPELINE (pipeline), clock);
  gst_element_set_start_time (pipeline, GST_CLOCK_TIME_NONE);
  gst_element_set_base_time (pipeline,
      now > 3600 * GST_SECOND ? now - 3600 * GST_SECOND : 0);
  gst_object_unref (clock);
  return pipeline;
}

/* What msesrc pushed in test_media_source_element_frees_played_media: the
 * memory of its buffers and how much of it is freed, its pads and their EOS
 * events, and the source buffer's update-end events. */
typedef struct
{
  GMutex mutex;
  GCond cond;
  GstElement *pipeline;
  GHashTable *live;
  guint pushed;
  guint freed;
  guint pads;
  guint eos;
  guint update_end;
} PlayedMedia;

static void
played_memory_freed (PlayedMedia * played, GstMiniObject * where)
{
  g_mutex_lock (&played->mutex);
  g_hash_table_remove (played->live, where);
  played->freed++;
  g_cond_broadcast (&played->cond);
  g_mutex_unlock (&played->mutex);
}

static GstPadProbeReturn
watch_played_media (GstPad * pad, GstPadProbeInfo * info, PlayedMedia * played)
{
  g_mutex_lock (&played->mutex);
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER) {
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);
    for (guint i = 0; i < gst_buffer_n_memory (buffer); i++) {
      GstMemory *memory = gst_buffer_peek_memory (buffer, i);
      if (g_hash_table_add (played->live, memory)) {
        gst_mini_object_weak_ref (GST_MINI_OBJECT (memory),
            (GstMiniObjectNotify) played_memory_freed, played);
        played->pushed++;
      }
    }
  } else if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) == GST_EVENT_EOS) {
    played->eos++;
  }
  g_cond_broadcast (&played->cond);
  g_mutex_unlock (&played->mutex);
  return GST_PAD_PROBE_OK;
}

static void
play_into_fakesink (GstElement * src, GstPad * pad, PlayedMedia * played)
{
  /* The probe goes on before the pad is linked, so it sees every buffer. */
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER |
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      (GstPadProbeCallback) watch_played_media, played, NULL);
  GstElement *sink = gst_element_factory_make ("fakesink", NULL);
  g_object_set (sink, "sync", FALSE, NULL);
  gst_bin_add (GST_BIN (played->pipeline), sink);
  GstPad *sink_pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (gst_element_sync_state_with_parent (sink));
  fail_unless_equals_int (gst_pad_link (pad, sink_pad), GST_PAD_LINK_OK);
  gst_object_unref (sink_pad);
  g_mutex_lock (&played->mutex);
  played->pads++;
  g_mutex_unlock (&played->mutex);
}

static void
count_played_update_end (GstSourceBuffer * source_buffer, PlayedMedia * played)
{
  g_mutex_lock (&played->mutex);
  played->update_end++;
  g_cond_broadcast (&played->cond);
  g_mutex_unlock (&played->mutex);
}

/* Waits up to five seconds for `*count` to reach `target`, and returns it. */
static guint
await_played (PlayedMedia * played, const guint * count, const guint * target)
{
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  g_mutex_lock (&played->mutex);
  while ((*count == 0 || *count < *target)
      && g_cond_wait_until (&played->cond, &played->mutex, deadline)) {
  }
  guint result = *count;
  g_mutex_unlock (&played->mutex);
  return result;
}

GST_START_TEST (test_media_source_element_frees_played_media)
{
  /* Proves the media msesrc has played is freed once the pipeline, the
   * media source and its source buffer are gone, instead of living on for
   * as long as the process does. */
  PlayedMedia played = { 0 };
  g_mutex_init (&played.mutex);
  g_cond_init (&played.cond);
  played.live = g_hash_table_new (g_direct_hash, g_direct_equal);
  played.pipeline = new_pipeline_past_end ();
  GstElement *src = g_object_new (GST_TYPE_MSE_SRC, NULL);
  gst_bin_add (GST_BIN (played.pipeline), src);
  g_signal_connect (src, "pad-added", G_CALLBACK (play_into_fakesink), &played);
  GstMediaSource *media_source = gst_media_source_new ();
  gst_media_source_attach (media_source, GST_MSE_SRC (src));
  fail_if (gst_element_set_state (played.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE);

  GError *error = NULL;
  GstSourceBuffer *source_buffer =
      gst_media_source_add_source_buffer (media_source, "video/mp4", &error);
  g_assert_no_error (error);
  g_signal_connect (source_buffer, "on-update-end",
      G_CALLBACK (count_played_update_end), &played);
  gchar *path = test_mp4_path ();
  gchar *data;
  gsize length;
  fail_unless (g_file_get_contents (path, &data, &length, &error));
  g_assert_no_error (error);
  g_free (path);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          gst_buffer_new_wrapped (data, length), &error));
  g_assert_no_error (error);
  const guint one = 1;
  fail_unless_equals_int (await_played (&played, &played.update_end, &one), 1);
  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);
  fail_unless (await_played (&played, &played.eos, &played.pads) > 0);
  fail_unless_equals_int (played.eos, played.pads);

  gst_element_set_state (played.pipeline, GST_STATE_NULL);
  gst_media_source_detach (media_source);
  gst_object_unref (played.pipeline);

  /* Each track feed holds a reference to the source buffer until it has
   * pushed its end of stream, and the media source let go of its own when
   * it was detached: wait for this test's to be the last, so that the
   * source buffer is freed here rather than on a feed's thread. */
  gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (GST_OBJECT_REFCOUNT_VALUE (source_buffer) > 1
      && g_get_monotonic_time () < deadline) {
    g_usleep (G_TIME_SPAN_MILLISECOND);
  }
  fail_unless_equals_int (GST_OBJECT_REFCOUNT_VALUE (source_buffer), 1);
  g_signal_handlers_disconnect_by_data (source_buffer, &played);
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);

  fail_unless (played.pushed > 0, "msesrc pushed no media");
  fail_unless_equals_int (await_played (&played, &played.freed,
          &played.pushed), played.pushed);

  g_hash_table_unref (played.live);
  g_mutex_clear (&played.mutex);
  g_cond_clear (&played.cond);
}

GST_END_TEST;

/* Formats every message, as a log function printing it would. */
static void
format_debug_message (GstDebugCategory * category, GstDebugLevel level,
    const gchar * file, const gchar * function, gint line, GObject * object,
    GstDebugMessage * message, gpointer user_data)
{
  (void) gst_debug_message_get (message);
}

GST_START_TEST (test_media_source_element_seeks_with_debug_output)
{
  /* Proves msesrc handles a seek with its debug output on, which prints the
   * seek event. */
  gst_debug_remove_log_function (gst_debug_log_default);
  gst_debug_add_log_function (format_debug_message, NULL, NULL);
  gst_debug_set_threshold_for_name ("gst-mse", GST_LEVEL_DEBUG);

  GstElement *pipeline = gst_pipeline_new (NULL);
  GstElement *src = g_object_new (GST_TYPE_MSE_SRC, NULL);
  gst_bin_add (GST_BIN (pipeline), src);
  GstMediaSource *media_source = gst_media_source_new ();
  gst_media_source_attach (media_source, GST_MSE_SRC (src));
  fail_if (gst_element_set_state (pipeline, GST_STATE_PAUSED) ==
      GST_STATE_CHANGE_FAILURE);

  fail_unless (gst_element_send_event (src, gst_event_new_seek (1.0,
              GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, 0,
              GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE)));
  fail_if (gst_element_send_event (src, gst_event_new_seek (1.0,
              GST_FORMAT_BYTES, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, 0,
              GST_SEEK_TYPE_NONE, -1)));

  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_media_source_detach (media_source);
  gst_object_unref (media_source);
  gst_object_unref (pipeline);
  gst_debug_unset_threshold_for_name ("gst-mse");
  gst_debug_remove_log_function (format_debug_message);
  gst_debug_add_log_function (gst_debug_log_default, NULL, NULL);
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

typedef struct
{
  AppendPipelineTestContext base;
  gint init_segments;
  GMutex samples_mutex;
  /* Samples received, by track. */
  GHashTable *samples;
} TracksTestContext;

static void
count_init_segment (GstAppendPipeline * pipeline, gpointer user_data)
{
  TracksTestContext *context = user_data;
  g_atomic_int_inc (&context->init_segments);
}

static void
count_sample (GstAppendPipeline * pipeline, GstMediaSourceTrack * track,
    GstSample * sample, gpointer user_data)
{
  TracksTestContext *context = user_data;
  g_mutex_lock (&context->samples_mutex);
  guint count = GPOINTER_TO_UINT (g_hash_table_lookup (context->samples,
          track));
  g_hash_table_insert (context->samples, track, GUINT_TO_POINTER (count + 1));
  g_mutex_unlock (&context->samples_mutex);
}

static guint
samples_for (TracksTestContext * context, GstMediaSourceTrack * track)
{
  g_mutex_lock (&context->samples_mutex);
  guint count = GPOINTER_TO_UINT (g_hash_table_lookup (context->samples,
          track));
  g_mutex_unlock (&context->samples_mutex);
  return count;
}

static GstAppendPipeline *
tracks_test_pipeline (TracksTestContext * context)
{
  GstAppendPipelineCallbacks callbacks = {
    .received_init_segment = count_init_segment,
    .new_sample = count_sample,
    .eos = test_append_pipeline_eos,
    .error = test_append_pipeline_error,
  };
  context->samples = g_hash_table_new (NULL, NULL);
  GstAppendPipeline *pipeline =
      gst_append_pipeline_new (&callbacks, context, NULL);
  GstCaps *caps = gst_caps_new_simple ("video/quicktime", "variant",
      G_TYPE_STRING, "iso", NULL);
  gst_append_pipeline_set_caps (pipeline, caps);
  gst_caps_unref (caps);
  return pipeline;
}

static void
tracks_test_pipeline_free (GstAppendPipeline * pipeline,
    TracksTestContext * context)
{
  gst_object_unref (pipeline);
  g_hash_table_unref (context->samples);
}

static void
assert_track (GstMediaSourceTrack * track, const gchar * media_type)
{
  GstCaps *caps = gst_media_source_track_get_initial_caps (track);
  fail_unless (GST_IS_CAPS (caps));
  GstStructure *structure = gst_caps_get_structure (caps, 0);
  fail_unless_equals_string (gst_structure_get_name (structure), media_type);
  fail_unless (gst_structure_has_field (structure, "codec_data"),
      "%" GST_PTR_FORMAT " has no codec data", caps);
  fail_unless (gst_media_source_track_get_active (track));
}

/* mse-h264-aac-init.mp4 with the duration of its movie header, the first
 * box in its moov, set to the given milliseconds (its timescale). */
static GstBuffer *
init_segment_with_duration (guint32 milliseconds)
{
  GstBuffer *buffer = test_file_buffer ("mse-h264-aac-init.mp4");
  gsize mvhd = mp4_box_offset (buffer, "moov") + 8;
  GstMapInfo map;
  fail_unless (gst_buffer_map (buffer, &map, GST_MAP_READWRITE));
  fail_unless (memcmp (map.data + mvhd + 4, "mvhd", 4) == 0);
  fail_unless_equals_int (map.data[mvhd + 8], 0);
  fail_unless_equals_int (GST_READ_UINT32_BE (map.data + mvhd + 20), 1000);
  GST_WRITE_UINT32_BE (map.data + mvhd + 24, milliseconds);
  gst_buffer_unmap (buffer, &map);
  return buffer;
}

GST_START_TEST (test_append_pipeline_init_segment_alone_has_tracks)
{
  /* Proves an MP4 initialization segment on its own reports its tracks
   * once, each with the codec and codec data of its sample description,
   * and its duration, though no parser has seen a frame: the tracks and
   * the duration come from the demuxer. */
  TracksTestContext context = { 0 };
  GstAppendPipeline *pipeline = tracks_test_pipeline (&context);

  fail_unless (gst_append_pipeline_append (pipeline,
          init_segment_with_duration (4000)) == GST_FLOW_OK);
  gst_append_pipeline_wait_for_appends (pipeline);
  fail_if (gst_append_pipeline_get_failed (pipeline));
  fail_unless_equals_int (g_atomic_int_get (&context.init_segments), 1);
  fail_unless_equals_int (gst_append_pipeline_n_tracks (pipeline), 2);
  fail_unless_equals_uint64 (gst_append_pipeline_get_duration (pipeline),
      4 * GST_SECOND);

  GPtrArray *video_tracks = gst_append_pipeline_get_video_tracks (pipeline);
  GPtrArray *audio_tracks = gst_append_pipeline_get_audio_tracks (pipeline);
  fail_unless (video_tracks != NULL && audio_tracks != NULL,
      "no initialization segment");
  fail_unless_equals_int (video_tracks->len, 1);
  fail_unless_equals_int (audio_tracks->len, 1);
  assert_track (g_ptr_array_index (video_tracks, 0), "video/x-h264");
  assert_track (g_ptr_array_index (audio_tracks, 0), "audio/mpeg");
  GstCaps *audio_caps =
      gst_media_source_track_get_initial_caps (g_ptr_array_index
      (audio_tracks, 0));
  gint mpeg_version = 0;
  gst_structure_get_int (gst_caps_get_structure (audio_caps, 0),
      "mpegversion", &mpeg_version);
  fail_unless_equals_int (mpeg_version, 4);
  g_ptr_array_unref (video_tracks);
  g_ptr_array_unref (audio_tracks);

  tracks_test_pipeline_free (pipeline, &context);
}

GST_END_TEST;

GST_START_TEST (test_append_pipeline_init_segment_new_track_id)
{
  /* Proves a later initialization segment whose only video track has
   * another track ID keeps the video track: the demuxer gives a new pad
   * for the new ID, and the track count stays that of the first
   * initialization segment, so it is not reported as a new track. */
  TracksTestContext context = { 0 };
  GstAppendPipeline *pipeline = tracks_test_pipeline (&context);

  fail_unless (gst_append_pipeline_append (pipeline,
          test_mp4_before_box ("moof")) == GST_FLOW_OK);
  gst_append_pipeline_wait_for_appends (pipeline);
  GPtrArray *first_video = gst_append_pipeline_get_video_tracks (pipeline);
  fail_unless (first_video != NULL, "no initialization segment");
  fail_unless_equals_int (first_video->len, 1);

  fail_unless (gst_append_pipeline_append (pipeline,
          with_video_track_5 (test_mp4_before_box ("moof"))) == GST_FLOW_OK);
  gst_append_pipeline_wait_for_appends (pipeline);
  fail_if (gst_append_pipeline_get_failed (pipeline));
  fail_unless_equals_int (gst_append_pipeline_n_tracks (pipeline), 3);
  fail_unless_equals_int (g_atomic_int_get (&context.init_segments), 1);
  GPtrArray *second_video = gst_append_pipeline_get_video_tracks (pipeline);
  fail_unless_equals_int (second_video->len, 1);
  fail_unless (g_ptr_array_index (second_video, 0) ==
      g_ptr_array_index (first_video, 0));
  g_ptr_array_unref (first_video);
  g_ptr_array_unref (second_video);

  tracks_test_pipeline_free (pipeline, &context);
}

GST_END_TEST;

GST_START_TEST (test_append_pipeline_init_segment_webvtt_track)
{
  /* Proves a WebVTT track of an MP4 initialization segment is a text
   * track: its caps, application/x-subtitle-vtt, name no media type that
   * says so. */
  TracksTestContext context = { 0 };
  GstAppendPipeline *pipeline = tracks_test_pipeline (&context);

  fail_unless (gst_append_pipeline_append (pipeline,
          test_mp4_init_segment_with_webvtt ()) == GST_FLOW_OK);
  gst_append_pipeline_wait_for_appends (pipeline);
  fail_if (gst_append_pipeline_get_failed (pipeline));
  fail_unless_equals_int (g_atomic_int_get (&context.init_segments), 1);
  fail_unless_equals_int (gst_append_pipeline_n_tracks (pipeline), 3);
  GPtrArray *text_tracks = gst_append_pipeline_get_text_tracks (pipeline);
  fail_unless (text_tracks != NULL, "no initialization segment");
  fail_unless_equals_int (text_tracks->len, 1);
  g_ptr_array_unref (text_tracks);

  tracks_test_pipeline_free (pipeline, &context);
}

GST_END_TEST;

GST_START_TEST (test_append_pipeline_stream_type_from_caps)
{
  /* Proves the demuxer's caps give the stream types parsebin gives: the
   * subtitle formats whose media type does not say so are text, and the
   * original media type of an encrypted stream counts only when it is a
   * string. */
  static const struct
  {
    const gchar *caps;
    GstStreamType type;
  } cases[] = {
    {"video/x-h264", GST_STREAM_TYPE_VIDEO},
    {"audio/mpeg", GST_STREAM_TYPE_AUDIO},
    {"text/x-raw", GST_STREAM_TYPE_TEXT},
    {"application/x-subtitle-vtt", GST_STREAM_TYPE_TEXT},
    {"application/x-ssa", GST_STREAM_TYPE_TEXT},
    {"application/x-ass", GST_STREAM_TYPE_TEXT},
    {"application/x-cenc, original-media-type=(string)video/x-h264",
        GST_STREAM_TYPE_VIDEO},
    {"application/x-cenc, original-media-type=(int)1",
        GST_STREAM_TYPE_UNKNOWN},
  };
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
    GstCaps *caps = gst_caps_from_string (cases[i].caps);
    fail_unless (GST_IS_CAPS (caps));
    fail_unless_equals_int (gst_append_pipeline_stream_type_from_caps (caps),
        cases[i].type);
    gst_caps_unref (caps);
  }
}

GST_END_TEST;

GST_START_TEST (test_append_pipeline_media_segment_feeds_init_segment_tracks)
{
  /* Proves the media segment after an initialization segment feeds the
   * tracks that segment reported. parsebin exposes the parsers' pads only
   * once they have seen frames; each pad is matched to the track the
   * demuxer gave for it, so no track is added and the segment is not
   * reported again. */
  TracksTestContext context = { 0 };
  GstAppendPipeline *pipeline = tracks_test_pipeline (&context);

  fail_unless (gst_append_pipeline_append (pipeline,
          test_mp4_before_box ("moof")) == GST_FLOW_OK);
  gst_append_pipeline_wait_for_appends (pipeline);
  fail_unless_equals_int (g_atomic_int_get (&context.init_segments), 1);
  fail_unless_equals_int (gst_append_pipeline_n_tracks (pipeline), 3);
  GPtrArray *video_tracks = gst_append_pipeline_get_video_tracks (pipeline);
  GPtrArray *audio_tracks = gst_append_pipeline_get_audio_tracks (pipeline);
  fail_unless (video_tracks != NULL && audio_tracks != NULL,
      "no initialization segment");
  fail_unless_equals_int (video_tracks->len, 1);
  fail_unless_equals_int (audio_tracks->len, 1);

  fail_unless (gst_append_pipeline_append (pipeline,
          test_mp4_from_box ("moof")) == GST_FLOW_OK);
  gst_append_pipeline_wait_for_appends (pipeline);
  fail_if (gst_append_pipeline_get_failed (pipeline));
  fail_unless_equals_int (g_atomic_int_get (&context.init_segments), 1);
  fail_unless_equals_int (gst_append_pipeline_n_tracks (pipeline), 3);

  GstMediaSourceTrack *video = g_ptr_array_index (video_tracks, 0);
  GstMediaSourceTrack *audio = g_ptr_array_index (audio_tracks, 0);
  fail_unless (samples_for (&context, video) > 0, "no video samples");
  fail_unless (samples_for (&context, audio) > 0, "no audio samples");
  GPtrArray *text_tracks = gst_append_pipeline_get_text_tracks (pipeline);
  guint known_samples = samples_for (&context, video) +
      samples_for (&context, audio);
  for (guint i = 0; i < text_tracks->len; i++) {
    known_samples += samples_for (&context, g_ptr_array_index (text_tracks, i));
  }
  guint all_samples = 0;
  GHashTableIter iter;
  gpointer count;
  g_hash_table_iter_init (&iter, context.samples);
  while (g_hash_table_iter_next (&iter, NULL, &count)) {
    all_samples += GPOINTER_TO_UINT (count);
  }
  fail_unless_equals_int (all_samples, known_samples);

  g_ptr_array_unref (video_tracks);
  g_ptr_array_unref (audio_tracks);
  g_ptr_array_unref (text_tracks);
  tracks_test_pipeline_free (pipeline, &context);
}

GST_END_TEST;

GST_START_TEST (test_append_pipeline_init_segment_reported_once)
{
  /* Proves the initialization segment of bytes with no demuxer, here ADTS
   * AAC, is reported once. The report is reached from pad-added on the
   * streaming thread and from the stream-collection message on the bus
   * task, whichever finds the last track present; when both do, the second
   * must not report again. */
  TracksTestContext context = { 0 };
  GstAppendPipelineCallbacks callbacks = {
    .received_init_segment = count_init_segment,
    .eos = test_append_pipeline_eos,
    .error = test_append_pipeline_error,
  };
  GstAppendPipeline *pipeline =
      gst_append_pipeline_new (&callbacks, &context, NULL);

  fail_unless (gst_append_pipeline_append (pipeline,
          test_file_buffer ("mse-aac.adts")) == GST_FLOW_OK);
  gst_append_pipeline_eos (pipeline);
  test_append_pipeline_await_eos (pipeline, &context.base);
  fail_if (gst_append_pipeline_get_failed (pipeline));
  fail_unless_equals_int (g_atomic_int_get (&context.init_segments), 1);

  gst_object_unref (pipeline);
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

/* Tells, from any thread, whether a watched object has been disposed. */
typedef struct
{
  GMutex mutex;
  GCond cond;
  gboolean disposed;
} DisposeWatch;

static void
dispose_watch_notify (DisposeWatch * watch, GObject * where_the_object_was)
{
  g_mutex_lock (&watch->mutex);
  watch->disposed = TRUE;
  g_cond_broadcast (&watch->cond);
  g_mutex_unlock (&watch->mutex);
}

static void
dispose_watch_init (DisposeWatch * watch, gpointer object)
{
  g_mutex_init (&watch->mutex);
  g_cond_init (&watch->cond);
  watch->disposed = FALSE;
  g_object_weak_ref (G_OBJECT (object), (GWeakNotify) dispose_watch_notify,
      watch);
}

/* Waits until the object is disposed; FALSE if it is not within
 * EVENT_TIMEOUT. */
static gboolean
dispose_watch_await (DisposeWatch * watch)
{
  gint64 deadline = g_get_monotonic_time () + EVENT_TIMEOUT;
  g_mutex_lock (&watch->mutex);
  gboolean disposed = watch->disposed;
  while (!disposed && g_cond_wait_until (&watch->cond, &watch->mutex, deadline)) {
    disposed = watch->disposed;
  }
  g_mutex_unlock (&watch->mutex);
  return disposed;
}

static void
dispose_watch_clear (DisposeWatch * watch)
{
  g_mutex_clear (&watch->mutex);
  g_cond_clear (&watch->cond);
}

typedef struct
{
  GstMediaSource *media_source;
  /* The test's own reference, dropped inside the handler. */
  GstSourceBuffer *source_buffer;
  DisposeWatch watch;
} RemoveInHandlerContext;

static void
remove_source_buffer_in_handler (GstSourceBuffer * source_buffer,
    RemoveInHandlerContext * context)
{
  gst_media_source_remove_source_buffer (context->media_source,
      source_buffer, NULL);
  gst_clear_object (&context->source_buffer);
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
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
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
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
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
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
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
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
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
   * time its append ends: H.264 and AAC here, then the H.264, MPEG audio
   * and text of mse.mp4, whose MPEG audio parser has no caps before a
   * frame. The tracks come from the demuxer's pads, not from the parsers
   * behind them. */
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
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);

  source_buffer = mp4_source_buffer (&media_source);
  source_buffer_events_init (&events, source_buffer);

  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_before_box ("moof"), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);
  fail_unless_equals_int (events.error, 0);
  fail_unless_equals_int (n_active_source_buffers (media_source), 1);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_from_box ("moof"), &error));
  g_assert_no_error (error);
  await_update_end (&events, 2);
  fail_unless_equals_int (events.error, 0);
  fail_unless_equals_int (n_active_source_buffers (media_source), 1);

  source_buffer_events_clear (&events, source_buffer);
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
}

GST_END_TEST;


GST_START_TEST (test_source_buffer_removed_in_event_handler)
{
  /* Proves a source buffer can be removed, and lose its last reference,
   * inside the handler of one of its own events: the handler runs on the
   * buffer's event thread, which is then the thread freeing the buffer. */
  RemoveInHandlerContext context = { 0 };
  context.source_buffer = mp4_source_buffer (&context.media_source);
  dispose_watch_init (&context.watch, context.source_buffer);
  g_signal_connect (context.source_buffer, "on-update-end",
      G_CALLBACK (remove_source_buffer_in_handler), &context);

  /* An append of no bytes still ends with update-end. */
  GError *error = NULL;
  fail_unless (gst_source_buffer_append_buffer (context.source_buffer,
          gst_buffer_new (), &error));
  g_assert_no_error (error);

  fail_unless (dispose_watch_await (&context.watch),
      "the source buffer was not freed by its handler");

  GstSourceBufferList *buffers =
      gst_media_source_get_source_buffers (context.media_source);
  fail_unless_equals_int (gst_source_buffer_list_get_length (buffers), 0);
  gst_object_unref (buffers);

  gst_object_unref (context.media_source);
  dispose_watch_clear (&context.watch);
}

GST_END_TEST;

/* An MP4 source buffer with mse.mp4 appended, its tracks fed. */
static GstSourceBuffer *
mp4_source_buffer_with_media (GstMediaSource ** media_source)
{
  GstSourceBuffer *source_buffer = mp4_source_buffer (media_source);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  GError *error = NULL;
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);
  fail_unless_equals_int (events.error, 0);
  source_buffer_events_clear (&events, source_buffer);
  return source_buffer;
}

GST_START_TEST (test_source_buffer_freed_when_removed)
{
  /* Proves a source buffer holding media is freed once the application
   * has removed it and let go of it, and likewise when the application
   * lets go of the media source, instead of living on with its track
   * feeds polling for data. */
  GstMediaSource *media_source;
  GstSourceBuffer *source_buffer = mp4_source_buffer_with_media (&media_source);
  DisposeWatch watch;
  dispose_watch_init (&watch, source_buffer);
  GError *error = NULL;

  fail_unless (gst_media_source_remove_source_buffer (media_source,
          source_buffer, &error));
  g_assert_no_error (error);
  gst_object_unref (source_buffer);
  fail_unless (dispose_watch_await (&watch),
      "the removed source buffer was not freed");
  dispose_watch_clear (&watch);
  gst_object_unref (media_source);

  source_buffer = mp4_source_buffer_with_media (&media_source);
  dispose_watch_init (&watch, source_buffer);

  gst_object_unref (media_source);
  gst_object_unref (source_buffer);
  fail_unless (dispose_watch_await (&watch),
      "the source buffer of a dropped media source was not freed");
  dispose_watch_clear (&watch);
}

GST_END_TEST;

GST_START_TEST (test_source_buffer_freed_at_end_of_stream)
{
  /* Proves the application can let go of a source buffer and its media
   * source right after ending the stream, while the track feeds are still
   * pushing the last samples and EOS. Repeated, since it matters which
   * thread is the last to let go. */
  for (guint i = 0; i < 5; i++) {
    GstMediaSource *media_source;
    GstSourceBuffer *source_buffer =
        mp4_source_buffer_with_media (&media_source);
    DisposeWatch watch;
    dispose_watch_init (&watch, source_buffer);
    GError *error = NULL;

    fail_unless (gst_media_source_end_of_stream (media_source,
            GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
    g_assert_no_error (error);
    gst_object_unref (media_source);
    gst_object_unref (source_buffer);
    fail_unless (dispose_watch_await (&watch),
        "the source buffer was not freed after the end of the stream");
    dispose_watch_clear (&watch);
  }
}

GST_END_TEST;

/* The end of the last buffered range of a source buffer. */
static GstClockTime
buffered_end (GstSourceBuffer * source_buffer)
{
  GError *error = NULL;
  GArray *buffered = gst_source_buffer_get_buffered (source_buffer, &error);
  g_assert_no_error (error);
  fail_unless (buffered->len > 0);
  GstClockTime end = g_array_index (buffered, GstMediaSourceRange,
      buffered->len - 1).end;
  g_array_unref (buffered);
  return end;
}

/* A pipeline whose running time stands at `running_time` on a test clock
 * from the start, for msesrc, which holds each EOS until running time comes
 * near the end of its track; it moves only as the test sets the clock. */
static GstElement *
new_pipeline_at (GstClockTime running_time, GstTestClock ** test_clock)
{
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstClock *clock = gst_test_clock_new_with_start_time (running_time);
  gst_pipeline_use_clock (GST_PIPELINE (pipeline), clock);
  gst_element_set_start_time (pipeline, GST_CLOCK_TIME_NONE);
  gst_element_set_base_time (pipeline, 0);
  if (test_clock) {
    *test_clock = GST_TEST_CLOCK (clock);
  } else {
    gst_object_unref (clock);
  }
  return pipeline;
}

/* Whether the media on a pad has caps whose name starts with `prefix`. */
static gboolean
pad_media_is (GstPad * pad, const gchar * prefix)
{
  GstCaps *caps = gst_pad_get_current_caps (pad);
  gboolean result = caps && !gst_caps_is_empty (caps)
      && g_str_has_prefix (gst_structure_get_name (gst_caps_get_structure
          (caps, 0)), prefix);
  gst_clear_caps (&caps);
  return result;
}

/* Marks a buffer that msesrc pushed before the source reopened. */
#define OLD_BUFFER_QUARK (g_quark_from_static_string ("mse-test-old-buffer"))

/* What reached the sinks behind msesrc's pads: how many marked buffers, how
 * many EOS events, and how many buffers since the last one, from which time.
 * With old_end set above zero, also how many buffers from before it came
 * after an EOS, and whether one from after it has come. */
typedef struct
{
  GMutex mutex;
  GCond cond;
  GstElement *pipeline;
  guint n_sinks;
  guint marked_old[8];
  guint eos[8];
  guint buffers_after_eos[8];
  GstClockTime first_pts_after_eos[8];
  GstClockTime old_end;
  guint old_after_eos[8];
  gboolean new_seen[8];
  GstClockTime end[8];
  GstClockTime end_duration[8];
  gboolean video[8];
} SinkFlow;

static GstPadProbeReturn
count_sink_flow (GstPad * pad, GstPadProbeInfo * info, SinkFlow * flow)
{
  guint i = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (pad), "index"));
  g_mutex_lock (&flow->mutex);
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER) {
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);
    GstClockTime pts = GST_BUFFER_PTS (buffer);
    flow->marked_old[i] += gst_mini_object_get_qdata (GST_MINI_OBJECT (buffer),
        OLD_BUFFER_QUARK) != NULL;
    GstClockTime end = pts + GST_BUFFER_DURATION (buffer);
    if (flow->end[i] == 0 || end > flow->end[i]) {
      flow->end[i] = end;
      flow->end_duration[i] = GST_BUFFER_DURATION (buffer);
    }
    flow->video[i] = pad_media_is (pad, "video/");
    if (flow->buffers_after_eos[i]++ == 0) {
      flow->first_pts_after_eos[i] = pts;
    }
    if (flow->old_end > 0) {
      if (pts >= flow->old_end) {
        flow->new_seen[i] = TRUE;
      } else if (flow->eos[i] > 0) {
        flow->old_after_eos[i]++;
      }
    }
  } else if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) == GST_EVENT_EOS) {
    flow->eos[i]++;
    flow->buffers_after_eos[i] = 0;
  }
  g_cond_broadcast (&flow->cond);
  g_mutex_unlock (&flow->mutex);
  return GST_PAD_PROBE_OK;
}

static void
link_fakesink (GstElement * src, GstPad * pad, SinkFlow * flow)
{
  GstElement *sink = gst_element_factory_make ("fakesink", NULL);
  g_object_set (sink, "sync", FALSE, NULL);
  gst_bin_add (GST_BIN (flow->pipeline), sink);
  GstPad *sink_pad = gst_element_get_static_pad (sink, "sink");
  g_mutex_lock (&flow->mutex);
  fail_unless (flow->n_sinks < G_N_ELEMENTS (flow->eos));
  g_object_set_data (G_OBJECT (sink_pad), "index",
      GUINT_TO_POINTER (flow->n_sinks++));
  g_mutex_unlock (&flow->mutex);
  gst_pad_add_probe (sink_pad, GST_PAD_PROBE_TYPE_BUFFER |
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      (GstPadProbeCallback) count_sink_flow, flow, NULL);
  fail_unless (gst_element_sync_state_with_parent (sink));
  fail_unless_equals_int (gst_pad_link (pad, sink_pad), GST_PAD_LINK_OK);
  gst_object_unref (sink_pad);
}

/* Waits until every sink has had `eos` EOS events and, if `buffers`, a
 * buffer since the last. */
static void
await_sink_flow (SinkFlow * flow, guint eos, gboolean buffers)
{
  gint64 deadline = g_get_monotonic_time () + EVENT_TIMEOUT;
  g_mutex_lock (&flow->mutex);
  while (TRUE) {
    gboolean done = flow->n_sinks > 0;
    for (guint i = 0; i < flow->n_sinks; i++) {
      done &= flow->eos[i] == eos && (!buffers
          || flow->buffers_after_eos[i] > 0);
    }
    if (done) {
      break;
    }
    if (!g_cond_wait_until (&flow->cond, &flow->mutex, deadline)) {
      guint n = flow->n_sinks;
      guint first_eos = n ? flow->eos[0] : 0;
      guint first_buffers = n ? flow->buffers_after_eos[0] : 0;
      g_mutex_unlock (&flow->mutex);
      ck_abort_msg ("timed out waiting for %u EOS%s on %u sinks; sink 0 had "
          "%u EOS and %u buffers after", eos,
          buffers ? " and media after" : "", n, first_eos, first_buffers);
    }
  }
  g_mutex_unlock (&flow->mutex);
}

GST_START_TEST (test_media_source_element_plays_after_reopen)
{
  /* Proves msesrc's pads push media appended after the end of stream: the
   * append reopens the source (MSE, prepare append step 5), and its pads,
   * which have pushed EOS, push the new media, not a replay of the old, and
   * then EOS again at the next end of stream; also after the source has
   * reopened and ended again with no media in between. */
  SinkFlow flow = { 0 };
  g_mutex_init (&flow.mutex);
  g_cond_init (&flow.cond);
  flow.pipeline = new_pipeline_past_end ();
  GstElement *src = g_object_new (GST_TYPE_MSE_SRC, NULL);
  gst_bin_add (GST_BIN (flow.pipeline), src);
  g_signal_connect (src, "pad-added", G_CALLBACK (link_fakesink), &flow);
  GstMediaSource *media_source = gst_media_source_new ();
  gst_media_source_attach (media_source, GST_MSE_SRC (src));
  fail_if (gst_element_set_state (flow.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE);

  GError *error = NULL;
  GstSourceBuffer *source_buffer =
      gst_media_source_add_source_buffer (media_source, "video/mp4", &error);
  g_assert_no_error (error);
  /* Sequence mode: mselib applies the timestamp offset only there. */
  fail_unless (gst_source_buffer_set_append_mode (source_buffer,
          GST_SOURCE_BUFFER_APPEND_MODE_SEQUENCE, &error));
  g_assert_no_error (error);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);
  GstClockTime end = buffered_end (source_buffer);

  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);
  await_sink_flow (&flow, 1, FALSE);

  /* Reopened by a setter and ended again with no media in between: the
   * pads' EOS still holds, and they must stay able to push. The offset puts
   * the same file again after what is buffered; its fragments give absolute
   * data offsets, so the parser starts over for it, which needs the source
   * open. */
  fail_unless (gst_source_buffer_set_timestamp_offset (source_buffer, end,
          &error));
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_abort (source_buffer, &error));
  g_assert_no_error (error);
  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);

  /* Reopened by the append itself. */
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 2);
  fail_unless_equals_int (events.error, 0);
  await_sink_flow (&flow, 1, TRUE);
  for (guint i = 0; i < flow.n_sinks; i++) {
    fail_unless (flow.first_pts_after_eos[i] >= end,
        "sink %u: replayed media at %" GST_TIMEP_FORMAT " after the end of "
        "stream at %" GST_TIMEP_FORMAT, i, &flow.first_pts_after_eos[i], &end);
  }

  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);
  await_sink_flow (&flow, 2, FALSE);

  gst_element_set_state (flow.pipeline, GST_STATE_NULL);
  source_buffer_events_clear (&events, source_buffer);
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
  gst_object_unref (flow.pipeline);
  g_mutex_clear (&flow.mutex);
  g_cond_clear (&flow.cond);
}

GST_END_TEST;

/* Waits until every sink has had media from after flow->old_end and, after
 * its last buffer, an EOS. */
static void
await_sink_ended_after_new (SinkFlow * flow)
{
  gint64 deadline = g_get_monotonic_time () + EVENT_TIMEOUT;
  g_mutex_lock (&flow->mutex);
  while (TRUE) {
    gboolean done = flow->n_sinks > 0;
    for (guint i = 0; i < flow->n_sinks; i++) {
      done &= flow->new_seen[i] && flow->eos[i] > 0
          && flow->buffers_after_eos[i] == 0;
    }
    if (done) {
      break;
    }
    if (!g_cond_wait_until (&flow->cond, &flow->mutex, deadline)) {
      gboolean new_seen = flow->n_sinks ? flow->new_seen[0] : FALSE;
      guint eos = flow->n_sinks ? flow->eos[0] : 0;
      g_mutex_unlock (&flow->mutex);
      ck_abort_msg ("timed out waiting for the new media and an EOS; sink 0 "
          "had new media %d, %u EOS", new_seen, eos);
    }
  }
  g_mutex_unlock (&flow->mutex);
}

GST_START_TEST (test_media_source_element_ends_after_append_during_feed)
{
  /* Proves a reopen and an append that land while the track feeds are still
   * pushing the media before the end of stream do not get an EOS pushed in
   * the middle of it: the old media all comes before any EOS, and the EOS
   * comes after the new media. */
  SinkFlow flow = { 0 };
  g_mutex_init (&flow.mutex);
  g_cond_init (&flow.cond);
  flow.pipeline = new_pipeline_past_end ();
  GstElement *src = g_object_new (GST_TYPE_MSE_SRC, NULL);
  gst_bin_add (GST_BIN (flow.pipeline), src);
  g_signal_connect (src, "pad-added", G_CALLBACK (link_fakesink), &flow);
  GstMediaSource *media_source = gst_media_source_new ();
  gst_media_source_attach (media_source, GST_MSE_SRC (src));
  fail_if (gst_element_set_state (flow.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE);

  GError *error = NULL;
  GstSourceBuffer *source_buffer =
      gst_media_source_add_source_buffer (media_source, "video/mp4", &error);
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_set_append_mode (source_buffer,
          GST_SOURCE_BUFFER_APPEND_MODE_SEQUENCE, &error));
  g_assert_no_error (error);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);
  GstClockTime end = buffered_end (source_buffer);
  g_mutex_lock (&flow.mutex);
  flow.old_end = end;
  g_mutex_unlock (&flow.mutex);

  /* No wait for the EOS at the sinks: the feeds may still be pushing. */
  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_set_timestamp_offset (source_buffer, end,
          &error));
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_abort (source_buffer, &error));
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 2);
  fail_unless_equals_int (events.error, 0);
  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);

  await_sink_ended_after_new (&flow);
  for (guint i = 0; i < flow.n_sinks; i++) {
    fail_unless_equals_int (flow.old_after_eos[i], 0);
  }

  gst_element_set_state (flow.pipeline, GST_STATE_NULL);
  source_buffer_events_clear (&events, source_buffer);
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
  gst_object_unref (flow.pipeline);
  g_mutex_clear (&flow.mutex);
  g_cond_clear (&flow.cond);
}

GST_END_TEST;

/* msesrc's pads, each into a multiqueue held shut: how many buffers each
 * pushed before the source reopened, marked, and after it, how far its
 * media reached and how many EOS it pushed. */
typedef struct
{
  SinkFlow sinks;
  guint n_pads;
  GstPad *src_pads[8];
  GstPad *held_pads[8];
  gulong hold_ids[8];
  gboolean reopened;
  guint pushed_old[8];
  guint pushed_new[8];
  GstClockTime pushed_end[8];
  guint pushed_eos[8];
} HeldFlow;

static GstPadProbeReturn
count_pushed (GstPad * pad, GstPadProbeInfo * info, HeldFlow * flow)
{
  guint i = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (pad), "index"));
  g_mutex_lock (&flow->sinks.mutex);
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER) {
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);
    GstClockTime end = GST_BUFFER_PTS (buffer) + GST_BUFFER_DURATION (buffer);
    if (!GST_CLOCK_TIME_IS_VALID (flow->pushed_end[i])
        || end > flow->pushed_end[i]) {
      flow->pushed_end[i] = end;
    }
    if (!flow->reopened) {
      gst_mini_object_set_qdata (GST_MINI_OBJECT (buffer), OLD_BUFFER_QUARK,
          GUINT_TO_POINTER (TRUE), NULL);
      flow->pushed_old[i]++;
    } else {
      flow->pushed_new[i]++;
    }
  } else if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) == GST_EVENT_EOS) {
    flow->pushed_eos[i]++;
  }
  g_cond_broadcast (&flow->sinks.cond);
  g_mutex_unlock (&flow->sinks.mutex);
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
hold (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  return GST_PAD_PROBE_OK;
}

/* A multiqueue with no limits between the pad and its sink, its output held
 * until the test lets it go, so that what msesrc pushed is still queued
 * downstream. A multiqueue goes on after an EOS with what comes behind it,
 * as playback's queues must. */
static void
link_held_multiqueue (GstElement * src, GstPad * pad, HeldFlow * flow)
{
  GstElement *multiqueue = gst_element_factory_make ("multiqueue", NULL);
  g_object_set (multiqueue, "max-size-buffers", 0, "max-size-bytes", 0,
      "max-size-time", (guint64) 0, NULL);
  gst_bin_add (GST_BIN (flow->sinks.pipeline), multiqueue);
  GstPad *queue_sink = gst_element_request_pad_simple (multiqueue, "sink_%u");
  GstPad *queue_src = NULL;
  GstIterator *it = gst_pad_iterate_internal_links (queue_sink);
  GValue item = G_VALUE_INIT;
  fail_unless_equals_int (gst_iterator_next (it, &item), GST_ITERATOR_OK);
  queue_src = g_value_dup_object (&item);
  g_value_unset (&item);
  gst_iterator_free (it);

  g_mutex_lock (&flow->sinks.mutex);
  fail_unless (flow->n_pads < G_N_ELEMENTS (flow->hold_ids));
  guint i = flow->n_pads++;
  g_object_set_data (G_OBJECT (pad), "index", GUINT_TO_POINTER (i));
  flow->pushed_end[i] = GST_CLOCK_TIME_NONE;
  flow->src_pads[i] = pad;
  flow->held_pads[i] = queue_src;
  flow->hold_ids[i] = gst_pad_add_probe (queue_src,
      GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, hold, NULL, NULL);
  g_mutex_unlock (&flow->sinks.mutex);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER |
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      (GstPadProbeCallback) count_pushed, flow, NULL);
  fail_unless (gst_element_sync_state_with_parent (multiqueue));
  link_fakesink (multiqueue, queue_src, &flow->sinks);
  fail_unless_equals_int (gst_pad_link (pad, queue_sink), GST_PAD_LINK_OK);
  gst_object_unref (queue_sink);
}

/* Whether a pad of msesrc carries audio or video, whose media goes up to
 * the buffered end; a text track may end before it. */
static gboolean
is_audio_or_video (GstPad * pad)
{
  return pad_media_is (pad, "audio/") || pad_media_is (pad, "video/");
}

/* Waits until every msesrc pad has pushed an EOS or its media, up to the
 * old end for audio and video, and if `after`, a buffer after the
 * reopen. */
static void
await_pushed (HeldFlow * flow, gboolean after)
{
  gint64 deadline = g_get_monotonic_time () + EVENT_TIMEOUT;
  g_mutex_lock (&flow->sinks.mutex);
  while (TRUE) {
    gboolean done = flow->n_pads > 0;
    for (guint i = 0; i < flow->n_pads; i++) {
      gboolean pushed = GST_CLOCK_TIME_IS_VALID (flow->pushed_end[i])
          && (flow->pushed_end[i] >= flow->sinks.old_end
          || !is_audio_or_video (flow->src_pads[i]));
      done &= (flow->pushed_eos[i] > 0 || pushed)
          && (!after || flow->pushed_new[i] > 0);
    }
    if (done) {
      break;
    }
    if (!g_cond_wait_until (&flow->sinks.cond, &flow->sinks.mutex, deadline)) {
      g_mutex_unlock (&flow->sinks.mutex);
      ck_abort_msg ("timed out waiting for msesrc to push its media%s",
          after ? " and media after the reopen" : "");
    }
  }
  g_mutex_unlock (&flow->sinks.mutex);
}

GST_START_TEST (test_media_source_element_keeps_queued_media_on_reopen)
{
  /* Proves media appended after the end of stream does not throw away the
   * media msesrc pushed before it that is still queued downstream: playback
   * may be far from the end when the page appends more (MSE, prepare append
   * step 5 reopens the source; nothing drops what is buffered). Playback
   * stays at the start on a test clock, so msesrc must push no EOS before
   * the reopen. Every sink gets all of the old media, then the new media,
   * then the one EOS. */
  HeldFlow flow = { 0 };
  g_mutex_init (&flow.sinks.mutex);
  g_cond_init (&flow.sinks.cond);
  GstTestClock *clock = NULL;
  flow.sinks.pipeline = new_pipeline_at (0, &clock);
  GstElement *src = g_object_new (GST_TYPE_MSE_SRC, NULL);
  gst_bin_add (GST_BIN (flow.sinks.pipeline), src);
  g_signal_connect (src, "pad-added", G_CALLBACK (link_held_multiqueue), &flow);
  GstMediaSource *media_source = gst_media_source_new ();
  gst_media_source_attach (media_source, GST_MSE_SRC (src));
  fail_if (gst_element_set_state (flow.sinks.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE);

  GError *error = NULL;
  GstSourceBuffer *source_buffer =
      gst_media_source_add_source_buffer (media_source, "video/mp4", &error);
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_set_append_mode (source_buffer,
          GST_SOURCE_BUFFER_APPEND_MODE_SEQUENCE, &error));
  g_assert_no_error (error);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);
  GstClockTime end = buffered_end (source_buffer);
  g_mutex_lock (&flow.sinks.mutex);
  flow.sinks.old_end = end;
  g_mutex_unlock (&flow.sinks.mutex);
  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);
  await_pushed (&flow, FALSE);

  g_mutex_lock (&flow.sinks.mutex);
  flow.reopened = TRUE;
  g_mutex_unlock (&flow.sinks.mutex);
  fail_unless (gst_source_buffer_set_timestamp_offset (source_buffer, end,
          &error));
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_abort (source_buffer, &error));
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 2);
  fail_unless_equals_int (events.error, 0);

  /* Every pad has pushed new media after the reopen while the old media is
   * still held downstream. */
  await_pushed (&flow, TRUE);
  g_mutex_lock (&flow.sinks.mutex);
  guint n_pads = flow.n_pads;
  g_mutex_unlock (&flow.sinks.mutex);
  for (guint i = 0; i < n_pads; i++) {
    gst_pad_remove_probe (flow.held_pads[i], flow.hold_ids[i]);
  }

  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);
  /* Playback reaches the new end. A source that pushes its EOS at once has
   * no waits; test_media_source_element_holds_eos_until_end checks them. */
  GList *pending = NULL;
  gst_test_clock_timed_wait_for_multiple_pending_ids (clock, n_pads, 5000,
      &pending);
  gst_test_clock_set_time (clock, 2 * end);
  gst_test_clock_process_id_list (clock, pending);
  g_list_free_full (pending, (GDestroyNotify) gst_clock_id_unref);
  await_sink_ended_after_new (&flow.sinks);
  /* The sinks were linked in the order of the pads. */
  for (guint i = 0; i < n_pads; i++) {
    fail_unless (flow.pushed_old[i] > 0);
    fail_unless_equals_int (flow.sinks.marked_old[i], flow.pushed_old[i]);
    fail_unless_equals_int (flow.sinks.old_after_eos[i], 0);
    fail_unless_equals_int (flow.pushed_eos[i], 1);
  }

  gst_element_set_state (flow.sinks.pipeline, GST_STATE_NULL);
  for (guint i = 0; i < n_pads; i++) {
    gst_object_unref (flow.held_pads[i]);
  }
  source_buffer_events_clear (&events, source_buffer);
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
  gst_object_unref (clock);
  gst_object_unref (flow.sinks.pipeline);
  g_mutex_clear (&flow.sinks.mutex);
  g_cond_clear (&flow.sinks.cond);
}

GST_END_TEST;

/* Waits until every sink has had media from after flow->old_end. */
static void
await_sink_new (SinkFlow * flow)
{
  gint64 deadline = g_get_monotonic_time () + EVENT_TIMEOUT;
  g_mutex_lock (&flow->mutex);
  while (TRUE) {
    gboolean done = flow->n_sinks > 0;
    for (guint i = 0; i < flow->n_sinks; i++) {
      done &= flow->new_seen[i];
    }
    if (done) {
      break;
    }
    if (!g_cond_wait_until (&flow->cond, &flow->mutex, deadline)) {
      g_mutex_unlock (&flow->mutex);
      ck_abort_msg ("timed out waiting for the new media");
    }
  }
  g_mutex_unlock (&flow->mutex);
}

GST_START_TEST (test_media_source_element_holds_eos_until_end)
{
  /* Proves msesrc pushes a track's EOS only when running time comes near the
   * end of its media, and that media appended before then follows the old
   * media with no EOS between them: playback that has not reached the end
   * of stream goes on into what the page appends (MSE, prepare append
   * step 5), and must never see the old end. */
  SinkFlow flow = { 0 };
  g_mutex_init (&flow.mutex);
  g_cond_init (&flow.cond);
  GstTestClock *clock = NULL;
  flow.pipeline = new_pipeline_at (0, &clock);
  GstElement *src = g_object_new (GST_TYPE_MSE_SRC, NULL);
  gst_bin_add (GST_BIN (flow.pipeline), src);
  g_signal_connect (src, "pad-added", G_CALLBACK (link_fakesink), &flow);
  GstMediaSource *media_source = gst_media_source_new ();
  gst_media_source_attach (media_source, GST_MSE_SRC (src));
  fail_if (gst_element_set_state (flow.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE);

  GError *error = NULL;
  GstSourceBuffer *source_buffer =
      gst_media_source_add_source_buffer (media_source, "video/mp4", &error);
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_set_append_mode (source_buffer,
          GST_SOURCE_BUFFER_APPEND_MODE_SEQUENCE, &error));
  g_assert_no_error (error);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);
  GstClockTime end = buffered_end (source_buffer);
  g_mutex_lock (&flow.mutex);
  flow.old_end = end;
  guint n_sinks = flow.n_sinks;
  g_mutex_unlock (&flow.mutex);
  fail_unless (n_sinks > 0);
  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);

  /* Each pad waits for running time to come near the end of its media; a
   * text track can end before the others. */
  GList *pending = NULL;
  fail_unless (gst_test_clock_timed_wait_for_multiple_pending_ids (clock,
          n_sinks, 5000, &pending));
  GstClockID id = NULL;
  fail_unless (gst_test_clock_peek_next_pending_id (clock, &id));
  GstClockTime first_wait = gst_clock_id_get_time (id);
  gst_clock_id_unref (id);
  fail_unless (first_wait >= GST_SECOND);
  /* A video pad waits until 4 frames before the end of its media, so that
   * the decoder drains its last frames in time; any other pad until the end
   * itself. Each pad has pushed all of its media to its sink by now. */
  g_mutex_lock (&flow.mutex);
  for (guint i = 0; i < n_sinks; i++) {
    GstClockTime expected = flow.video[i] ?
        flow.end[i] - 4 * flow.end_duration[i] : flow.end[i];
    gboolean found = FALSE;
    for (GList * l = pending; l; l = l->next) {
      found |= gst_clock_id_get_time (l->data) == expected;
    }
    fail_unless (found, "no clock wait at %" GST_TIME_FORMAT " for the %s "
        "ending at %" GST_TIME_FORMAT, GST_TIME_ARGS (expected),
        flow.video[i] ? "video" : "other track", GST_TIME_ARGS (flow.end[i]));
  }
  g_mutex_unlock (&flow.mutex);
  g_list_free_full (pending, (GDestroyNotify) gst_clock_id_unref);
  g_mutex_lock (&flow.mutex);
  for (guint i = 0; i < n_sinks; i++) {
    fail_unless_equals_int (flow.eos[i], 0);
  }
  g_mutex_unlock (&flow.mutex);

  /* The page appends more before playback gets to the end. */
  fail_unless (gst_source_buffer_set_timestamp_offset (source_buffer, end,
          &error));
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_abort (source_buffer, &error));
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 2);
  fail_unless_equals_int (events.error, 0);
  await_sink_new (&flow);
  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);
  fail_unless (gst_test_clock_timed_wait_for_multiple_pending_ids (clock,
          n_sinks, 5000, &pending));
  g_mutex_lock (&flow.mutex);
  for (guint i = 0; i < n_sinks; i++) {
    fail_unless_equals_int (flow.eos[i], 0);
  }
  g_mutex_unlock (&flow.mutex);

  /* Playback reaches the new end. */
  gst_test_clock_set_time (clock, 2 * end);
  fail_unless (gst_test_clock_process_id_list (clock, pending) == n_sinks);
  g_list_free_full (pending, (GDestroyNotify) gst_clock_id_unref);
  await_sink_ended_after_new (&flow);
  for (guint i = 0; i < n_sinks; i++) {
    fail_unless_equals_int (flow.eos[i], 1);
    fail_unless_equals_int (flow.old_after_eos[i], 0);
  }

  gst_element_set_state (flow.pipeline, GST_STATE_NULL);
  source_buffer_events_clear (&events, source_buffer);
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
  gst_object_unref (clock);
  gst_object_unref (flow.pipeline);
  g_mutex_clear (&flow.mutex);
  g_cond_clear (&flow.cond);
}

GST_END_TEST;

/* Whether a flush-stop that keeps the running time, such as one of msesrc
 * resuming after its EOS, lands inside a seek's flush on a pad, once
 * `armed`: a flush-stop like that after two flush-starts. It would undo the
 * seek's flush-start and let media from before the seek through. */
typedef struct
{
  GMutex mutex;
  gboolean armed;
  guint n_pads;
  guint flush_starts[8];
  guint undone;
} SeekFlush;

static GstPadProbeReturn
watch_seek_flush (GstPad * pad, GstPadProbeInfo * info, SeekFlush * seek)
{
  guint i = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (pad),
          "seek-index"));
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);
  gboolean reset_time = FALSE;
  g_mutex_lock (&seek->mutex);
  if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_START && seek->armed) {
    seek->flush_starts[i]++;
  } else if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_STOP) {
    gst_event_parse_flush_stop (event, &reset_time);
    seek->undone += !reset_time && seek->flush_starts[i] > 1;
    seek->flush_starts[i] = 0;
  }
  g_mutex_unlock (&seek->mutex);
  return GST_PAD_PROBE_OK;
}

static void
probe_seek_flush (GstElement * src, GstPad * pad, SeekFlush * seek)
{
  g_mutex_lock (&seek->mutex);
  fail_unless (seek->n_pads < G_N_ELEMENTS (seek->flush_starts));
  g_object_set_data (G_OBJECT (pad), "seek-index",
      GUINT_TO_POINTER (seek->n_pads++));
  g_mutex_unlock (&seek->mutex);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_FLUSH,
      (GstPadProbeCallback) watch_seek_flush, seek, NULL);
}

GST_START_TEST (test_media_source_element_seeks_while_resuming)
{
  /* Proves a seek made while msesrc resumes its pads after a reopen is not
   * undone by the resume: no flush-stop that keeps the running time comes
   * inside the seek's flush. */
  SeekFlush seek = { 0 };
  g_mutex_init (&seek.mutex);
  SinkFlow flow = { 0 };
  g_mutex_init (&flow.mutex);
  g_cond_init (&flow.cond);
  flow.pipeline = new_pipeline_past_end ();
  GstElement *src = g_object_new (GST_TYPE_MSE_SRC, NULL);
  gst_bin_add (GST_BIN (flow.pipeline), src);
  g_signal_connect (src, "pad-added", G_CALLBACK (probe_seek_flush), &seek);
  g_signal_connect (src, "pad-added", G_CALLBACK (link_fakesink), &flow);
  GstMediaSource *media_source = gst_media_source_new ();
  gst_media_source_attach (media_source, GST_MSE_SRC (src));
  fail_if (gst_element_set_state (flow.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE);

  GError *error = NULL;
  GstSourceBuffer *source_buffer =
      gst_media_source_add_source_buffer (media_source, "video/mp4", &error);
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_set_append_mode (source_buffer,
          GST_SOURCE_BUFFER_APPEND_MODE_SEQUENCE, &error));
  g_assert_no_error (error);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);
  GstClockTime end = buffered_end (source_buffer);
  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);
  await_sink_flow (&flow, 1, FALSE);

  fail_unless (gst_source_buffer_set_timestamp_offset (source_buffer, end,
          &error));
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_abort (source_buffer, &error));
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 2);
  fail_unless_equals_int (events.error, 0);

  g_mutex_lock (&seek.mutex);
  seek.armed = TRUE;
  g_mutex_unlock (&seek.mutex);
  fail_unless (gst_element_send_event (src, gst_event_new_seek (1.0,
              GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, 0,
              GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE)));
  g_mutex_lock (&seek.mutex);
  guint undone = seek.undone;
  g_mutex_unlock (&seek.mutex);
  fail_unless_equals_int (undone, 0);

  gst_element_set_state (flow.pipeline, GST_STATE_NULL);
  source_buffer_events_clear (&events, source_buffer);
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
  gst_object_unref (flow.pipeline);
  g_mutex_clear (&flow.mutex);
  g_cond_clear (&flow.cond);
  g_mutex_clear (&seek.mutex);
}

GST_END_TEST;

typedef struct
{
  GMutex mutex;
  GCond cond;
  gboolean ended;
} SourceEnded;

static void
note_source_ended (GstMediaSource * media_source, SourceEnded * ended)
{
  g_mutex_lock (&ended->mutex);
  ended->ended = TRUE;
  g_cond_broadcast (&ended->cond);
  g_mutex_unlock (&ended->mutex);
}

static gpointer
end_stream_on_thread (GstMediaSource * media_source)
{
  GError *error = NULL;
  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);
  return NULL;
}

GST_START_TEST (test_media_source_end_of_stream_races_reopen)
{
  /* Proves an end of stream on one thread and a reopen on another leave the
   * source open with tracks that are not ended, whichever finishes first:
   * media appended next is followed by an EOS at the next end of stream. */
  SinkFlow flow = { 0 };
  g_mutex_init (&flow.mutex);
  g_cond_init (&flow.cond);
  flow.pipeline = new_pipeline_past_end ();
  GstElement *src = g_object_new (GST_TYPE_MSE_SRC, NULL);
  gst_bin_add (GST_BIN (flow.pipeline), src);
  g_signal_connect (src, "pad-added", G_CALLBACK (link_fakesink), &flow);
  GstMediaSource *media_source = gst_media_source_new ();
  gst_media_source_attach (media_source, GST_MSE_SRC (src));
  fail_if (gst_element_set_state (flow.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE);
  SourceEnded ended = { 0 };
  g_mutex_init (&ended.mutex);
  g_cond_init (&ended.cond);
  g_signal_connect (media_source, "on-source-ended",
      G_CALLBACK (note_source_ended), &ended);

  GError *error = NULL;
  GstSourceBuffer *source_buffer =
      gst_media_source_add_source_buffer (media_source, "video/mp4", &error);
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_set_append_mode (source_buffer,
          GST_SOURCE_BUFFER_APPEND_MODE_SEQUENCE, &error));
  g_assert_no_error (error);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);
  GstClockTime end = buffered_end (source_buffer);
  g_mutex_lock (&flow.mutex);
  flow.old_end = end;
  g_mutex_unlock (&flow.mutex);

  /* The reopen comes as soon as the source is ended, which can be before
   * the end of stream has marked the buffers. */
  GThread *thread = g_thread_new ("end-of-stream",
      (GThreadFunc) end_stream_on_thread, media_source);
  gint64 deadline = g_get_monotonic_time () + EVENT_TIMEOUT;
  g_mutex_lock (&ended.mutex);
  while (!ended.ended) {
    fail_unless (g_cond_wait_until (&ended.cond, &ended.mutex, deadline));
  }
  g_mutex_unlock (&ended.mutex);
  fail_unless (gst_source_buffer_set_timestamp_offset (source_buffer, end,
          &error));
  g_assert_no_error (error);
  g_thread_join (thread);
  fail_unless_equals_int (gst_media_source_get_ready_state (media_source),
      GST_MEDIA_SOURCE_READY_STATE_OPEN);

  fail_unless (gst_source_buffer_abort (source_buffer, &error));
  g_assert_no_error (error);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 2);
  fail_unless_equals_int (events.error, 0);
  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);
  await_sink_ended_after_new (&flow);

  gst_element_set_state (flow.pipeline, GST_STATE_NULL);
  g_signal_handlers_disconnect_by_data (media_source, &ended);
  g_mutex_clear (&ended.mutex);
  g_cond_clear (&ended.cond);
  source_buffer_events_clear (&events, source_buffer);
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
  gst_object_unref (flow.pipeline);
  g_mutex_clear (&flow.mutex);
  g_cond_clear (&flow.cond);
}

GST_END_TEST;

/* The memory of the buffers msesrc pushed, and how much of it is freed. */
typedef struct
{
  GMutex mutex;
  GCond cond;
  GHashTable *live;
  guint pushed;
  guint freed;
} PushedMemory;

static void
pushed_memory_freed (PushedMemory * pushed, GstMiniObject * where)
{
  g_mutex_lock (&pushed->mutex);
  g_hash_table_remove (pushed->live, where);
  pushed->freed++;
  g_cond_broadcast (&pushed->cond);
  g_mutex_unlock (&pushed->mutex);
}

static GstPadProbeReturn
watch_pushed_memory (GstPad * pad, GstPadProbeInfo * info,
    PushedMemory * pushed)
{
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);
  g_mutex_lock (&pushed->mutex);
  for (guint i = 0; i < gst_buffer_n_memory (buffer); i++) {
    GstMemory *memory = gst_buffer_peek_memory (buffer, i);
    if (g_hash_table_add (pushed->live, memory)) {
      gst_mini_object_weak_ref (GST_MINI_OBJECT (memory),
          (GstMiniObjectNotify) pushed_memory_freed, pushed);
      pushed->pushed++;
    }
  }
  g_mutex_unlock (&pushed->mutex);
  return GST_PAD_PROBE_OK;
}

/* The end of the range test_track_feed_frees_removed_media removes. mse.mp4's
 * second video key frame is at 4.2 s, so the range ends in the first group
 * and leaves the last one buffered. */
#define REMOVED_END (2 * GST_SECOND)

static GstPadProbeReturn
watch_removed_memory (GstPad * pad, GstPadProbeInfo * info,
    PushedMemory * pushed)
{
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);
  if (!GST_BUFFER_PTS_IS_VALID (buffer)
      || GST_BUFFER_PTS (buffer) >= REMOVED_END) {
    return GST_PAD_PROBE_OK;
  }
  return watch_pushed_memory (pad, info, pushed);
}

static void
probe_removed_memory (GstElement * src, GstPad * pad, PushedMemory * pushed)
{
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER,
      (GstPadProbeCallback) watch_removed_memory, pushed, NULL);
}

GST_START_TEST (test_track_feed_frees_removed_media)
{
  /* Proves media that has played and is then removed from the source
   * buffer is freed while the pipeline keeps playing, without a seek: the
   * track feeds must not hold on to what they have pushed. */
  PushedMemory pushed = { 0 };
  g_mutex_init (&pushed.mutex);
  g_cond_init (&pushed.cond);
  pushed.live = g_hash_table_new (g_direct_hash, g_direct_equal);
  SinkFlow flow = { 0 };
  g_mutex_init (&flow.mutex);
  g_cond_init (&flow.cond);
  flow.pipeline = new_pipeline_past_end ();
  GstElement *src = g_object_new (GST_TYPE_MSE_SRC, NULL);
  gst_bin_add (GST_BIN (flow.pipeline), src);
  g_signal_connect (src, "pad-added", G_CALLBACK (probe_removed_memory),
      &pushed);
  g_signal_connect (src, "pad-added", G_CALLBACK (link_fakesink), &flow);
  GstMediaSource *media_source = gst_media_source_new ();
  gst_media_source_attach (media_source, GST_MSE_SRC (src));
  fail_if (gst_element_set_state (flow.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE);

  GError *error = NULL;
  GstSourceBuffer *source_buffer =
      gst_media_source_add_source_buffer (media_source, "video/mp4", &error);
  g_assert_no_error (error);
  SourceBufferEvents events;
  source_buffer_events_init (&events, source_buffer);
  fail_unless (gst_source_buffer_append_buffer (source_buffer,
          test_mp4_buffer (), &error));
  g_assert_no_error (error);
  await_update_end (&events, 1);
  fail_unless_equals_int (events.error, 0);
  fail_unless (gst_media_source_end_of_stream (media_source,
          GST_MEDIA_SOURCE_EOS_ERROR_NONE, &error));
  g_assert_no_error (error);
  await_sink_flow (&flow, 1, FALSE);

  fail_unless (gst_source_buffer_remove (source_buffer, 0, REMOVED_END,
          &error));
  g_assert_no_error (error);
  await_update_end (&events, 2);
  fail_unless_equals_int (events.error, 0);

  gint64 deadline = g_get_monotonic_time () + EVENT_TIMEOUT;
  g_mutex_lock (&pushed.mutex);
  fail_unless (pushed.pushed > 0, "msesrc pushed no media before %"
      GST_TIME_FORMAT, GST_TIME_ARGS (REMOVED_END));
  while (pushed.freed < pushed.pushed
      && g_cond_wait_until (&pushed.cond, &pushed.mutex, deadline)) {
  }
  guint n_pushed = pushed.pushed;
  guint n_freed = pushed.freed;
  g_mutex_unlock (&pushed.mutex);
  fail_unless_equals_int (n_freed, n_pushed);

  gst_element_set_state (flow.pipeline, GST_STATE_NULL);
  gst_object_unref (flow.pipeline);
  source_buffer_events_clear (&events, source_buffer);
  gst_object_unref (source_buffer);
  gst_object_unref (media_source);
  g_hash_table_unref (pushed.live);
  g_mutex_clear (&pushed.mutex);
  g_cond_clear (&pushed.cond);
  g_mutex_clear (&flow.mutex);
  g_cond_clear (&flow.cond);
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
  tcase_add_test (tc_media_source,
      test_media_source_element_seeks_with_debug_output);
  tcase_add_test (tc_media_source,
      test_media_source_element_frees_played_media);
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
    tcase_add_test (tc_source_buffer, test_source_buffer_freed_when_removed);
    tcase_add_test (tc_source_buffer,
        test_source_buffer_freed_at_end_of_stream);
    tcase_add_test (tc_source_buffer,
        test_media_source_element_plays_after_reopen);
    tcase_add_test (tc_source_buffer,
        test_media_source_element_ends_after_append_during_feed);
    tcase_add_test (tc_source_buffer,
        test_media_source_element_keeps_queued_media_on_reopen);
    tcase_add_test (tc_source_buffer,
        test_media_source_element_holds_eos_until_end);
    tcase_add_test (tc_source_buffer,
        test_media_source_element_seeks_while_resuming);
    tcase_add_test (tc_source_buffer,
        test_media_source_end_of_stream_races_reopen);
    tcase_add_test (tc_source_buffer, test_track_feed_frees_removed_media);
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
      test_append_pipeline_init_segment_alone_has_tracks);
  tcase_add_test (tc_append_pipeline,
      test_append_pipeline_init_segment_new_track_id);
  tcase_add_test (tc_append_pipeline,
      test_append_pipeline_init_segment_webvtt_track);
  tcase_add_test (tc_append_pipeline,
      test_append_pipeline_stream_type_from_caps);
  tcase_add_test (tc_append_pipeline,
      test_append_pipeline_media_segment_feeds_init_segment_tracks);
  tcase_add_test (tc_append_pipeline,
      test_append_pipeline_init_segment_reported_once);
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

/* GStreamer
 *
 * SPDX-License-Identifier: LGPL-2.1
 *
 * Copyright (C) 2016, 2017 Metrological Group B.V.
 * Copyright (C) 2016, 2017 Igalia S.L
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

#include "gstappendpipeline-private.h"

#include "mse.h"

#include "gstmselogging-private.h"
#include "gstmsemediatype-private.h"
#include "gstmediasourcetrack-private.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

typedef enum
{
  PARSE_ERROR,
  IGNORED,
  ADDED,
} AddTrackResult;

typedef struct
{
  GstAppendPipeline *pipeline;
  GstTask *task;
  GRecMutex mutex;
  GstBus *bus;
} BackgroundTask;

typedef struct
{
  GstAppendPipelineCallbacks callbacks;
  gpointer user_data;
} Callbacks;

typedef struct
{
  GstAppendPipeline *parent;
  /* The stream-id of the demuxer pad, or of parsebin's pad when there is no
   * demuxer. Parsers pass the demuxer's stream-start on, so parsebin's pad
   * for this track has the same id. */
  gchar *stream_id;
  /* parsebin's pad, and the stream parsebin made for it: NULL until
   * parsebin exposes the pad, which for a track known from the demuxer can
   * be after its initialization segment was reported. */
  GstPad *src_pad;
  GstStream *stream;
  GstAppSink *sink;
  GstMediaSourceTrack *mse_track;
  GstClockTime previous_pts;
} Track;

typedef struct
{
  GstClockTime duration;
  GPtrArray *video_tracks;
  GPtrArray *audio_tracks;
  GPtrArray *text_tracks;
} InitSegment;

struct _GstAppendPipeline
{
  GstObject parent_instance;

  GstPipeline *pipeline;
  GstAppSrc *src;
  /* The caps the parser starts from: set by gst_append_pipeline_set_caps(),
   * taken by the next reset when the source has others. */
  GstCaps *caps;
  GstElement *parsebin;
  GstBus *bus;

  /* Set once parsebin plugs a container demuxer. The demuxer's pads, which
   * it adds as it parses the initialization segment, then give the tracks,
   * and its no-more-pads reports the initialization segment. parsebin
   * exposes a pad only once the parser behind it has caps and a stream-id,
   * which some parsers take from the first frame, so its pads only carry
   * the samples. */
  gboolean have_demuxer;

  GstStreamCollection *streams;
  /* The collection the last init segment report was made for. The report
   * can be reached from the stream-collection message on the bus task and
   * from each pad-added on the streaming thread, whichever last finds all
   * tracks present; both must not report the same init segment. */
  GstStreamCollection *reported_streams;
  GArray *tracks;
  /* The tracks of the init segments before the last reset that no init
   * segment since has taken back. The source buffer keeps its track buffers
   * and feeds for them, so the init segment that follows a reset gets them
   * back rather than new tracks with the same ids. */
  GPtrArray *retired_tracks;

  gboolean received_init_segment;
  gboolean have_outstanding_samples;
  InitSegment init_segment;
  /* Guards init_segment's track arrays and received_init_segment: the
   * streaming thread adds tracks, reset() replaces the arrays, and the
   * getters copy them for another source buffer's streaming thread, which
   * collects every buffer's tracks. Never held while calling out, so it
   * nests inside any other lock. */
  GMutex init_segment_lock;

  gboolean encountered_error;

  BackgroundTask *task;

  Callbacks callbacks;

  /* Appends pushed into the pipeline whose end-of-append the background
   * task has not consumed yet; waited on by
   * gst_append_pipeline_wait_for_appends(). */
  GMutex appends_lock;
  GCond appends_cond;
  guint appends_in_flight;
  gboolean appends_abandoned;
};

static void
appends_done (GstAppendPipeline * self, gboolean abandoned)
{
  g_mutex_lock (&self->appends_lock);
  if (abandoned) {
    self->appends_abandoned = TRUE;
    self->appends_in_flight = 0;
  } else if (self->appends_in_flight > 0) {
    self->appends_in_flight--;
  }
  g_cond_broadcast (&self->appends_cond);
  g_mutex_unlock (&self->appends_lock);
}

G_DEFINE_TYPE (GstAppendPipeline, gst_append_pipeline, GST_TYPE_OBJECT);

#define END_OF_APPEND "end-of-append"
#define ABORT "abort"
#define SHUTDOWN "shutdown"

static void process_init_segment (GstAppendPipeline *);

static gboolean
send_abort (GstAppendPipeline * self)
{
  return gst_bus_post (self->bus, gst_message_new_application (NULL,
          gst_structure_new_empty (ABORT)));
}

static gboolean
send_shutdown (GstAppendPipeline * self)
{
  return gst_bus_post (self->bus, gst_message_new_application (NULL,
          gst_structure_new_empty (SHUTDOWN)));
}

static GstEvent *
new_end_of_append_event (void)
{
  GstStructure *structure = gst_structure_new_empty (END_OF_APPEND);
  return gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM, structure);
}

static gboolean
is_end_of_append_event (GstEvent * event)
{
  return GST_EVENT_TYPE (event) == GST_EVENT_CUSTOM_DOWNSTREAM
      && gst_event_has_name (event, END_OF_APPEND);
}

static inline guint
n_tracks (GstAppendPipeline * self)
{
  return self->tracks->len;
}

static inline Track *
index_track (GstAppendPipeline * self, guint i)
{
  return &g_array_index (self->tracks, Track, i);
}

static inline void
call_parse_error (GstAppendPipeline * self)
{
  GstAppendPipelineCallbacks *callbacks = &self->callbacks.callbacks;
  gpointer user_data = self->callbacks.user_data;

  if (callbacks->error) {
    callbacks->error (self, user_data);
    GST_TRACE_OBJECT (self, "done");
  } else {
    GST_TRACE_OBJECT (self, "dropping");
  }
}

static inline void
call_received_init_segment (GstAppendPipeline * self)
{
  GstAppendPipelineCallbacks *callbacks = &self->callbacks.callbacks;
  gpointer user_data = self->callbacks.user_data;

  if (callbacks->received_init_segment) {
    callbacks->received_init_segment (self, user_data);
    GST_TRACE_OBJECT (self, "done");
  } else {
    GST_TRACE_OBJECT (self, "dropping");
  }
}

static inline void
call_new_sample (GstAppendPipeline * self, GstMediaSourceTrack * track,
    GstSample * sample)
{
  GstAppendPipelineCallbacks *callbacks = &self->callbacks.callbacks;
  gpointer user_data = self->callbacks.user_data;

  if (callbacks->new_sample) {
    callbacks->new_sample (self, track, sample, user_data);
    GST_TRACE_OBJECT (self, "done");
  } else {
    GST_TRACE_OBJECT (self, "dropping");
  }
}

static inline void
call_duration_changed (GstAppendPipeline * self)
{
  GstAppendPipelineCallbacks *callbacks = &self->callbacks.callbacks;
  gpointer user_data = self->callbacks.user_data;

  if (callbacks->duration_changed) {
    callbacks->duration_changed (self, user_data);
    GST_TRACE_OBJECT (self, "done");
  } else {
    GST_TRACE_OBJECT (self, "dropping");
  }
}

static inline void
call_eos (GstAppendPipeline * self, GstMediaSourceTrack * track)
{
  GstAppendPipelineCallbacks *callbacks = &self->callbacks.callbacks;
  gpointer user_data = self->callbacks.user_data;

  if (callbacks->eos) {
    callbacks->eos (self, track, user_data);
    GST_TRACE_OBJECT (self, "done");
  } else {
    GST_TRACE_OBJECT (self, "dropping");
  }
}

static inline GstSample *
patch_missing_duration (GstAppendPipeline * self, GstSample * sample)
{
  GstBuffer *buffer = gst_sample_get_buffer (sample);
  if (!GST_BUFFER_DURATION_IS_VALID (buffer)) {
    GST_BUFFER_DURATION (buffer) = GST_SECOND / 60;
    GST_TRACE_OBJECT (self, "sample is missing duration, patched to %"
        GST_TIMEP_FORMAT, &buffer->duration);
  }
  return sample;
}

static inline GstSample *
patch_missing_pts (GstAppendPipeline * self, GstSample * sample, GstClockTime
    fallback)
{
  GstBuffer *buffer = gst_sample_get_buffer (sample);
  if (!GST_BUFFER_PTS_IS_VALID (buffer) && GST_CLOCK_TIME_IS_VALID (fallback)) {
    GST_TRACE_OBJECT (self, "sample is missing pts, patching with %"
        GST_TIMEP_FORMAT, &fallback);
    GST_BUFFER_PTS (buffer) = fallback;
  }
  return sample;
}

static inline GstSample *
patch_missing_dts (GstAppendPipeline * self, GstSample * sample)
{
  GstBuffer *buffer = gst_sample_get_buffer (sample);
  if (!GST_BUFFER_DTS_IS_VALID (buffer) && GST_BUFFER_PTS_IS_VALID (buffer)) {
    GST_TRACE_OBJECT (self, "sample is missing dts, patching with pts %"
        GST_TIMEP_FORMAT, &buffer->pts);
    GST_BUFFER_DTS (buffer) = GST_BUFFER_PTS (buffer);
  }
  return sample;
}

static gboolean
consume_sample_from_track (GstAppendPipeline * self, Track * track)
{
  GstSample *sample = gst_app_sink_try_pull_sample (track->sink, 0);
  if (sample == NULL) {
    return FALSE;
  }
  GstBuffer *buffer = gst_sample_get_buffer (sample);
  if (!GST_IS_BUFFER (buffer)) {
    GST_WARNING_OBJECT (self, "got null buffer in sample");
    goto done;
  }
  sample = patch_missing_pts (self, sample, track->previous_pts);
  sample = patch_missing_duration (self, sample);
  sample = patch_missing_dts (self, sample);
  track->previous_pts = GST_BUFFER_PTS (buffer);
  call_new_sample (self, track->mse_track, sample);

done:
  gst_clear_sample (&sample);
  return TRUE;
}

static void
consume_all_samples (GstAppendPipeline * self)
{
  if (!self->received_init_segment) {
    GST_DEBUG_OBJECT (self, "not all tracks are available, delaying");
    self->have_outstanding_samples = TRUE;
    return;
  }
  guint track_count = n_tracks (self);
  while (TRUE) {
    gboolean sample_consumed = FALSE;
    for (guint i = 0; i < track_count; i++) {
      Track *track = index_track (self, i);
      sample_consumed |= consume_sample_from_track (self, track);
    }
    if (!sample_consumed) {
      break;
    }
  }
  call_duration_changed (self);
  self->have_outstanding_samples = FALSE;
}

static void
handle_shutdown (BackgroundTask * task)
{
  gst_task_stop (task->task);
  GstAppendPipeline *self = task->pipeline;
  appends_done (self, TRUE);
  guint track_count = n_tracks (self);
  for (guint i = 0; i < track_count; i++) {
    Track *track = index_track (self, i);
    call_eos (self, track->mse_track);
  }
  call_eos (self, NULL);
}

static void
handle_abort (BackgroundTask * task)
{
  gst_task_stop (task->task);
  appends_done (task->pipeline, TRUE);
}

static void
task_function (gpointer user_data)
{
  BackgroundTask *task = (BackgroundTask *) user_data;
  GstAppendPipeline *self = task->pipeline;
  GstMessage *message = gst_bus_timed_pop (task->bus, GST_CLOCK_TIME_NONE);
  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_APPLICATION:{
      if (gst_message_has_name (message, END_OF_APPEND)) {
        GST_TRACE_OBJECT (self, "end of append");
        consume_all_samples (self);
        appends_done (self, FALSE);
        goto done;
      }
      if (gst_message_has_name (message, SHUTDOWN)) {
        GST_DEBUG_OBJECT (self, "shutdown");
        handle_shutdown (task);
        goto done;
      }
      if (gst_message_has_name (message, ABORT)) {
        GST_DEBUG_OBJECT (self, "abort");
        handle_abort (task);
        goto done;
      }
      g_error ("received unsupported application message");
    }
    case GST_MESSAGE_STREAM_COLLECTION:{
      GST_DEBUG_OBJECT (self, "stream collection");
      GstStreamCollection *streams;
      gst_message_parse_stream_collection (message, &streams);
      gst_clear_object (&self->streams);
      self->streams = streams;
      process_init_segment (self);
      goto done;
    }
    case GST_MESSAGE_EOS:
      GST_DEBUG_OBJECT (self, "end of stream");
      if (self->have_outstanding_samples) {
        GST_DEBUG_OBJECT (self, "consuming remaining samples before EOS");
        consume_all_samples (self);
      }
      handle_shutdown (task);
      goto done;
    case GST_MESSAGE_ERROR:
      GST_DEBUG_OBJECT (self, "error: %" GST_PTR_FORMAT, message);
      self->encountered_error = TRUE;
      call_parse_error (self);
      handle_shutdown (task);
      goto done;
    default:
      GST_TRACE_OBJECT (self, "ignoring message %" GST_PTR_FORMAT, message);
      goto done;
  }
done:
  gst_message_unref (message);
}

static GstMediaSourceTrack *
reuse_or_new_track (GstAppendPipeline * self, GstMediaSourceTrackType type,
    const gchar * id, GstCaps * caps)
{
  for (guint i = 0; i < self->retired_tracks->len; i++) {
    GstMediaSourceTrack *track = g_ptr_array_index (self->retired_tracks, i);
    if (gst_media_source_track_get_track_type (track) == type
        && g_str_equal (gst_media_source_track_get_id (track), id)) {
      GST_DEBUG_OBJECT (self, "reusing track %s from before the reset", id);
      return g_ptr_array_steal_index (self->retired_tracks, i);
    }
  }
  return gst_media_source_track_new_with_initial_caps (type, id, caps);
}

static inline GstAppSink *
new_appsink (GstAppendPipeline * self, GstStreamType type)
{
  const gchar *type_name = gst_stream_type_get_name (type);
  gchar *name = g_strdup_printf ("%s-%u", type_name, n_tracks (self));
  GstAppSink *appsink =
      GST_APP_SINK (gst_element_factory_make ("appsink", name));
  gst_base_sink_set_sync (GST_BASE_SINK (appsink), FALSE);
  gst_base_sink_set_async_enabled (GST_BASE_SINK (appsink), FALSE);
  gst_base_sink_set_drop_out_of_segment (GST_BASE_SINK (appsink), FALSE);
  gst_base_sink_set_last_sample_enabled (GST_BASE_SINK (appsink), FALSE);
  g_free (name);
  return appsink;
}

static GstPadProbeReturn
black_hole_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  g_return_val_if_fail (GST_PAD_PROBE_INFO_TYPE (info) &
      GST_PAD_PROBE_TYPE_BUFFER, GST_PAD_PROBE_DROP);
  return GST_PAD_PROBE_DROP;
}

static GstPadProbeReturn
event_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstAppendPipeline *self = GST_APPEND_PIPELINE (user_data);
  GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

  if (is_end_of_append_event (event)) {
    GST_TRACE_OBJECT (self, "end of append event");
    if (gst_bus_post (self->bus, gst_message_new_application (NULL,
                gst_structure_new_empty (END_OF_APPEND)))) {
      return GST_PAD_PROBE_DROP;
    } else {
      GST_ERROR_OBJECT (self, "failed to post end of append");
      goto error;
    }
  }

  if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
    GST_DEBUG_OBJECT (self, "eos event");
    if (send_shutdown (self)) {
      return GST_PAD_PROBE_OK;
    } else {
      GST_ERROR_OBJECT (self, "failed to post shutdown");
      goto error;
    }
  }

  return GST_PAD_PROBE_OK;

error:
  GST_PAD_PROBE_INFO_FLOW_RETURN (info) = GST_FLOW_ERROR;
  gst_event_unref (event);
  return GST_PAD_PROBE_HANDLED;
}

/* Whether a stream of this type and caps can be a track: IGNORED for a type
 * MSE has no track for, PARSE_ERROR for caps no element can take. */
static AddTrackResult
check_track (GstAppendPipeline * self, GstStreamType type, GstCaps * caps)
{
  switch (type) {
    case GST_STREAM_TYPE_AUDIO:
    case GST_STREAM_TYPE_TEXT:
    case GST_STREAM_TYPE_VIDEO:
      break;
    default:
      GST_DEBUG_OBJECT (self, "no track for caps %" GST_PTR_FORMAT, caps);
      return IGNORED;
  }

  if (type != GST_STREAM_TYPE_TEXT &&
      !gst_media_source_media_type_is_caps_supported (caps)) {
    GST_ERROR_OBJECT (self, "unsupported caps: %" GST_PTR_FORMAT, caps);
    return PARSE_ERROR;
  }

  return ADDED;
}

/* Adds a track with its appsink, not linked yet. The returned pointer is
 * valid until the next track is added. */
static Track *
append_track (GstAppendPipeline * self, GstStreamType type,
    const gchar * stream_id, GstCaps * caps)
{
  GstMediaSourceTrackType track_type =
      gst_media_source_track_type_from_stream_type (type);

  GstAppSink *appsink = new_appsink (self, type);
  gst_bin_add (GST_BIN (self->pipeline), GST_ELEMENT (appsink));
  gst_element_sync_state_with_parent (GST_ELEMENT (appsink));

  Track track_template = {
    .parent = self,
    .stream_id = g_strdup (stream_id),
    .sink = gst_object_ref (appsink),
    .mse_track = reuse_or_new_track (self, track_type,
        GST_OBJECT_NAME (appsink), caps),
    .previous_pts = GST_CLOCK_TIME_NONE,
  };
  g_array_append_val (self->tracks, track_template);

  GST_TRACE_OBJECT (self, "added appsink %s for stream %s",
      GST_OBJECT_NAME (appsink), stream_id);

  return index_track (self, n_tracks (self) - 1);
}

static void
link_track (GstAppendPipeline * self, Track * track, GstPad * pad)
{
  GstPad *appsink_pad =
      gst_element_get_static_pad (GST_ELEMENT (track->sink), "sink");
  GstPadLinkReturn link_result = gst_pad_link (pad, appsink_pad);
  gst_clear_object (&appsink_pad);
  if (GST_PAD_LINK_FAILED (link_result)) {
    g_error ("failed to link parser to appsink: %s",
        gst_pad_link_get_name (link_result));
  }
  track->src_pad = gst_object_ref (pad);
  track->stream = gst_pad_get_stream (pad);

  GST_TRACE_OBJECT (self, "linked appsink %s to pad %s",
      GST_OBJECT_NAME (track->sink), GST_OBJECT_NAME (pad));
}

static Track *
find_track (GstAppendPipeline * self, const gchar * stream_id)
{
  guint track_count = n_tracks (self);
  for (guint i = 0; i < track_count; i++) {
    Track *track = index_track (self, i);
    if (g_strcmp0 (track->stream_id, stream_id) == 0) {
      return track;
    }
  }
  return NULL;
}

static void
clear_track (Track * track)
{
  g_clear_pointer (&track->stream_id, g_free);
  gst_clear_object (&track->sink);
  gst_clear_object (&track->src_pad);
  gst_clear_object (&track->mse_track);
  gst_clear_object (&track->stream);
}

static inline GPtrArray *
init_segment_tracks_for (InitSegment * self, GstStreamType type)
{
  switch (type) {
    case GST_STREAM_TYPE_AUDIO:
      return self->audio_tracks;
    case GST_STREAM_TYPE_TEXT:
      return self->text_tracks;
    case GST_STREAM_TYPE_VIDEO:
      return self->video_tracks;
    default:
      g_assert_not_reached ();
      return NULL;
  }
}

/* Lists the track in the initialization segment; the first track of each
 * type is the active one. */
static void
add_init_segment_track (GstAppendPipeline * self, GstStreamType type,
    GstMediaSourceTrack * mse_track)
{
  g_mutex_lock (&self->init_segment_lock);
  GPtrArray *tracks = init_segment_tracks_for (&self->init_segment, type);
  gboolean first = tracks->len < 1;
  g_ptr_array_add (tracks, gst_object_ref (mse_track));
  g_mutex_unlock (&self->init_segment_lock);
  if (first) {
    gst_media_source_track_set_active (mse_track, TRUE);
  }
}

static void
process_init_segment_track (GstPad * pad, GstAppendPipeline * self)
{
  GST_OBJECT_LOCK (self);
  GstStream *stream = gst_pad_get_stream (pad);
  GstCaps *caps = gst_stream_get_caps (stream);
  gchar *stream_id = gst_pad_get_stream_id (pad);
  GST_DEBUG_OBJECT (self, "%" GST_PTR_FORMAT " (stream %s) got caps %"
      GST_PTR_FORMAT, pad, stream_id, caps);

  if (gst_pad_is_linked (pad)) {
    GST_TRACE_OBJECT (self, "%" GST_PTR_FORMAT " is already linked, skipping",
        pad);
    goto done;
  }

  /* A track the demuxer already gave: its samples can flow now. */
  Track *track = find_track (self, stream_id);
  if (track != NULL && track->src_pad == NULL) {
    link_track (self, track, pad);
    goto done;
  }

  if (!GST_IS_CAPS (caps)) {
    GST_ERROR_OBJECT (self, "no caps on %" GST_PTR_FORMAT
        " after stream collection", pad);
    call_parse_error (self);
    goto done;
  }

  GstStreamType type = gst_stream_get_stream_type (stream);
  switch (check_track (self, type, caps)) {
    case ADDED:
      track = append_track (self, type, stream_id, caps);
      link_track (self, track, pad);
      add_init_segment_track (self, type, track->mse_track);
      break;
    case IGNORED:
      gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, black_hole_probe, self,
          NULL);
      break;
    case PARSE_ERROR:
      call_parse_error (self);
      break;
  }

done:
  g_free (stream_id);
  gst_clear_object (&stream);
  gst_clear_caps (&caps);
  GST_OBJECT_UNLOCK (self);
}

static void
on_pad_added (GstElement * parsebin, GstPad * pad, gpointer user_data)
{
  GstAppendPipeline *self = GST_APPEND_PIPELINE (user_data);
  process_init_segment_track (pad, self);
  process_init_segment (self);
}

static gboolean
has_track_for_stream (GstAppendPipeline * self, GstStream * stream)
{
  guint track_count = n_tracks (self);
  for (guint i = 0; i < track_count; i++) {
    Track *track = index_track (self, i);
    if (track->stream == stream) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
has_all_tracks (GstAppendPipeline * self)
{
  if (!GST_IS_STREAM_COLLECTION (self->streams)) {
    return FALSE;
  }
  for (guint i = 0; i < gst_stream_collection_get_size (self->streams); i++) {
    GstStream *stream = gst_stream_collection_get_stream (self->streams, i);
    switch (gst_stream_get_stream_type (stream)) {
      case GST_STREAM_TYPE_AUDIO:
      case GST_STREAM_TYPE_VIDEO:
      case GST_STREAM_TYPE_TEXT:
        break;
      default:
        continue;
    }
    if (!has_track_for_stream (self, stream)) {
      return FALSE;
    }
  }
  return TRUE;
}

/* Called with the object lock held. The duration comes from the element that
 * parsed the header: before parsebin exposes a pad, a query on parsebin
 * reaches no demuxer. */
static void
report_init_segment (GstAppendPipeline * self, GstElement * parser)
{
  gint64 duration;
  InitSegment *init_segment = &self->init_segment;

  if (gst_element_query_duration (parser, GST_FORMAT_TIME, &duration)) {
    init_segment->duration = MAX (0, duration);
  } else {
    init_segment->duration = GST_CLOCK_TIME_NONE;
  }

  GST_DEBUG_OBJECT (self, "init segment says duration=%" GST_TIME_FORMAT,
      GST_TIME_ARGS ((GstClockTime) duration));

  g_mutex_lock (&self->init_segment_lock);
  self->received_init_segment = TRUE;
  g_mutex_unlock (&self->init_segment_lock);

  call_received_init_segment (self);
}

/* Reports the initialization segment once parsebin's stream collection has
 * a track for each of its streams. That is the whole report for bytes with
 * no demuxer, such as ADTS AAC; with a demuxer it is only the fallback for
 * one that never says no-more-pads. */
static void
process_init_segment (GstAppendPipeline * self)
{
  GST_OBJECT_LOCK (self);

  if (self->have_demuxer && self->received_init_segment) {
    goto done;
  }

  if (!has_all_tracks (self)) {
    goto done;
  }

  if (self->streams == self->reported_streams) {
    GST_DEBUG_OBJECT (self, "init segment already reported for %"
        GST_PTR_FORMAT, self->streams);
    goto done;
  }
  gst_clear_object (&self->reported_streams);
  self->reported_streams = gst_object_ref (self->streams);

  report_init_segment (self, self->parsebin);

done:
  GST_OBJECT_UNLOCK (self);
}

static GstStreamType
stream_type_from_caps (GstCaps * caps)
{
  GstStructure *structure = gst_caps_get_structure (caps, 0);
  const gchar *name = gst_structure_get_name (structure);
  if (gst_structure_has_field (structure, "original-media-type")) {
    name = gst_structure_get_string (structure, "original-media-type");
  }
  if (g_str_has_prefix (name, "video/") || g_str_has_prefix (name, "image/")) {
    return GST_STREAM_TYPE_VIDEO;
  }
  if (g_str_has_prefix (name, "audio/")) {
    return GST_STREAM_TYPE_AUDIO;
  }
  if (g_str_has_prefix (name, "text/") || g_str_has_prefix (name, "subtitle/")
      || g_str_has_prefix (name, "subpicture/")
      || g_str_has_prefix (name, "closedcaption/")) {
    return GST_STREAM_TYPE_TEXT;
  }
  return GST_STREAM_TYPE_UNKNOWN;
}

typedef struct
{
  gchar *stream_id;
  GstCaps *caps;
  GstStreamType type;
} DemuxerStream;

static void
demuxer_stream_clear (DemuxerStream * stream)
{
  g_clear_pointer (&stream->stream_id, g_free);
  gst_clear_caps (&stream->caps);
}

static guint
type_rank (GstStreamType type)
{
  switch (type) {
    case GST_STREAM_TYPE_VIDEO:
      return 0;
    case GST_STREAM_TYPE_AUDIO:
      return 1;
    case GST_STREAM_TYPE_TEXT:
      return 2;
    default:
      return 3;
  }
}

/* parsebin's order: video, then audio, then text, then by stream-id. Tracks
 * are named by their position, and a reset keeps a track only for the same
 * type and name, so the order must not depend on the order of the
 * container's tracks. */
static gint
compare_demuxer_streams (gconstpointer a, gconstpointer b)
{
  const DemuxerStream *stream_a = a;
  const DemuxerStream *stream_b = b;
  guint rank_a = type_rank (stream_a->type);
  guint rank_b = type_rank (stream_b->type);
  if (rank_a != rank_b) {
    return rank_a < rank_b ? -1 : 1;
  }
  return g_strcmp0 (stream_a->stream_id, stream_b->stream_id);
}

static gboolean
add_demuxer_stream (GstElement * demuxer, GstPad * pad, gpointer user_data)
{
  GArray *streams = user_data;
  DemuxerStream stream = {
    .stream_id = gst_pad_get_stream_id (pad),
    .caps = gst_pad_get_current_caps (pad),
  };
  if (stream.stream_id == NULL || stream.caps == NULL
      || gst_caps_is_empty (stream.caps)) {
    GST_DEBUG_OBJECT (pad, "no stream yet");
    demuxer_stream_clear (&stream);
    return TRUE;
  }
  stream.type = stream_type_from_caps (stream.caps);
  g_array_append_val (streams, stream);
  return TRUE;
}

/* The demuxer has added a pad for every track of the initialization
 * segment, each with its stream-start and caps: they are the tracks. */
static void
on_demuxer_no_more_pads (GstElement * demuxer, gpointer user_data)
{
  GstAppendPipeline *self = GST_APPEND_PIPELINE (user_data);
  GArray *streams = g_array_new (FALSE, TRUE, sizeof (DemuxerStream));
  g_array_set_clear_func (streams, (GDestroyNotify) demuxer_stream_clear);
  gst_element_foreach_src_pad (demuxer, add_demuxer_stream, streams);
  g_array_sort (streams, compare_demuxer_streams);

  GST_OBJECT_LOCK (self);
  gboolean added = FALSE;
  gboolean failed = FALSE;
  for (guint i = 0; i < streams->len; i++) {
    DemuxerStream *stream = &g_array_index (streams, DemuxerStream, i);
    if (find_track (self, stream->stream_id) != NULL) {
      continue;
    }
    switch (check_track (self, stream->type, stream->caps)) {
      case ADDED:{
        Track *track = append_track (self, stream->type, stream->stream_id,
            stream->caps);
        add_init_segment_track (self, stream->type, track->mse_track);
        added = TRUE;
        break;
      }
      case IGNORED:
        break;
      case PARSE_ERROR:
        failed = TRUE;
        break;
    }
  }
  g_array_unref (streams);

  if (failed) {
    call_parse_error (self);
  } else if (added || !self->received_init_segment) {
    /* A later initialization segment with the same tracks, such as a
     * quality switch, makes the demuxer say no-more-pads again: it
     * changes no track and is not reported again. */
    report_init_segment (self, demuxer);
  }
  GST_OBJECT_UNLOCK (self);
}

static gboolean
is_container_demuxer (GstElement * element)
{
  GstElementFactory *factory = gst_element_get_factory (element);
  if (factory == NULL || !gst_element_factory_list_is_type (factory,
          GST_ELEMENT_FACTORY_TYPE_DEMUXER)) {
    return FALSE;
  }
  /* A tag demuxer such as id3demux, "Codec/Demuxer/Metadata", strips a tag
   * and passes the rest on whole: its pad is not a track. */
  const gchar *klass = gst_element_factory_get_metadata (factory,
      GST_ELEMENT_METADATA_KLASS);
  return klass != NULL && strstr (klass, "Metadata") == NULL;
}

static void
on_deep_element_added (GstBin * parsebin, GstBin * sub_bin,
    GstElement * element, gpointer user_data)
{
  GstAppendPipeline *self = GST_APPEND_PIPELINE (user_data);
  if (!is_container_demuxer (element)) {
    return;
  }

  GST_OBJECT_LOCK (self);
  gboolean first = !self->have_demuxer;
  self->have_demuxer = TRUE;
  GST_OBJECT_UNLOCK (self);

  /* Only the container's demuxer, the first one parsebin plugs, gives the
   * tracks. */
  if (!first) {
    GST_DEBUG_OBJECT (self, "not watching %" GST_PTR_FORMAT, element);
    return;
  }
  GST_DEBUG_OBJECT (self, "tracks come from %" GST_PTR_FORMAT, element);
  g_signal_connect_object (element, "no-more-pads",
      G_CALLBACK (on_demuxer_no_more_pads), self, 0);
}

static inline void
init_segment_init (InitSegment * self)
{
  self->audio_tracks = g_ptr_array_new_with_free_func (gst_object_unref);
  self->text_tracks = g_ptr_array_new_with_free_func (gst_object_unref);
  self->video_tracks = g_ptr_array_new_with_free_func (gst_object_unref);
  self->duration = GST_CLOCK_TIME_NONE;
}

static inline void
init_segment_finalize (InitSegment * self)
{
  g_ptr_array_free (self->audio_tracks, TRUE);
  g_ptr_array_free (self->text_tracks, TRUE);
  g_ptr_array_free (self->video_tracks, TRUE);
}

static GArray *
new_tracks_array (void)
{
  GArray *tracks = g_array_new (TRUE, TRUE, sizeof (Track));
  g_array_set_clear_func (tracks, (GDestroyNotify) clear_track);
  return tracks;
}

static BackgroundTask *
background_task_new (GstAppendPipeline * pipeline)
{
  BackgroundTask *task = g_new0 (BackgroundTask, 1);
  g_rec_mutex_init (&task->mutex);
  task->task = gst_task_new (task_function, task, NULL);
  task->pipeline = pipeline;
  task->bus = gst_object_ref (pipeline->bus);
  gst_task_set_lock (task->task, &task->mutex);
  return task;
}

static gboolean
background_task_start (BackgroundTask * task)
{
  gst_bus_set_flushing (task->bus, FALSE);
  gchar *name = g_strdup_printf ("%s:bg", GST_OBJECT_NAME (task->pipeline));
  g_object_set (task->task, "name", name, NULL);
  g_clear_pointer (&name, g_free);
  return gst_task_start (task->task);
}

static gboolean
background_task_stop (BackgroundTask * task)
{
  send_abort (task->pipeline);
  gst_task_join (task->task);
  gst_bus_set_flushing (task->bus, TRUE);
  return TRUE;
}

static void
background_task_cleanup (gpointer ptr)
{
  BackgroundTask *task = (BackgroundTask *) ptr;
  background_task_stop (task);
  task->pipeline = NULL;
  gst_clear_object (&task->task);
  gst_clear_object (&task->bus);
  g_rec_mutex_clear (&task->mutex);
  g_free (task);
}

static GstElement *
new_appsrc (GstAppendPipeline * self)
{
  GstElement *appsrc = GST_ELEMENT (gst_element_factory_make ("appsrc", "src"));
  GstPad *appsrc_pad = GST_PAD (gst_element_get_static_pad (appsrc, "src"));
  gst_pad_add_probe (appsrc_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      event_probe, self, NULL);
  gst_object_unref (appsrc_pad);
  return appsrc;
}

static void
gst_append_pipeline_init (GstAppendPipeline * self)
{
  GstElement *appsrc = new_appsrc (self);
  GstElement *parsebin =
      GST_ELEMENT (gst_element_factory_make ("parsebin", "parse"));
  GstElement *pipeline = gst_pipeline_new ("append-pipeline");

  gst_bin_add_many (GST_BIN (pipeline), appsrc, parsebin, NULL);
  if (!gst_element_link (appsrc, parsebin)) {
    g_error ("failed to link appsrc to parsebin");
  }

  self->bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  self->pipeline = gst_object_ref_sink (GST_PIPELINE (pipeline));
  self->src = GST_APP_SRC (appsrc);
  self->parsebin = parsebin;

  self->task = background_task_new (self);

  g_signal_connect_object (parsebin, "pad-added", G_CALLBACK (on_pad_added),
      self, 0);
  /* Connected before parsebin connects its own handlers to the demuxer, so
   * the tracks exist by the time parsebin exposes pads for them. */
  g_signal_connect_object (parsebin, "deep-element-added",
      G_CALLBACK (on_deep_element_added), self, 0);

  self->have_demuxer = FALSE;
  self->received_init_segment = FALSE;
  self->encountered_error = FALSE;
  self->tracks = new_tracks_array ();
  self->retired_tracks = g_ptr_array_new_with_free_func (gst_object_unref);
  init_segment_init (&self->init_segment);
  g_mutex_init (&self->init_segment_lock);

  g_mutex_init (&self->appends_lock);
  g_cond_init (&self->appends_cond);
  self->appends_in_flight = 0;
  self->appends_abandoned = FALSE;
}

static void
gst_append_pipeline_dispose (GObject * object)
{
  GstAppendPipeline *self = (GstAppendPipeline *) object;

  send_shutdown (self);
  g_clear_pointer (&self->task, background_task_cleanup);

  gst_element_set_state (GST_ELEMENT (self->pipeline), GST_STATE_NULL);

  G_OBJECT_CLASS (gst_append_pipeline_parent_class)->dispose (object);
}

static void
gst_append_pipeline_finalize (GObject * object)
{
  GstAppendPipeline *self = (GstAppendPipeline *) object;

  gst_clear_object (&self->pipeline);
  gst_clear_object (&self->bus);
  gst_clear_caps (&self->caps);

  init_segment_finalize (&self->init_segment);

  g_array_free (self->tracks, TRUE);
  g_ptr_array_unref (self->retired_tracks);
  gst_clear_object (&self->streams);
  gst_clear_object (&self->reported_streams);

  g_mutex_clear (&self->init_segment_lock);
  g_mutex_clear (&self->appends_lock);
  g_cond_clear (&self->appends_cond);

  G_OBJECT_CLASS (gst_append_pipeline_parent_class)->finalize (object);
}

static void
gst_append_pipeline_class_init (GstAppendPipelineClass * klass)
{
  GObjectClass *oclass = G_OBJECT_CLASS (klass);

  oclass->dispose = GST_DEBUG_FUNCPTR (gst_append_pipeline_dispose);
  oclass->finalize = GST_DEBUG_FUNCPTR (gst_append_pipeline_finalize);
}

GstAppendPipeline *
gst_append_pipeline_new (GstAppendPipelineCallbacks * callbacks,
    gpointer user_data, GError ** error)
{
  gst_mse_init_logging ();
  GstAppendPipeline *self = g_object_new (GST_TYPE_APPEND_PIPELINE, NULL);
  GstStateChangeReturn started =
      gst_element_set_state (GST_ELEMENT (self->pipeline), GST_STATE_PLAYING);
  if (started != GST_STATE_CHANGE_SUCCESS) {
    GST_ERROR_OBJECT (self, "failed to start: %s",
        gst_state_change_return_get_name (started));
    g_set_error (error,
        GST_MEDIA_SOURCE_ERROR, GST_MEDIA_SOURCE_ERROR_INVALID_STATE,
        "failed to start append pipeline");
    goto error;
  }
  if (callbacks) {
    self->callbacks.callbacks = *callbacks;
    self->callbacks.user_data = user_data;
  }
  if (!background_task_start (self->task)) {
    GST_ERROR_OBJECT (self, "failed to start background task");
    g_set_error (error,
        GST_MEDIA_SOURCE_ERROR, GST_MEDIA_SOURCE_ERROR_INVALID_STATE,
        "failed to start append pipeline's background task");
    goto error;
  }
  return gst_object_ref_sink (self);
error:
  gst_clear_object (&self);
  return NULL;
}

/* Tells the parser what the appended bytes are, so it does not have to
 * typefind them first; %NULL caps make it typefind. A source that has no
 * caps yet takes them at once. One that has other caps keeps them until the
 * next reset: appsrc cannot go back to no caps, and after a restart it
 * announces the caps it last pushed before any queued ones, which would make
 * parsebin pick the old type's demuxer. */
void
gst_append_pipeline_set_caps (GstAppendPipeline * self, GstCaps * caps)
{
  g_return_if_fail (GST_IS_APPEND_PIPELINE (self));
  gst_caps_replace (&self->caps, caps);
  GstCaps *src_caps = gst_app_src_get_caps (self->src);
  if (src_caps == NULL && caps != NULL) {
    gst_app_src_set_caps (self->src, caps);
  }
  gst_clear_caps (&src_caps);
}

/* Replaces the stopped pipeline's source with a new one when its caps are
 * not the ones last set. */
static void
renew_appsrc_if_needed (GstAppendPipeline * self)
{
  GstCaps *src_caps = gst_app_src_get_caps (self->src);
  gboolean same = src_caps == self->caps || (src_caps != NULL
      && self->caps != NULL && gst_caps_is_equal (src_caps, self->caps));
  gst_clear_caps (&src_caps);
  if (same) {
    return;
  }

  GST_DEBUG_OBJECT (self, "new source for caps %" GST_PTR_FORMAT, self->caps);
  GstElement *old_appsrc = GST_ELEMENT (self->src);
  gst_element_set_state (old_appsrc, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (self->pipeline), old_appsrc);

  GstElement *appsrc = new_appsrc (self);
  gst_bin_add (GST_BIN (self->pipeline), appsrc);
  if (!gst_element_link (appsrc, self->parsebin)) {
    g_error ("failed to link appsrc to parsebin");
  }
  if (self->caps != NULL) {
    gst_app_src_set_caps (GST_APP_SRC (appsrc), self->caps);
  }
  self->src = GST_APP_SRC (appsrc);
}

GstFlowReturn
gst_append_pipeline_append (GstAppendPipeline * self, GstBuffer * buffer)
{
  g_return_val_if_fail (GST_IS_APPEND_PIPELINE (self), GST_FLOW_ERROR);
  GstFlowReturn push_result = gst_app_src_push_buffer (self->src, buffer);
  if (push_result != GST_FLOW_OK)
    return push_result;

  g_mutex_lock (&self->appends_lock);
  self->appends_in_flight++;
  g_mutex_unlock (&self->appends_lock);

  if (!gst_element_send_event (GST_ELEMENT_CAST (self->src),
          new_end_of_append_event ())) {
    GST_ERROR_OBJECT (self, "failed to push end-of-append event");
    appends_done (self, FALSE);
    return GST_FLOW_ERROR;
  }
  return GST_FLOW_OK;
}

/* Blocks until every buffer given to gst_append_pipeline_append() has been
 * parsed and its samples handed to the new_sample callback, or until the
 * pipeline shut down, failed or was aborted. Must be called without any lock
 * the new_sample callback takes, or the callback would deadlock against it. */
void
gst_append_pipeline_wait_for_appends (GstAppendPipeline * self)
{
  g_return_if_fail (GST_IS_APPEND_PIPELINE (self));
  g_mutex_lock (&self->appends_lock);
  while (self->appends_in_flight > 0 && !self->appends_abandoned) {
    g_cond_wait (&self->appends_cond, &self->appends_lock);
  }
  g_mutex_unlock (&self->appends_lock);
}

GstFlowReturn
gst_append_pipeline_eos (GstAppendPipeline * self)
{
  g_return_val_if_fail (GST_IS_APPEND_PIPELINE (self), GST_FLOW_ERROR);
  return gst_app_src_end_of_stream (self->src);
}

gboolean
gst_append_pipeline_stop (GstAppendPipeline * self)
{
  g_return_val_if_fail (GST_IS_APPEND_PIPELINE (self), FALSE);

  GstElement *pipeline = GST_ELEMENT (self->pipeline);

  GstStateChangeReturn stopped =
      gst_element_set_state (pipeline, GST_STATE_NULL);
  if (stopped != GST_STATE_CHANGE_SUCCESS) {
    GST_ERROR_OBJECT (self, "failed to stop: %s",
        gst_state_change_return_get_name (stopped));
    return FALSE;
  }
  self->have_demuxer = FALSE;
  self->received_init_segment = FALSE;
  self->encountered_error = FALSE;
  appends_done (self, TRUE);

  return TRUE;
}

gboolean
gst_append_pipeline_reset (GstAppendPipeline * self)
{
  g_return_val_if_fail (GST_IS_APPEND_PIPELINE (self), FALSE);

  GstElement *pipeline = GST_ELEMENT (self->pipeline);

  GstStateChangeReturn stopped =
      gst_element_set_state (pipeline, GST_STATE_READY);
  if (stopped != GST_STATE_CHANGE_SUCCESS) {
    GST_ERROR_OBJECT (self, "failed to stop: %s",
        gst_state_change_return_get_name (stopped));
    return FALSE;
  }

  background_task_stop (self->task);

  renew_appsrc_if_needed (self);

  /* The appsinks of the old tracks stay in the pipeline otherwise, and the
   * next init segment's appsink of the same name cannot be added, so its
   * pad link is refused and add_track() aborts. */
  for (guint i = 0; i < self->tracks->len; i++) {
    Track *track = &g_array_index (self->tracks, Track, i);
    if (track->sink == NULL) {
      continue;
    }
    GstPad *sink_pad = gst_element_get_static_pad (GST_ELEMENT (track->sink),
        "sink");
    if (track->src_pad != NULL && sink_pad != NULL
        && gst_pad_is_linked (sink_pad)) {
      gst_pad_unlink (track->src_pad, sink_pad);
    }
    gst_clear_object (&sink_pad);
    gst_element_set_state (GST_ELEMENT (track->sink), GST_STATE_NULL);
    gst_bin_remove (GST_BIN (self->pipeline), GST_ELEMENT (track->sink));
  }

  /* Retired tracks no initialization segment has taken back stay retired:
   * after two resets in a row, the second finds no tracks, and the next
   * initialization segment must still get the first one's. A track is
   * either taken back or new, so no type and name is here twice. */
  for (guint i = 0; i < self->tracks->len; i++) {
    Track *track = &g_array_index (self->tracks, Track, i);
    if (track->mse_track != NULL) {
      g_ptr_array_add (self->retired_tracks, gst_object_ref (track->mse_track));
    }
  }

  /* Another source buffer's streaming thread may be copying the
   * initialization segment's tracks: swap in empty arrays under the lock
   * and free the old ones after. */
  g_mutex_lock (&self->init_segment_lock);
  InitSegment old_init_segment = self->init_segment;
  init_segment_init (&self->init_segment);
  self->received_init_segment = FALSE;
  g_mutex_unlock (&self->init_segment_lock);
  init_segment_finalize (&old_init_segment);

  gst_clear_object (&self->streams);
  gst_clear_object (&self->reported_streams);
  g_array_free (self->tracks, TRUE);

  /* parsebin removed its elements on the way to READY and plugs a new
   * demuxer for the next initialization segment. */
  self->have_demuxer = FALSE;
  self->have_outstanding_samples = FALSE;
  self->encountered_error = FALSE;
  self->tracks = new_tracks_array ();

  g_mutex_lock (&self->appends_lock);
  self->appends_in_flight = 0;
  self->appends_abandoned = FALSE;
  g_cond_broadcast (&self->appends_cond);
  g_mutex_unlock (&self->appends_lock);

  if (!background_task_start (self->task)) {
    GST_ERROR_OBJECT (self, "failed to start background task");
    return FALSE;
  }

  GstStateChangeReturn started =
      gst_element_set_state (pipeline, GST_STATE_PLAYING);
  if (started == GST_STATE_CHANGE_SUCCESS) {
    return TRUE;
  } else {
    GST_ERROR_OBJECT (self, "failed to start: %s",
        gst_state_change_return_get_name (started));
    return FALSE;
  }
}

gsize
gst_append_pipeline_n_tracks (GstAppendPipeline * self)
{
  g_return_val_if_fail (GST_IS_APPEND_PIPELINE (self), 0);
  return n_tracks (self);
}

gboolean
gst_append_pipeline_has_init_segment (GstAppendPipeline * self)
{
  g_return_val_if_fail (GST_IS_APPEND_PIPELINE (self), FALSE);
  return self->received_init_segment;
}

GstClockTime
gst_append_pipeline_get_duration (GstAppendPipeline * self)
{
  g_return_val_if_fail (GST_IS_APPEND_PIPELINE (self), GST_CLOCK_TIME_NONE);
  if (self->received_init_segment) {
    return self->init_segment.duration;
  } else {
    return GST_CLOCK_TIME_NONE;
  }
}

/* The getters return a copy holding a reference to each track, or NULL
 * before an initialization segment: the caller may be another source
 * buffer's streaming thread, and this buffer's abort() frees the
 * pipeline's own arrays. Free with g_ptr_array_unref(). */
static GPtrArray *
copy_init_segment_tracks (GstAppendPipeline * self, GstStreamType type)
{
  GPtrArray *tracks = NULL;
  g_mutex_lock (&self->init_segment_lock);
  if (self->received_init_segment) {
    tracks = g_ptr_array_copy (init_segment_tracks_for (&self->init_segment,
            type), (GCopyFunc) gst_object_ref, NULL);
    g_ptr_array_set_free_func (tracks, gst_object_unref);
  }
  g_mutex_unlock (&self->init_segment_lock);
  return tracks;
}

GPtrArray *
gst_append_pipeline_get_audio_tracks (GstAppendPipeline * self)
{
  g_return_val_if_fail (GST_IS_APPEND_PIPELINE (self), NULL);
  return copy_init_segment_tracks (self, GST_STREAM_TYPE_AUDIO);
}

GPtrArray *
gst_append_pipeline_get_text_tracks (GstAppendPipeline * self)
{
  g_return_val_if_fail (GST_IS_APPEND_PIPELINE (self), NULL);
  return copy_init_segment_tracks (self, GST_STREAM_TYPE_TEXT);
}

GPtrArray *
gst_append_pipeline_get_video_tracks (GstAppendPipeline * self)
{
  g_return_val_if_fail (GST_IS_APPEND_PIPELINE (self), NULL);
  return copy_init_segment_tracks (self, GST_STREAM_TYPE_VIDEO);
}

gboolean
gst_append_pipeline_get_eos (GstAppendPipeline * self)
{
  g_return_val_if_fail (GST_IS_APPEND_PIPELINE (self), FALSE);
  return gst_task_get_state (self->task->task) != GST_TASK_STARTED;
}

void
gst_append_pipeline_fail (GstAppendPipeline * self)
{
  g_return_if_fail (GST_IS_APPEND_PIPELINE (self));
  gst_bus_post (self->bus, gst_message_new_error (NULL, NULL, NULL));
}

gboolean
gst_append_pipeline_get_failed (GstAppendPipeline * self)
{
  g_return_val_if_fail (GST_IS_APPEND_PIPELINE (self), FALSE);
  return self->encountered_error;
}

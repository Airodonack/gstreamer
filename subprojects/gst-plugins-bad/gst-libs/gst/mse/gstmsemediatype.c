/* GStreamer
 *
 * SPDX-License-Identifier: LGPL-2.1
 *
 * Copyright (C) 2019 Igalia S.L.
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

#include <glib.h>
#include <gst/gst.h>

#include "gstmsemediatype-private.h"

static const gchar tspecials[] = {
  '(', ')', '<', '>', '@', ',', ';', ':',
  '\\', '"', '/', '[', ']', '?', '=',
  0,
};

static gboolean
is_tspecial (const gchar c)
{
  for (const gchar * tspecial = tspecials; *tspecial != 0; tspecial++) {
    if (*tspecial == c) {
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
is_token_character (const gchar c)
{
  if (g_ascii_iscntrl (c) || is_tspecial (c)) {
    return FALSE;
  }
  return g_ascii_isgraph (c);
}

static gboolean
is_eos (const gchar c)
{
  return c == '\0';
}

static void
ignore_whitespace (const gchar ** input)
{
  g_return_if_fail (input != NULL);

  const gchar *output;
  for (output = *input; g_ascii_isspace (*output); output++) {
  }
  *input = output;
}

static gboolean
any_of (const gchar ** input, const gchar ** choices, gchar ** value)
{
  g_return_val_if_fail (input != NULL, FALSE);
  g_return_val_if_fail (choices != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  for (const gchar ** choice = choices; *choice != NULL; choice++) {
    gulong prefix_length = strlen (*choice);
    if (g_str_has_prefix (*input, *choice)) {
      *value = g_strndup (*input, prefix_length);
      *input += prefix_length;
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean
literal (const gchar ** input, const gchar * value)
{
  g_return_val_if_fail (input != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  if (!g_str_has_prefix (*input, value)) {
    return FALSE;
  }
  *input += strlen (value);
  return TRUE;
}

static gboolean
token (const gchar ** input, gchar ** value)
{
  g_return_val_if_fail (input != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  const gchar *unparsed = *input;
  for (; is_token_character (unparsed[0]); unparsed++) {
  }
  gsize length = unparsed - *input;
  if (length < 1) {
    return FALSE;
  }
  *value = g_strndup (*input, length);
  *input = unparsed;
  return TRUE;
}

static gboolean
quoted_string_char (const gchar ** input, gchar * value)
{
  g_return_val_if_fail (input != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  const gchar *unparsed = *input;
  gchar c = unparsed[0];
  if (!g_ascii_isprint (c)) {
    return FALSE;
  }

  if (c == '"' || c == '\\' || c == '\r') {
    return FALSE;
  }

  *value = c;
  (*input)++;
  return TRUE;
}

static gboolean
escaped_ascii_char (const gchar ** input, gchar * value)
{
  g_return_val_if_fail (input != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  if (!literal (input, "\\")) {
    return FALSE;
  }

  gchar c = (*input)[0];
  if (!g_ascii_isprint (c)) {
    return FALSE;
  }

  *value = c;
  (*input)++;
  return TRUE;
}

static gboolean
quoted_string (const gchar ** input, gchar ** value)
{
  g_return_val_if_fail (input != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  const gchar *unparsed = *input;
  if (!literal (&unparsed, "\"")) {
    return FALSE;
  }

  GString *string_value = g_string_new (NULL);
  for (;;) {
    if (literal (&unparsed, "\"")) {
      *input = unparsed;
      *value = g_string_free (string_value, FALSE);
      return TRUE;
    }

    if (is_eos (unparsed[0])) {
      goto error;
    }

    gchar c;

    if (quoted_string_char (&unparsed, &c)) {
      g_string_append_c (string_value, c);
      continue;
    }

    if (escaped_ascii_char (&unparsed, &c)) {
      g_string_append_c (string_value, c);
      continue;
    }

    goto error;
  }
error:
  g_string_free (string_value, TRUE);
  return FALSE;
}

static const gchar *discrete_media_types[] = {
  "text",
  "image",
  "audio",
  "video",
  "application",
  NULL,
};

static gboolean
discrete_media_type (const gchar ** input, gchar ** value)
{
  return any_of (input, discrete_media_types, value);
}

static gboolean
composite_media_type (const gchar ** input, gchar ** value)
{
  return token (input, value);
}

static gboolean
media_type_parameter (const gchar ** input, gchar ** param_name,
    gchar ** param_value)
{
  g_return_val_if_fail (input != NULL, FALSE);
  g_return_val_if_fail (param_name != NULL, FALSE);
  g_return_val_if_fail (param_value != NULL, FALSE);

  ignore_whitespace (input);
  if (!token (input, param_name)) {
    return FALSE;
  }
  if (!literal (input, "=")) {
    return FALSE;
  }
  if (!token (input, param_value)) {
    if (!quoted_string (input, param_value)) {
      return FALSE;
    }
  }
  ignore_whitespace (input);
  return TRUE;
}

static gboolean
media_type (const gchar ** input, gchar ** value)
{
  g_return_val_if_fail (input != NULL, FALSE);
  g_return_val_if_fail (value != NULL, FALSE);

  gchar *discrete = NULL;
  gchar *composite = NULL;

  if (!discrete_media_type (input, &discrete)) {
    goto error;
  }
  if (!literal (input, "/")) {
    goto error;
  }
  if (!composite_media_type (input, &composite)) {
    goto error;
  }

  *value = g_strdup_printf ("%s/%s", discrete, composite);
  g_clear_pointer (&discrete, g_free);
  g_clear_pointer (&composite, g_free);
  return TRUE;

error:
  g_clear_pointer (&discrete, g_free);
  g_clear_pointer (&composite, g_free);
  return FALSE;
}


/* The parameters after the media type, separated by semicolons. Only the
 * codecs parameter is kept; a parameter that does not parse makes the whole
 * type invalid, as a codecs parameter after it could not be found. */
static gboolean
media_type_codecs (const gchar ** input, gchar *** codecs)
{
  g_return_val_if_fail (input != NULL, FALSE);
  g_return_val_if_fail (codecs != NULL, FALSE);

  gchar *codecs_value = NULL;
  do {
    gchar *param_name = NULL;
    gchar *param_value = NULL;
    if (!media_type_parameter (input, &param_name, &param_value)) {
      g_clear_pointer (&codecs_value, g_free);
      return FALSE;
    }
    if (codecs_value == NULL && g_strcmp0 (param_name, "codecs") == 0) {
      codecs_value = g_steal_pointer (&param_value);
    }
    g_clear_pointer (&param_name, g_free);
    g_clear_pointer (&param_value, g_free);
  } while (literal (input, ";"));

  if (!is_eos (*input[0])) {
    g_clear_pointer (&codecs_value, g_free);
    return FALSE;
  }

  if (codecs_value == NULL) {
    return TRUE;
  }

  gchar **codec_names = g_strsplit (codecs_value, ",", 0);
  guint n_codecs = g_strv_length (codec_names);
  GPtrArray *codecs_array = g_ptr_array_new_full (n_codecs, NULL);
  for (guint i = 0; i < n_codecs; i++) {
    gchar *codec = g_strstrip (g_strdup (codec_names[i]));
    if (g_strcmp0 (codec, "") != 0) {
      g_ptr_array_add (codecs_array, g_strdup (codec));
    }
    g_clear_pointer (&codec, g_free);
  }
  g_ptr_array_add (codecs_array, NULL);
  *codecs = (gchar **) g_ptr_array_free (codecs_array, FALSE);

  g_clear_pointer (&codecs_value, g_free);
  g_strfreev (codec_names);

  return TRUE;
}

gboolean
gst_media_source_media_type_parse (GstMediaSourceMediaType * self,
    const gchar * type)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (type != NULL, FALSE);

  gchar *lowercase = g_ascii_strdown (type, -1);
  const gchar *input = lowercase;

  self->mime_type = NULL;
  self->codecs = NULL;

  ignore_whitespace (&input);
  if (!media_type (&input, &self->mime_type)) {
    goto error;
  }
  ignore_whitespace (&input);
  if (is_eos (input[0])) {
    g_free (lowercase);
    return TRUE;
  }
  if (!literal (&input, ";")) {
    goto error;
  }
  if (!media_type_codecs (&input, &self->codecs)) {
    goto error;
  }
  g_free (lowercase);
  return TRUE;

error:
  gst_media_source_media_type_reset (self);
  g_free (lowercase);
  return FALSE;
}

/* The containers a codec can come in, for the byte stream formats below. */
typedef enum
{
  CONTAINER_MP4 = 1 << 0,
  CONTAINER_WEBM = 1 << 1,
} Container;

/* A codec as the codecs parameter names it, after the ISO BMFF and WebM
 * byte stream formats of the Media Source Extensions Byte Stream Format
 * Registry and RFC 6381, and the caps of its elementary stream. A codec with
 * n_fields > 0 is named by its id followed by at least that many
 * dot-separated fields (profile, level, ...); one with none is named by its
 * id alone. The set and the field counts follow what Chrome accepts. */
typedef struct
{
  const gchar *id;
  guint n_fields;
  gboolean is_video;
  Container containers;
  GstStaticCaps caps;
} Codec;

static Codec mse_codecs[] = {
  {"avc1", 1, TRUE, CONTAINER_MP4, GST_STATIC_CAPS ("video/x-h264")},
  {"avc3", 1, TRUE, CONTAINER_MP4, GST_STATIC_CAPS ("video/x-h264")},
  {"hev1", 3, TRUE, CONTAINER_MP4, GST_STATIC_CAPS ("video/x-h265")},
  {"hvc1", 3, TRUE, CONTAINER_MP4, GST_STATIC_CAPS ("video/x-h265")},
  {"av01", 3, TRUE, CONTAINER_MP4 | CONTAINER_WEBM,
      GST_STATIC_CAPS ("video/x-av1")},
  {"vp09", 3, TRUE, CONTAINER_MP4 | CONTAINER_WEBM,
      GST_STATIC_CAPS ("video/x-vp9")},
  {"vp9", 0, TRUE, CONTAINER_WEBM, GST_STATIC_CAPS ("video/x-vp9")},
  {"vp8", 0, TRUE, CONTAINER_WEBM, GST_STATIC_CAPS ("video/x-vp8")},
  /* MPEG-4 AAC: LC, HE (SBR) and HE v2 (PS) audio object types. */
  {"mp4a.40.2", 0, FALSE, CONTAINER_MP4,
      GST_STATIC_CAPS ("audio/mpeg, mpegversion=(int)4")},
  {"mp4a.40.02", 0, FALSE, CONTAINER_MP4,
      GST_STATIC_CAPS ("audio/mpeg, mpegversion=(int)4")},
  {"mp4a.40.5", 0, FALSE, CONTAINER_MP4,
      GST_STATIC_CAPS ("audio/mpeg, mpegversion=(int)4")},
  {"mp4a.40.05", 0, FALSE, CONTAINER_MP4,
      GST_STATIC_CAPS ("audio/mpeg, mpegversion=(int)4")},
  {"mp4a.40.29", 0, FALSE, CONTAINER_MP4,
      GST_STATIC_CAPS ("audio/mpeg, mpegversion=(int)4")},
  /* MPEG-2 AAC: Main, LC and SSR profiles. */
  {"mp4a.66", 0, FALSE, CONTAINER_MP4,
      GST_STATIC_CAPS ("audio/mpeg, mpegversion=(int)2")},
  {"mp4a.67", 0, FALSE, CONTAINER_MP4,
      GST_STATIC_CAPS ("audio/mpeg, mpegversion=(int)2")},
  {"mp4a.68", 0, FALSE, CONTAINER_MP4,
      GST_STATIC_CAPS ("audio/mpeg, mpegversion=(int)2")},
  /* MPEG-2 and MPEG-1 audio, layer 3. */
  {"mp4a.69", 0, FALSE, CONTAINER_MP4,
      GST_STATIC_CAPS ("audio/mpeg, mpegversion=(int)1, layer=(int)3")},
  {"mp4a.6b", 0, FALSE, CONTAINER_MP4,
      GST_STATIC_CAPS ("audio/mpeg, mpegversion=(int)1, layer=(int)3")},
  {"opus", 0, FALSE, CONTAINER_MP4 | CONTAINER_WEBM,
      GST_STATIC_CAPS ("audio/x-opus")},
  {"flac", 0, FALSE, CONTAINER_MP4, GST_STATIC_CAPS ("audio/x-flac")},
  {"vorbis", 0, FALSE, CONTAINER_WEBM, GST_STATIC_CAPS ("audio/x-vorbis")},
};

/* A byte stream format of the registry, with the caps of the bytes. The
 * append pipeline gets those caps when typefinding would hold back the
 * tracks: parsebin needs more than an ISO BMFF initialization segment to
 * decide, while qtdemux parses the moov on its own. WebM is still
 * typefound, so its tracks wait for the first Cluster. The MPEG audio
 * formats have no demuxer, and their parsers take their caps from the
 * first frame, so caps would not bring their tracks earlier. ISO BMFF and
 * WebM may name their codecs; the MPEG audio formats must not, and have no
 * timestamps of their own. */
typedef struct
{
  const gchar *mime_type;
  GstStaticCaps caps;
  gboolean caps_skip_typefinding;
  Container container;
  gboolean takes_video;
  gboolean generates_timestamps;
} ByteStreamFormat;

static ByteStreamFormat byte_stream_formats[] = {
  {"video/mp4", GST_STATIC_CAPS ("video/quicktime, variant=(string)iso"),
      TRUE, CONTAINER_MP4, TRUE, FALSE},
  {"audio/mp4", GST_STATIC_CAPS ("video/quicktime, variant=(string)iso"),
      TRUE, CONTAINER_MP4, FALSE, FALSE},
  {"video/webm", GST_STATIC_CAPS ("video/webm"), FALSE, CONTAINER_WEBM, TRUE,
      FALSE},
  {"audio/webm", GST_STATIC_CAPS ("audio/webm"), FALSE, CONTAINER_WEBM, FALSE,
      FALSE},
  {"audio/mpeg", GST_STATIC_CAPS ("audio/mpeg, mpegversion=(int)1"), FALSE, 0,
      FALSE, TRUE},
  {"audio/aac", GST_STATIC_CAPS ("audio/mpeg, mpegversion=(int)4, "
            "stream-format=(string)adts"), FALSE, 0, FALSE, TRUE},
};

static ByteStreamFormat *
find_byte_stream_format (const gchar * mime_type)
{
  if (mime_type == NULL) {
    return NULL;
  }
  for (guint i = 0; i < G_N_ELEMENTS (byte_stream_formats); i++) {
    if (g_str_equal (byte_stream_formats[i].mime_type, mime_type)) {
      return &byte_stream_formats[i];
    }
  }
  return NULL;
}

static gboolean
has_fields (const gchar * fields, guint n_fields)
{
  gchar **parts = g_strsplit (fields, ".", -1);
  guint n_parts = g_strv_length (parts);
  gboolean valid = n_parts >= n_fields;
  for (guint i = 0; valid && i < n_parts; i++) {
    valid = parts[i][0] != '\0';
    for (const gchar * c = parts[i]; valid && *c != '\0'; c++) {
      valid = g_ascii_isalnum (*c);
    }
  }
  g_strfreev (parts);
  return valid;
}

/* An H.264 codec string carries the profile, constraint flags and level
 * as six hexadecimal digits (RFC 6381, 3.3). */
static gboolean
is_avc_profile_level (const gchar * fields)
{
  if (strlen (fields) != 6) {
    return FALSE;
  }
  for (const gchar * c = fields; *c != '\0'; c++) {
    if (!g_ascii_isxdigit (*c)) {
      return FALSE;
    }
  }
  return TRUE;
}

static Codec *
find_codec (const gchar * name)
{
  for (guint i = 0; i < G_N_ELEMENTS (mse_codecs); i++) {
    Codec *codec = &mse_codecs[i];
    if (codec->n_fields == 0) {
      if (g_str_equal (name, codec->id)) {
        return codec;
      }
      continue;
    }
    gsize id_length = strlen (codec->id);
    if (!g_str_has_prefix (name, codec->id) || name[id_length] != '.') {
      continue;
    }
    const gchar *fields = name + id_length + 1;
    gboolean is_avc = g_str_has_prefix (codec->id, "avc");
    if (is_avc ? is_avc_profile_level (fields) :
        has_fields (fields, codec->n_fields)) {
      return codec;
    }
  }
  return NULL;
}

static gboolean
is_supported (const GstCaps * caps, GList * elements)
{
  GList *supporting_elements = gst_element_factory_list_filter (elements, caps,
      GST_PAD_SINK, FALSE);
  gboolean supported = supporting_elements != NULL;
  gst_plugin_feature_list_free (supporting_elements);
  return supported;
}

static gboolean
is_static_caps_supported (GstStaticCaps * static_caps, GList * elements)
{
  GstCaps *caps = gst_static_caps_get (static_caps);
  gboolean supported = is_supported (caps, elements);
  gst_clear_caps (&caps);
  return supported;
}

static gboolean
is_codec_supported (ByteStreamFormat * format, const gchar * name,
    GList * elements)
{
  Codec *codec = find_codec (name);
  if (codec == NULL) {
    return FALSE;
  }
  if ((codec->containers & format->container) == 0) {
    return FALSE;
  }
  if (codec->is_video && !format->takes_video) {
    return FALSE;
  }
  return is_static_caps_supported (&codec->caps, elements);
}

static inline GList *
get_srcpad_elements (void)
{
  return gst_element_factory_list_get_elements (GST_ELEMENT_FACTORY_TYPE_DEMUXER
      | GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_DECRYPTOR |
      GST_ELEMENT_FACTORY_TYPE_DEPAYLOADER | GST_ELEMENT_FACTORY_TYPE_SINK |
      GST_ELEMENT_FACTORY_TYPE_PARSER, GST_RANK_MARGINAL);
}

gboolean
gst_media_source_media_type_is_caps_supported (const GstCaps * caps)
{
  GList *elements = get_srcpad_elements ();
  gboolean supported = is_supported (caps, elements);
  gst_plugin_feature_list_free (elements);
  return supported;
}

/* A type is supported when it names a byte stream format from the table and
 * every codec it lists belongs to that format, and when elements exist that
 * take the format's bytes and each codec's stream. The codecs parameter may
 * be left out, as addSourceBuffer() callers do, but a codecs parameter that
 * lists nothing names no supported codec. */
gboolean
gst_media_source_media_type_is_supported (GstMediaSourceMediaType * self)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (self->mime_type != NULL, FALSE);

  ByteStreamFormat *format = find_byte_stream_format (self->mime_type);
  if (format == NULL) {
    return FALSE;
  }
  if (self->codecs != NULL) {
    if (format->container == 0 || self->codecs[0] == NULL) {
      return FALSE;
    }
  }

  GList *elements = get_srcpad_elements ();

  gboolean supported = is_static_caps_supported (&format->caps, elements);
  for (gchar ** codec = self->codecs; supported && codec && *codec; codec++) {
    supported = is_codec_supported (format, *codec, elements);
  }

  gst_plugin_feature_list_free (elements);

  return supported;
}

GstCaps *
gst_media_source_media_type_get_caps (GstMediaSourceMediaType * self)
{
  g_return_val_if_fail (self != NULL, NULL);

  ByteStreamFormat *format = find_byte_stream_format (self->mime_type);
  if (format == NULL || !format->caps_skip_typefinding) {
    return NULL;
  }
  return gst_static_caps_get (&format->caps);
}

gboolean
gst_media_source_media_type_generates_timestamp (GstMediaSourceMediaType * self)
{
  g_return_val_if_fail (self != NULL, FALSE);
  g_return_val_if_fail (self->mime_type != NULL, FALSE);

  ByteStreamFormat *format = find_byte_stream_format (self->mime_type);
  return format != NULL && format->generates_timestamps;
}

void
gst_media_source_media_type_reset (GstMediaSourceMediaType * self)
{
  g_return_if_fail (self != NULL);
  g_clear_pointer (&self->mime_type, g_free);
  g_clear_pointer (&self->codecs, g_strfreev);
}

void
gst_media_source_media_type_free (GstMediaSourceMediaType * self)
{
  gst_media_source_media_type_reset (self);
  g_free (self);
}

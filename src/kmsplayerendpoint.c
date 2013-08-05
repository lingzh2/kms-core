#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include "kmselement.h"
#include "kmsagnosticcaps.h"
#include "kmsplayerendpoint.h"

#define PLUGIN_NAME "playerendpoint"
#define AUDIO_APPSRC "audio_appsrc"
#define VIDEO_APPSRC "video_appsrc"
#define URIDECODEBIN "uridecodebin"

GST_DEBUG_CATEGORY_STATIC (kms_player_end_point_debug_category);
#define GST_CAT_DEFAULT kms_player_end_point_debug_category

#define KMS_PLAYER_END_POINT_GET_PRIVATE(obj) ( \
  G_TYPE_INSTANCE_GET_PRIVATE (                 \
    (obj),                                      \
    KMS_TYPE_PLAYER_END_POINT,                  \
    KmsPlayerEndPointPrivate                    \
  )                                             \
)

struct _KmsPlayerEndPointPrivate
{
  GstElement *pipeline;
  GstElement *uridecodebin;
};

/* pad templates */

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (KmsPlayerEndPoint, kms_player_end_point,
    KMS_TYPE_URI_END_POINT,
    GST_DEBUG_CATEGORY_INIT (kms_player_end_point_debug_category, PLUGIN_NAME,
        0, "debug category for playerendpoint element"));

static void
kms_player_end_point_dispose (GObject * object)
{
  KmsPlayerEndPoint *playerendpoint = KMS_PLAYER_END_POINT (object);

  GST_DEBUG_OBJECT (playerendpoint, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (kms_player_end_point_parent_class)->dispose (object);
}

static void
kms_player_end_point_finalize (GObject * object)
{
  KmsPlayerEndPoint *playerendpoint = KMS_PLAYER_END_POINT (object);

  GST_DEBUG_OBJECT (playerendpoint, "finalize");

  /* clean up object here */

  G_OBJECT_CLASS (kms_player_end_point_parent_class)->finalize (object);
}

void
read_buffer (GstElement * appsink, gpointer user_data)
{
  GstElement *appsrc = GST_ELEMENT (user_data);
  GstFlowReturn ret;
  GstSample *sample;
  GstBuffer *buffer;
  GstCaps *caps;

  g_signal_emit_by_name (appsink, "pull-sample", &sample);

  if (sample == NULL) {
    GST_DEBUG_OBJECT (appsrc, "----------------->sample=NULL");
    return;
  }

  g_object_get (G_OBJECT (appsrc), "caps", &caps, NULL);

  if (caps == NULL) {
    /* Appsrc has not yet caps defined */
    caps = gst_sample_get_caps (sample);
    if (caps != NULL)
      g_object_set (appsrc, "caps", caps, NULL);
    else
      GST_ERROR ("No caps found for %s", GST_ELEMENT_NAME (appsrc));
  }

  buffer = gst_sample_get_buffer (sample);

  if (buffer == NULL) {
    return;
  }
  g_signal_emit_by_name (appsrc, "push-buffer", buffer, &ret);

  if (ret != GST_FLOW_OK) {
    /* something wrong */
    GST_ERROR ("Could not send buffer to appsrc. Ret code %d", ret);
  }
}

static void
pad_added (GstElement * element, GstPad * pad, KmsPlayerEndPoint * self)
{
  GST_DEBUG ("Pad added");
  GstElement *appsrc, *agnosticbin, *appsink;
  GstPad *sinkpad;
  GstCaps *audio_caps, *video_caps;
  GstCaps *src_caps;

  /* Create and link appsrc--agnosticbin with proper caps */
  audio_caps = gst_caps_from_string (KMS_AGNOSTIC_AUDIO_CAPS);
  video_caps = gst_caps_from_string (KMS_AGNOSTIC_VIDEO_CAPS);
  src_caps = gst_pad_query_caps (pad, NULL);
  GST_DEBUG ("caps are %" GST_PTR_FORMAT, src_caps);

  if (gst_caps_can_intersect (audio_caps, src_caps)) {

    agnosticbin = kms_element_get_audio_agnosticbin (KMS_ELEMENT (self));
    GST_DEBUG_OBJECT (self, "Linked appsrc_audio--audio_agnosticbin");

  } else if (gst_caps_can_intersect (video_caps, src_caps)) {

    agnosticbin = kms_element_get_video_agnosticbin (KMS_ELEMENT (self));
    GST_DEBUG_OBJECT (self, "Linked appsrc_video--video_agnosticbin");

  } else {

    GST_DEBUG_OBJECT (self, "NOT agnostic caps");
    goto end;
  }

  /* Create appsrc element and link to agnosticbin */
  appsrc = gst_element_factory_make ("appsrc", NULL);
  g_object_set (G_OBJECT (appsrc), "is-live", TRUE, "do-timestamp", TRUE,
      "min-latency", (gint64) 0, "format", GST_FORMAT_TIME, NULL);
  gst_bin_add (GST_BIN (self), appsrc);
  gst_element_sync_state_with_parent (appsrc);
  gst_element_link (appsrc, agnosticbin);

  /* Create appsink and link to pad */
  appsink = gst_element_factory_make ("appsink", NULL);
  g_object_set (appsink, "sync", TRUE, "enable-last-sample",
      FALSE, "emit-signals", TRUE, NULL);
  gst_bin_add (GST_BIN (self->priv->pipeline), appsink);

  sinkpad = gst_element_get_static_pad (appsink, "sink");
  gst_pad_link (pad, sinkpad);
  GST_DEBUG_OBJECT (self, "Linked uridecodebin---appsink");
  g_object_unref (sinkpad);

  /* Connect new-sample signal to callback */
  g_signal_connect (appsink, "new-sample", G_CALLBACK (read_buffer), appsrc);

end:
  if (audio_caps != NULL)
    gst_caps_unref (audio_caps);

  if (video_caps != NULL)
    gst_caps_unref (video_caps);

  if (src_caps != NULL)
    gst_caps_unref (src_caps);
}

static void
pad_removed (GstElement * element, GstPad * pad, gpointer data)
{
  GST_DEBUG ("Pad removed");

  KmsPlayerEndPoint *self = KMS_PLAYER_END_POINT (data);
  GstElement *sink;
  GstPad *peer;

  if (GST_PAD_IS_SINK (pad))
    return;

  peer = gst_pad_get_peer (pad);
  if (peer == NULL)
    return;

  sink = gst_pad_get_parent_element (peer);
  if (sink == NULL) {
    GST_ERROR ("No parent element for pad %s was found",
        GST_ELEMENT_NAME (sink));
    return;
  }

  gst_pad_unlink (pad, peer);

  if (!gst_element_set_locked_state (sink, TRUE))
    GST_ERROR ("Could not block element %s", GST_ELEMENT_NAME (sink));

  GST_DEBUG ("Removing sink %s from %s", GST_ELEMENT_NAME (sink),
      GST_ELEMENT_NAME (self->priv->pipeline));

  gst_element_set_state (sink, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (self->priv->pipeline), sink);

  gst_object_unref (peer);
  g_object_unref (sink);
}

static void
kms_player_end_point_stopped (KmsUriEndPoint * obj)
{
  KmsPlayerEndPoint *self = KMS_PLAYER_END_POINT (obj);

  /* Set internal pipeline to NULL */
  gst_element_set_state (self->priv->pipeline, GST_STATE_NULL);
  GST_DEBUG_OBJECT (self, "---> STOPPED");
}

static void
kms_player_end_point_started (KmsUriEndPoint * obj)
{
  KmsPlayerEndPoint *self = KMS_PLAYER_END_POINT (obj);

  /* Set uri property in uridecodebin */
  g_object_set (G_OBJECT (self->priv->uridecodebin), "uri",
      KMS_URI_END_POINT (self)->uri, NULL);

  /* Set internal pipeline to playing */
  gst_element_set_state (self->priv->pipeline, GST_STATE_PLAYING);

  GST_DEBUG_OBJECT (self, "---> STARTED");
}

static void
kms_player_end_point_paused (KmsUriEndPoint * obj)
{
  KmsPlayerEndPoint *self = KMS_PLAYER_END_POINT (obj);

  /* Set internal pipeline to paused */
  gst_element_set_state (self->priv->pipeline, GST_STATE_PAUSED);
  GST_DEBUG_OBJECT (self, "---> PAUSED");
}

static void
kms_player_end_point_class_init (KmsPlayerEndPointClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  KmsUriEndPointClass *urienpoint_class = KMS_URI_END_POINT_CLASS (klass);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "PlayerEndPoint", "Sink/Generic", "Kurento plugin player end point",
      "Joaquin Mengual García <kini.mengual@gmail.com>");

  gobject_class->dispose = kms_player_end_point_dispose;
  gobject_class->finalize = kms_player_end_point_finalize;

  urienpoint_class->stopped = kms_player_end_point_stopped;
  urienpoint_class->started = kms_player_end_point_started;
  urienpoint_class->paused = kms_player_end_point_paused;

  /* Registers a private structure for the instantiatable type */
  g_type_class_add_private (klass, sizeof (KmsPlayerEndPointPrivate));
}

static void
kms_player_end_point_init (KmsPlayerEndPoint * self)
{
  self->priv = KMS_PLAYER_END_POINT_GET_PRIVATE (self);

  self->priv->pipeline = gst_pipeline_new ("pipeline");
  self->priv->uridecodebin =
      gst_element_factory_make ("uridecodebin", URIDECODEBIN);

  gst_bin_add (GST_BIN (self->priv->pipeline), self->priv->uridecodebin);

  /* Connect to signals */
  g_signal_connect (self->priv->uridecodebin, "pad-added",
      G_CALLBACK (pad_added), self);
  g_signal_connect (self->priv->uridecodebin, "pad-removed",
      G_CALLBACK (pad_removed), self);
}

gboolean
kms_player_end_point_plugin_init (GstPlugin * plugin)
{

  return gst_element_register (plugin, PLUGIN_NAME, GST_RANK_NONE,
      KMS_TYPE_PLAYER_END_POINT);
}
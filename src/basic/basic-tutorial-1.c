#include <gst/gst.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

int
tutorial_main1 (int argc, char *argv[])
{
  GstElement *pipeline;
  GstBus *bus;
  GstMessage *msg;

  /* Initialize GStreamer */
  gst_init (&argc, &argv);

  /* Build the pipeline */
  pipeline =
      gst_parse_launch
      ("playbin uri=https://gstreamer.freedesktop.org/data/media/sintel_trailer-480p.webm",
      NULL);

  /* Start playing */
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  /* Wait until error or EOS */
  bus = gst_element_get_bus (pipeline);
  msg =
      gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE,
      GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

  /* See next tutorial for proper error message handling/parsing */
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    g_printerr ("An error occurred! Re-run with the GST_DEBUG=*:WARN "
        "environment variable set for more details.\n");
  }

  /* Free resources */
  gst_message_unref (msg);
  gst_object_unref (bus);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
  return 0;
}

int test1()
{
#ifdef __APPLE__
  return gst_macos_main ((GstMainFunc) tutorial_main1, 0, NULL, NULL);
#else
  return tutorial_main1 (0, NULL);
#endif
}

static gboolean
handle_keyboard (GIOChannel *source, GIOCondition cond, gpointer data)
{
  gchar *str = NULL;
  if (g_io_channel_read_line (source, &str, NULL, NULL, NULL) == G_IO_STATUS_NORMAL) {
    if (g_strrstr (str, "q")) {
      g_main_loop_quit ((GMainLoop *)data);
    }
    g_free (str);
  }
  return TRUE;
}

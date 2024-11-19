// audio_player2.c

#include "audio_player2.h"
#include "audio_demux2.h"  // Include the header for audio_demux2 element

static void on_pad_added(GstElement *element, GstPad *pad, gpointer data) {
    g_print("Pad added to demuxer.\n");
    GstElement *decoder = GST_ELEMENT(data);
    GstPad *sink_pad = gst_element_get_static_pad(decoder, "sink");

    if (gst_pad_is_linked(sink_pad)) {
        gst_object_unref(sink_pad);
        return;
    }

    GstPadLinkReturn ret = gst_pad_link(pad, sink_pad);
    if (GST_PAD_LINK_FAILED(ret)) {
        g_printerr("Failed to link dynamic pad to decoder sink pad.\n");
    } else {
        g_print("Dynamic pad linked to decoder sink pad.\n");
    }

    gst_object_unref(sink_pad);
}

void audio_player2(const gchar *file_path) {
    GstElement *pipeline, *demuxer, *decoder, *converter, *sink;
    GstBus *bus;
    GstMessage *msg;
    GstStateChangeReturn ret;

    /* Initialize GStreamer */
    gst_init(NULL, NULL);
    audio_demux2_plugin_init(NULL);

    /* Create the elements */
    pipeline = gst_pipeline_new("audio-player2");
    demuxer = gst_element_factory_make("audio_demux2", "demuxer");
    decoder = gst_element_factory_make("avdec_aac", "decoder");
    converter = gst_element_factory_make("audioconvert", "converter");
    sink = gst_element_factory_make("autoaudiosink", "audio-output");

    if (!pipeline || !demuxer || !decoder || !converter || !sink) {
        g_printerr("Not all elements could be created.\n");
        return;
    }

    /* Set the input file */
    g_object_set(demuxer, "location", file_path, NULL);

    /* Build the pipeline */
    gst_bin_add_many(GST_BIN(pipeline), demuxer, decoder, converter, sink, NULL);

    /* Link the decoder to the converter and sink (demuxer will be linked dynamically) */
    if (!gst_element_link_many(decoder, converter, sink, NULL)) {
        g_printerr("Elements could not be linked.\n");
        gst_object_unref(pipeline);
        return;
    }

    /* Connect to the pad-added signal */
    g_signal_connect(demuxer, "pad-added", G_CALLBACK(on_pad_added), decoder);

    /* Start playing */
    ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Unable to set the pipeline to the playing state.\n");
        gst_object_unref(pipeline);
        return;
    }

    /* Listen to the bus */
    bus = gst_element_get_bus(pipeline);
    gboolean terminate = FALSE;
    while (!terminate) {
        msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                                         GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

        if (msg != NULL) {
            GError *err;
            gchar *debug_info;

            switch (GST_MESSAGE_TYPE(msg)) {
                case GST_MESSAGE_ERROR:
                    gst_message_parse_error(msg, &err, &debug_info);
                    g_printerr("Error received from element %s: %s\n",
                               GST_OBJECT_NAME(msg->src), err->message);
                    g_printerr("Debugging information: %s\n",
                               debug_info ? debug_info : "none");
                    g_clear_error(&err);
                    g_free(debug_info);
                    terminate = TRUE;
                    break;

                case GST_MESSAGE_EOS:
                    g_print("End-Of-Stream reached.\n");
                    terminate = TRUE;
                    break;

                default:
                    /* We should not reach here */
                    g_printerr("Unexpected message received.\n");
                    break;
            }
            gst_message_unref(msg);
        }
    }

    /* Free resources */
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
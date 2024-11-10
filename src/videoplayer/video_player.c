#include <gst/gst.h>
#include "my_demux.h"
#include "video_player.h"
// Plugin initialization function declaration (must be consistent with the definition in my_demux.c)
extern gboolean plugin_init(GstPlugin *plugin);

void on_pad_added(GstElement *element, GstPad *pad, gpointer data);

#define FILE_PATH "/Users/lizhen/Downloads/test.mp4"
// Define your own main function
int video_player(const char *file_path) {
    GstElement *pipeline, *demuxer, *h264parse, *decoder, *video_sink;
    GstBus *bus;
    GstMessage *msg;

    gst_init(NULL, NULL);
    plugin_init(NULL);

    g_print("Entering my_main function.\n");

    // Create GStreamer elements
    pipeline = gst_pipeline_new("mp4-player");
    demuxer = gst_element_factory_make("mydemux", "demuxer");
    h264parse = gst_element_factory_make("h264parse", "h264parse");
    decoder = gst_element_factory_make("avdec_h264", "decoder");
    video_sink = gst_element_factory_make("glimagesink", "video_sink");

    if (!pipeline || !demuxer || !h264parse || !decoder || !video_sink) {
        g_printerr("Failed to create one or more GStreamer elements.\n");
        return -1;
    }

    // Set input file path for the demuxer
    g_object_set(G_OBJECT(demuxer), "location", FILE_PATH, NULL);

    // Add elements to the pipeline
    gst_bin_add_many(GST_BIN(pipeline), demuxer, h264parse, decoder, video_sink, NULL);

    // Link h264parse to decoder and video sink
    if (!gst_element_link_many(h264parse, decoder, video_sink, NULL)) {
        g_printerr("Failed to link h264parse, decoder, and video sink.\n");
        gst_object_unref(pipeline);
        return -1;
    }

    g_print("Link h264parse, decoder, and video sink.\n");
    // Handle demuxer's dynamic pad
    g_signal_connect(demuxer, "pad-added", G_CALLBACK(on_pad_added), h264parse);

    g_print("Connect demuxer's pad-added signal to on_pad_added function.\n");

    // Start playback
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    g_print("start playing.\n");

    // Get the message bus
    bus = gst_element_get_bus(pipeline);
    msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

    // Handle messages
    if (msg != NULL) {
        GError *err;
        gchar *debug_info;

        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR:
            gst_message_parse_error(msg, &err, &debug_info);
            g_printerr("Error: %s\n", err->message);
            g_error_free(err);
            g_free(debug_info);
            break;
        case GST_MESSAGE_EOS:
            g_print("Playback finished\n");
            break;
        default:
            g_printerr("Unknown message type\n");
            break;
        }
        gst_message_unref(msg);
    }

    // Clean up
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    return 0;
}

// The on_pad_added function
void on_pad_added(GstElement *element, GstPad *pad, gpointer data) {
    g_print("on_pad_added function is called for pad: %s\n", GST_PAD_NAME(pad));

    GstElement *h264parse = (GstElement *)data;
    GstPad *sink_pad = gst_element_get_static_pad(h264parse, "sink");

    if (gst_pad_is_linked(sink_pad)) {
        g_print("Sink pad is already linked. Ignoring.\n");
        g_object_unref(sink_pad);
        return;
    }

    GstCaps *new_pad_caps = gst_pad_get_current_caps(pad);
    if (!new_pad_caps) {
        g_printerr("Failed to get caps for pad: %s\n", GST_PAD_NAME(pad));
        g_object_unref(sink_pad);
        return;
    }

    GstStructure *new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
    if (!new_pad_struct) {
        g_printerr("Failed to get structure for pad: %s\n", GST_PAD_NAME(pad));
        gst_caps_unref(new_pad_caps);
        g_object_unref(sink_pad);
        return;
    }

    const gchar *new_pad_type = gst_structure_get_name(new_pad_struct);
    if (!new_pad_type || !g_str_has_prefix(new_pad_type, "video/x-h264")) {
        g_print("Pad type %s is not video/x-h264. Ignoring.\n", new_pad_type ? new_pad_type : "(null)");
        gst_caps_unref(new_pad_caps);
        g_object_unref(sink_pad);
        return;
    }

    g_print("Caps for pad %s: %s\n", GST_PAD_NAME(pad), gst_caps_to_string(new_pad_caps));

    g_print("Linking video pad: %s to sink pad.\n", GST_PAD_NAME(pad));
    if (gst_pad_link(pad, sink_pad) != GST_PAD_LINK_OK) {
        g_printerr("Failed to link video pad: %s to sink pad.\n", GST_PAD_NAME(pad));
    } else {
        g_print("Successfully linked video pad: %s to sink pad.\n", GST_PAD_NAME(pad));
    }

    gst_caps_unref(new_pad_caps);
    g_object_unref(sink_pad);
}


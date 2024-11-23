#include <gst/gst.h>
#include "media_demux.h"

#define FILE_PATH "/Users/lizhen/Downloads/test.mp4"

/* Bus message handler */
static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    GMainLoop *loop = (GMainLoop *)data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(loop);
            break;
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);
            g_printerr("Error from element %s: %s\n", GST_OBJECT_NAME(msg->src), error->message);
            g_printerr("Debugging info: %s\n", debug ? debug : "none");
            g_clear_error(&error);
            g_free(debug);
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
            g_print("Element %s changed state from %s to %s.\n",
                    GST_OBJECT_NAME(msg->src),
                    gst_element_state_get_name(old_state),
                    gst_element_state_get_name(new_state));
            break;
        }
        default:
            break;
    }
    return TRUE;
}

/* Unified pad added signal handler */
static void on_pad_added(GstElement *element, GstPad *pad, gpointer data) {
    g_print("##########on_pad_added#########\n");
    GstElement *audio_queue = ((GstElement **)data)[0];
    GstElement *video_queue = ((GstElement **)data)[1];
    GstPad *sink_pad = NULL;

    GstCaps *new_pad_caps = gst_pad_get_current_caps(pad);
    GstStructure *new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
    const gchar *new_pad_type = gst_structure_get_name(new_pad_struct);

    if (g_str_has_prefix(new_pad_type, "audio/")) {
        sink_pad = gst_element_get_static_pad(audio_queue, "sink");
    } else if (g_str_has_prefix(new_pad_type, "video/")) {
        sink_pad = gst_element_get_static_pad(video_queue, "sink");
    }

    if (sink_pad && !gst_pad_is_linked(sink_pad)) {
        if (gst_pad_link(pad, sink_pad) != GST_PAD_LINK_OK) {
            g_printerr("Failed to link pad of type %s.\n", new_pad_type);
        } else {
            g_print("%s pad linked successfully.\n", new_pad_type);
        }
    } else {
        g_print("Unsupported or already linked pad type: %s\n", new_pad_type);
    }

    if (sink_pad) {
        gst_object_unref(sink_pad);
    }
    gst_caps_unref(new_pad_caps);
}

int media_player(const char *file_path) {
    GMainLoop *loop;
    GstElement *pipeline, *demux, *audio_queue, *audio_decoder, *audio_convert, *audio_sink, *video_queue, *h264parse, *video_decoder, *video_sink;
    GstBus *bus;
    guint bus_watch_id;

    /* Initialize GStreamer */
    gst_init(NULL, NULL);
    media_demux_plugin_init(NULL); /* Ensure the plugin is registered */

    /* Create main loop */
    loop = g_main_loop_new(NULL, FALSE);

    /* Create elements */
    pipeline = gst_pipeline_new("media-player");
    demux = gst_element_factory_make("media_demux", "demux");
    audio_queue = gst_element_factory_make("queue", "audio-queue");
    audio_decoder = gst_element_factory_make("avdec_aac", "audio-decoder");
    audio_convert = gst_element_factory_make("audioconvert", "audio-convert");
    audio_sink = gst_element_factory_make("autoaudiosink", "audio-output");
    video_queue = gst_element_factory_make("queue", "video-queue");
    h264parse = gst_element_factory_make("h264parse", "h264-parse");
    video_decoder = gst_element_factory_make("avdec_h264", "video-decoder");
    video_sink = gst_element_factory_make("glimagesink", "video-output");

    if (!pipeline || !demux || !audio_queue || !audio_decoder || !audio_convert || !audio_sink || !video_queue || !h264parse || !video_decoder || !video_sink) {
        g_printerr("Failed to create GStreamer elements.\n");
        return -1;
    }

    /* Set the file path property */
    g_object_set(G_OBJECT(demux), "location", file_path, NULL);

    /* Add elements to the pipeline */
    gst_bin_add_many(GST_BIN(pipeline), demux, audio_queue, audio_decoder, audio_convert, audio_sink, video_queue, h264parse, video_decoder, video_sink, NULL);

    /* Link audio elements */
    if (!gst_element_link_many(audio_queue, audio_decoder, audio_convert, audio_sink, NULL)) {
        g_printerr("Failed to link audio elements.\n");
        gst_object_unref(pipeline);
        return -1;
    }

    /* Link video elements */
    if (!gst_element_link_many(video_queue, h264parse, video_decoder, video_sink, NULL)) {
        g_printerr("Failed to link video elements.\n");
        gst_object_unref(pipeline);
        return -1;
    }

    /* Connect pad-added signal */
    GstElement *elements[] = {audio_queue, video_queue};
    g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), elements);

    /* Set the pipeline to playing state */
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    /* Add a bus watch to handle messages */
    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    bus_watch_id = gst_bus_add_watch(bus, bus_call, loop);
    gst_object_unref(bus);

    /* Start the main loop */
    g_print("Starting playback: %s\n", file_path);
    g_main_loop_run(loop);

    /* Cleanup after playback */
    g_print("Stopping playback\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);

    gst_object_unref(pipeline);
    g_source_remove(bus_watch_id);
    g_main_loop_unref(loop);

    return 0;
}

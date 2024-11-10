#include <gst/gst.h>
#include "media_demux.h"

#define FILE_PATH "/Users/lizhen/Downloads/test.mp4"

static void on_pad_added(GstElement *src, GstPad *new_pad, gpointer user_data) {
    GstElement *pipeline = GST_ELEMENT(user_data);
    GstPad *sink_pad = NULL;
    GstCaps *new_pad_caps = NULL;
    GstStructure *new_pad_struct = NULL;
    const gchar *new_pad_type = NULL;

    // 获取新 Pad 的 Caps
    new_pad_caps = gst_pad_get_current_caps(new_pad);
    if (!new_pad_caps) {
        g_print("New pad has no caps set.\n");
        return;
    }

    new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
    new_pad_type = gst_structure_get_name(new_pad_struct);

    g_print("New pad type: %s\n", new_pad_type);
    g_print("Source pad caps: %s\n", gst_caps_to_string(new_pad_caps));

    if (g_str_has_prefix(new_pad_type, "audio/x-aac")) {
        // 链接到音频分支
        GstElement *audio_decoder = gst_bin_get_by_name(GST_BIN(pipeline), "audio-decoder");
        if (audio_decoder) {
            sink_pad = gst_element_get_static_pad(audio_decoder, "sink");
            if (sink_pad) {
                GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
                if (ret != GST_PAD_LINK_OK) {
                    g_printerr("Failed to link new pad to audio decoder sink pad.\n");
                } else {
                    g_print("#1 Successfully linked audio decoder.\n");
                }
                gst_object_unref(sink_pad);
            }
            gst_object_unref(audio_decoder);
        }
    } else if (g_str_has_prefix(new_pad_type, "video/x-h264")) {
        // 链接到视频分支
        GstElement *h264parse = gst_bin_get_by_name(GST_BIN(pipeline), "h264parse");
        if (h264parse) {
            sink_pad = gst_element_get_static_pad(h264parse, "sink");
            if (sink_pad) {
                GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
                if (ret != GST_PAD_LINK_OK) {
                    g_printerr("Failed to link new pad to h264parse sink pad.\n");
                } else {
                    g_print("#2 Successfully linked h264parse.\n");
                }
                gst_object_unref(sink_pad);
            }
            gst_object_unref(h264parse);
        }
    } else {
        g_print("Unsupported pad type: %s\n", new_pad_type);
    }

    gst_caps_unref(new_pad_caps);
}

int media_player(const char *file_path){
    GstElement *pipeline, *demux, *audio_decoder, *audio_convert, *h264parse, *video_decoder, *audio_sink, *video_sink;
    GstBus *bus;
    GstMessage *msg;

    gst_init(NULL, NULL);
    media_demux_plugin_init(NULL); // 确保插件已注册

    // 创建元素
    pipeline = gst_pipeline_new("media-pipeline");
    demux = gst_element_factory_make("media_demux", "demux");
    audio_decoder = gst_element_factory_make("avdec_aac", "audio-decoder");
    audio_convert = gst_element_factory_make("audioconvert", "audio-convert");
    h264parse = gst_element_factory_make("h264parse", "h264parse");
    video_decoder = gst_element_factory_make("avdec_h264", "video-decoder");
    audio_sink = gst_element_factory_make("autoaudiosink", "audio-output");
    video_sink = gst_element_factory_make("glimagesink", "video-output");

    if (!pipeline || !demux || !audio_decoder || !audio_convert || !h264parse || !video_decoder || !audio_sink || !video_sink) {
        g_printerr("Not all elements could be created.\n");
        return -1;
    }

    // 设置 location 属性
    g_object_set(G_OBJECT(demux), "location", FILE_PATH, NULL);

    // 构建管道
    gst_bin_add_many(GST_BIN(pipeline), demux, audio_decoder, audio_convert, h264parse, video_decoder, audio_sink, video_sink, NULL);

    // 链接音频和视频分支
    if (!gst_element_link_many(audio_decoder, audio_convert, audio_sink, NULL)) {
        g_printerr("Failed to link audio branch.\n");
        gst_object_unref(pipeline);
        return -1;
    }

    if (!gst_element_link_many(h264parse, video_decoder, video_sink, NULL)) {
        g_printerr("Failed to link video branch.\n");
        gst_object_unref(pipeline);
        return -1;
    }

    // 连接 demux 的 pad-added 信号
    g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), pipeline);

    // 运行管道
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // 监听管道消息
    bus = gst_element_get_bus(pipeline);
    msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

    // 处理消息
    if (msg != NULL) {
        GError *err;
        gchar *debug_info;

        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR:
                gst_message_parse_error(msg, &err, &debug_info);
                g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
                g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
                g_clear_error(&err);
                g_free(debug_info);
                break;
            case GST_MESSAGE_EOS:
                g_print("End-Of-Stream reached.\n");
                break;
            default:
                // 其他消息类型
                break;
        }
        gst_message_unref(msg);
    }

    // 清理
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 0;
}
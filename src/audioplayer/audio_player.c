    #include "audio_player.h"
    #include "audio_demux.h"
    #include <gst/gst.h>
    
    
    void audio_player(const char *file_path) {
        GstElement *pipeline, *demuxer, *decoder, *audioconvert, *audio_sink;
        GstBus *bus;
        GstMessage *msg;
    
        gst_init(NULL, NULL);
        audio_demux_plugin_init(NULL);
    
        // 创建 GStreamer 元素
        pipeline = gst_pipeline_new("audio-player");
        demuxer = gst_element_factory_make("audio_demux", "demuxer");
        decoder = gst_element_factory_make("avdec_aac", "decoder");
        audioconvert = gst_element_factory_make("audioconvert", "audioconvert");
        audio_sink = gst_element_factory_make("autoaudiosink", "audio_sink");
    
        if (!pipeline || !demuxer || !decoder || !audioconvert || !audio_sink) {
            g_printerr("Failed to create GStreamer elements.\n");
            return;
        }
    
        // 设置输入文件路径
        g_object_set(G_OBJECT(demuxer), "location", file_path, NULL);
    
        // 将元素添加到管道中
        gst_bin_add_many(GST_BIN(pipeline), demuxer, decoder, audioconvert, audio_sink, NULL);
    
        // 链接 demuxer 到 decoder，再到 audioconvert，再到 audio_sink
        if (!gst_element_link_many(demuxer, decoder, audioconvert, audio_sink, NULL)) {
            g_printerr("Failed to link demuxer, decoder, audioconvert, and audio sink.\n");
            gst_object_unref(pipeline);
            return;
        }
    
        // 设置管道状态为播放
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    
        // 监听消息总线
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
                    g_printerr("Unexpected message received.\n");
                    break;
            }
            gst_message_unref(msg);
        }
    
        // 清理
        gst_object_unref(bus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }

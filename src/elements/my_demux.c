#define PACKAGE "my_plugin"
#include "my_demux.h"
#include <gst/gst.h>
#include <libavformat/avformat.h>

G_DEFINE_TYPE(MyDemux, my_demux, GST_TYPE_ELEMENT)

static GstStateChangeReturn my_demux_change_state(GstElement *element, GstStateChange transition);
static void my_demux_loop(MyDemux *demux);
static GstFlowReturn my_demux_push_data(MyDemux *demux);

// 定义视频的动态 Pad 模板 (音频流被忽略)
static GstStaticPadTemplate video_src_template = GST_STATIC_PAD_TEMPLATE(
    "video_src_%u", GST_PAD_SRC, GST_PAD_SOMETIMES,
    GST_STATIC_CAPS("video/x-h264"));

enum {
    PROP_0,
    PROP_LOCATION,
};

// 设置和获取属性的函数
static void my_demux_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void my_demux_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);

static void my_demux_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
    MyDemux *demux = MY_DEMUX(object);

    switch (prop_id) {
        case PROP_LOCATION:
            if (demux->location) {
                g_free(demux->location);
            }
            demux->location = g_value_dup_string(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void my_demux_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    MyDemux *demux = MY_DEMUX(object);

    switch (prop_id) {
        case PROP_LOCATION:
            g_value_set_string(value, demux->location);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void my_demux_class_init(MyDemuxClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

    gobject_class->set_property = my_demux_set_property;
    gobject_class->get_property = my_demux_get_property;

    // 注册 location 属性
    g_object_class_install_property(gobject_class, PROP_LOCATION,
        g_param_spec_string("location", "Location", "File path to open", NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    gst_element_class_set_static_metadata(element_class,
        "MyDemux", "Demuxer",
        "Custom FFmpeg-based Demuxer",
        "Your Name <your.email@example.com>");

    // 添加视频的动态 pad 模板
    gst_element_class_add_static_pad_template(element_class, &video_src_template);

    element_class->change_state = GST_DEBUG_FUNCPTR(my_demux_change_state);
}

static void my_demux_init(MyDemux *demux) {
    demux->fmt_ctx = NULL;
    demux->started = FALSE;
    demux->video_stream_idx = -1;
    demux->video_src_pad = NULL;
    demux->task = NULL;
    demux->task_lock = NULL;
    demux->location = NULL;
}

static void my_demux_finalize(GObject *object) {
    MyDemux *demux = MY_DEMUX(object);

    if (demux->task) {
        gst_task_stop(demux->task);
        gst_task_join(demux->task);
        g_object_unref(demux->task);
        demux->task = NULL;
    }

    if (demux->task_lock) {
        g_rec_mutex_clear(demux->task_lock);
        g_free(demux->task_lock);
        demux->task_lock = NULL;
    }

    if (demux->location) {
        g_free(demux->location);
        demux->location = NULL;
    }

    if (demux->fmt_ctx) {
        avformat_close_input(&demux->fmt_ctx);
        demux->fmt_ctx = NULL;
    }

    G_OBJECT_CLASS(my_demux_parent_class)->finalize(object);
}

// 提取 codec_data 并传递给下游元素
static gboolean my_demux_start(MyDemux *demux) {
    if (!demux->location) {
        GST_ERROR_OBJECT(demux, "No file location specified.");
        return FALSE;
    }

    if (avformat_open_input(&demux->fmt_ctx, demux->location, NULL, NULL) < 0) {
        GST_ERROR_OBJECT(demux, "Failed to open file at location: %s", demux->location);
        return FALSE;
    }

    if (avformat_find_stream_info(demux->fmt_ctx, NULL) < 0) {
        GST_ERROR_OBJECT(demux, "Failed to find stream info.");
        return FALSE;
    }

    // 只处理视频流
    for (gint i = 0; i < demux->fmt_ctx->nb_streams; i++) {
        AVStream *stream = demux->fmt_ctx->streams[i];
        gchar *stream_id = g_strdup_printf("stream-%d", i);

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && demux->video_stream_idx == -1) {
            demux->video_stream_idx = i;

            demux->video_src_pad = gst_pad_new_from_static_template(&video_src_template, "video_src_%u");
            gst_element_add_pad(GST_ELEMENT(demux), demux->video_src_pad);

            // 设置 caps 和 codec_data
            GstCaps *video_caps = NULL;

            if (stream->codecpar->extradata_size > 0 && stream->codecpar->extradata != NULL) {
                GstBuffer *codec_data = gst_buffer_new_allocate(NULL, stream->codecpar->extradata_size, NULL);
                gst_buffer_fill(codec_data, 0, stream->codecpar->extradata, stream->codecpar->extradata_size);

                video_caps = gst_caps_new_simple("video/x-h264",
                                                 "stream-format", G_TYPE_STRING, "avc",
                                                 "alignment", G_TYPE_STRING, "au",
                                                 "width", G_TYPE_INT, stream->codecpar->width,
                                                 "height", G_TYPE_INT, stream->codecpar->height,
                                                 "codec_data", GST_TYPE_BUFFER, codec_data,
                                                 NULL);
                gst_buffer_unref(codec_data);
            } else {
                video_caps = gst_caps_new_simple("video/x-h264",
                                                 "stream-format", G_TYPE_STRING, "avc",
                                                 "alignment", G_TYPE_STRING, "au",
                                                 "width", G_TYPE_INT, stream->codecpar->width,
                                                 "height", G_TYPE_INT, stream->codecpar->height,
                                                 NULL);
            }

            // 设置 caps
            gst_pad_set_caps(demux->video_src_pad, video_caps);
            gst_caps_unref(video_caps);

            // 发送 stream-start 事件
            GstEvent *stream_start_event = gst_event_new_stream_start(stream_id);
            gst_pad_push_event(demux->video_src_pad, stream_start_event);

            // 发送 segment 事件
            GstSegment segment;
            gst_segment_init(&segment, GST_FORMAT_TIME);
            segment.start = 0;
            segment.time = 0;
            GstEvent *segment_event = gst_event_new_segment(&segment);
            gst_pad_push_event(demux->video_src_pad, segment_event);

            // 发送 pad-added 信号
            g_signal_emit_by_name(GST_ELEMENT(demux), "pad-added", demux->video_src_pad);

            g_free(stream_id);  // 释放 stream_id

            break; // 找到一个视频流即可，跳出循环
        }

        g_free(stream_id);
    }

    if (demux->video_stream_idx == -1) {
        GST_ERROR_OBJECT(demux, "No valid video streams found.");
        return FALSE;
    }

    demux->started = TRUE;

    // 在这里初始化任务
    if (!demux->task) {
        demux->task = gst_task_new((GstTaskFunction)my_demux_loop, demux, NULL);
        demux->task_lock = g_new(GRecMutex, 1);
        g_rec_mutex_init(demux->task_lock);
        gst_task_set_lock(demux->task, demux->task_lock);
    }

    return TRUE;
}

static gboolean my_demux_stop(MyDemux *demux) {
    demux->started = FALSE;

    if (demux->task) {
        gst_task_stop(demux->task);
        gst_task_join(demux->task);
    }

    if (demux->fmt_ctx) {
        avformat_close_input(&demux->fmt_ctx);
        demux->fmt_ctx = NULL;
    }

    return TRUE;
}

static GstFlowReturn my_demux_push_data(MyDemux *demux) {
    AVPacket pkt;

    // 获取当前处理的 AVStream
    AVStream *stream = demux->fmt_ctx->streams[demux->video_stream_idx];

    // 读取一帧数据
    if (av_read_frame(demux->fmt_ctx, &pkt) >= 0) {
        if (pkt.stream_index == demux->video_stream_idx) {
            GstBuffer *buffer = gst_buffer_new_allocate(NULL, pkt.size, NULL);
            gst_buffer_fill(buffer, 0, pkt.data, pkt.size);

            // 设置 buffer 的 PTS、DTS 和时长
            if (pkt.pts != AV_NOPTS_VALUE) {
                GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(pkt.pts, GST_SECOND * stream->time_base.num, stream->time_base.den);
            }
            if (pkt.dts != AV_NOPTS_VALUE) {
                GST_BUFFER_DTS(buffer) = gst_util_uint64_scale(pkt.dts, GST_SECOND * stream->time_base.num, stream->time_base.den);
            }
            if (pkt.duration > 0) {
                GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(pkt.duration, GST_SECOND * stream->time_base.num, stream->time_base.den);
            }

            GstFlowReturn ret = gst_pad_push(demux->video_src_pad, buffer);
            av_packet_unref(&pkt);
            return ret;
        }

        av_packet_unref(&pkt);
    } else {
        // 读取结束，发送 EOS
        gst_pad_push_event(demux->video_src_pad, gst_event_new_eos());
        return GST_FLOW_EOS;
    }

    return GST_FLOW_OK;
}

static void my_demux_loop(MyDemux *demux) {
    while (demux->started) {
        GstFlowReturn ret = my_demux_push_data(demux);
        if (ret != GST_FLOW_OK) {
            // 处理错误或 EOS
            break;
        }
        // 控制读取速度
        g_usleep(10000);
    }
}

static GstStateChangeReturn my_demux_change_state(GstElement *element, GstStateChange transition) {
    g_print("Entering my_demux_change_state function.\n");
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
    MyDemux *demux = MY_DEMUX(element);

    g_print("transition: %d\n", transition);

    switch (transition) {
        case GST_STATE_CHANGE_NULL_TO_READY:
            // 初始化 FFmpeg 库（如果尚未初始化）
            avformat_network_init();
            break;

        case GST_STATE_CHANGE_READY_TO_PAUSED:
            g_print("Switching from READY to PAUSED\n");
            if (!my_demux_start(demux)) {
                g_print("Failed to start demuxer.\n");
                return GST_STATE_CHANGE_FAILURE;
            }
            // 在 PAUSED 状态启动任务，以便在 PAUSED 状态下推送数据
            gst_task_start(demux->task);
            break;

        case GST_STATE_CHANGE_PAUSED_TO_READY:
            g_print("Switching from PAUSED to READY\n");
            my_demux_stop(demux);
            break;

        default:
            break;
    }

    ret = GST_ELEMENT_CLASS(my_demux_parent_class)->change_state(element, transition);

    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_print("State change failed.\n");
    } else {
        g_print("State change successful.\n");
    }

    return ret;
}

gboolean plugin_init(GstPlugin *plugin) {
    return gst_element_register(plugin, "mydemux", GST_RANK_NONE, MY_TYPE_DEMUX);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    mydemux,
    "Custom FFmpeg-based Demuxer",
    plugin_init,
    "1.0",
    "LGPL",
    PACKAGE,
    "http://gstreamer.net/"
)
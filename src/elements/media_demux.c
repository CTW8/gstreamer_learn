#define PACKAGE "mediadmux"
#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>
#include <gst/audio/audio.h>
#include <gst/video/video.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/timestamp.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
#include <libavutil/pixdesc.h> // 添加此行
#include "media_demux.h" // 确保此头文件正确定义

typedef struct _MediaDemuxPrivate {
    AVFormatContext *fmt_ctx;
    GList *video_src_pads; // 支持多个视频 Pad
    GList *audio_src_pads; // 支持多个音频 Pad
    gboolean started;
    GstTask *task;
    GRecMutex *task_lock;
    gchar *location;
} MediaDemuxPrivate;

typedef struct _MediaDemux {
    GstElement parent;
    // 不需要显式地定义 MediaDemuxPrivate *priv;
} MediaDemux;

G_DEFINE_TYPE_WITH_PRIVATE(MediaDemux, media_demux, GST_TYPE_ELEMENT);

// 定义属性枚举
enum {
    PROP_0,
    PROP_LOCATION,
};

// Pad templates
static GstStaticPadTemplate src_template_audio =
    GST_STATIC_PAD_TEMPLATE("audio_src_%u",
                            GST_PAD_SRC,
                            GST_PAD_SOMETIMES, // 改为 GST_PAD_SOMETIMES
                            GST_STATIC_CAPS("audio/x-raw"));

static GstStaticPadTemplate src_template_video =
    GST_STATIC_PAD_TEMPLATE("video_src_%u",
                            GST_PAD_SRC,
                            GST_PAD_SOMETIMES, // 改为 GST_PAD_SOMETIMES
                            GST_STATIC_CAPS("video/x-raw"));

// 函数声明
static gboolean media_demux_start(MediaDemux *demux);
static gboolean media_demux_stop(MediaDemux *demux);
static GstFlowReturn media_demux_push_data(MediaDemux *demux);
static void media_demux_loop(MediaDemux *demux);
static GstStateChangeReturn media_demux_change_state(GstElement *element, GstStateChange transition);

// 属性设置函数
static void media_demux_set_property(GObject *object, guint prop_id,
                                     const GValue *value, GParamSpec *pspec) {
    MediaDemux *demux = (MediaDemux *)object;
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);

    switch (prop_id) {
        case PROP_LOCATION:
            g_free(priv->location);
            priv->location = g_value_dup_string(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void media_demux_get_property(GObject *object, guint prop_id,
                                     GValue *value, GParamSpec *pspec) {
    MediaDemux *demux = (MediaDemux *)object;
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);

    switch (prop_id) {
        case PROP_LOCATION:
            g_value_set_string(value, priv->location);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void media_demux_set_property_wrapper(GObject *object, guint prop_id,
                                             const GValue *value, GParamSpec *pspec) {
    media_demux_set_property(object, prop_id, value, pspec);
}

static void media_demux_get_property_wrapper(GObject *object, guint prop_id,
                                             GValue *value, GParamSpec *pspec) {
    media_demux_get_property(object, prop_id, value, pspec);
}
static GstPad* create_and_add_pad(MediaDemux *demux, AVStream *stream) {
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);
    GstPadTemplate *template;
    gchar *pad_name;
    GstPad *pad;

    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        template = gst_static_pad_template_get(&src_template_video);
        pad_name = g_strdup_printf("video_src_%u", priv->video_src_pads ? g_list_length(priv->video_src_pads) : 0);
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        template = gst_static_pad_template_get(&src_template_audio);
        pad_name = g_strdup_printf("audio_src_%u", priv->audio_src_pads ? g_list_length(priv->audio_src_pads) : 0);
    } else {
        // Unsupported stream type
        return NULL;
    }

    pad = gst_pad_new_from_template(template, pad_name);
    g_free(pad_name);
    gst_object_unref(template);

    if (!pad) {
        g_print("Failed to create pad from template\n");
        return NULL;
    }

    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        priv->video_src_pads = g_list_append(priv->video_src_pads, pad);
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        priv->audio_src_pads = g_list_append(priv->audio_src_pads, pad);
    }

    if (!gst_element_add_pad(GST_ELEMENT(demux), pad)) {
        g_print("Failed to add pad to element\n");
        gst_object_unref(pad);
        return NULL;
    }

    gst_pad_set_active(pad, TRUE);

    // Define Caps Correctly
    GstCaps *caps;
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        const char *pix_fmt = av_get_pix_fmt_name(stream->codecpar->format);
        if (!pix_fmt) {
            pix_fmt = "I420"; // Default format
        }

        gint num = stream->avg_frame_rate.num;
        gint den = stream->avg_frame_rate.den;
        if (den == 0) {
            den = 1;
        }

        caps = gst_caps_new_simple("video/x-raw", // Changed to video/x-raw
                                   "format", G_TYPE_STRING, pix_fmt,
                                   "width", G_TYPE_INT, stream->codecpar->width,
                                   "height", G_TYPE_INT, stream->codecpar->height,
                                   "framerate", GST_TYPE_FRACTION, num, den,
                                   NULL);
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        const char *audio_fmt = NULL;
        switch (stream->codecpar->format) {
            case AV_SAMPLE_FMT_S16:
                audio_fmt = "S16LE";
                break;
            case AV_SAMPLE_FMT_FLT:
                audio_fmt = "F32LE";
                break;
            // Add more formats as needed
            default:
                audio_fmt = "S16LE"; // Default format
                break;
        }

        caps = gst_caps_new_simple("audio/x-raw", // Changed to audio/x-raw
                                   "format", G_TYPE_STRING, audio_fmt,
                                   "rate", G_TYPE_INT, stream->codecpar->sample_rate,
                                   "channels", G_TYPE_INT, stream->codecpar->ch_layout.nb_channels,
                                   NULL);
    } else {
        caps = NULL;
    }

    if (caps) {
        // Send stream-start first
        gchar *stream_id = g_strdup_printf("stream-%d", stream->index);
        gst_pad_push_event(pad, gst_event_new_stream_start(stream_id));
        g_free(stream_id);

        // Then send caps
        GstEvent *caps_event = gst_event_new_caps(caps);
        gst_caps_unref(caps);
        gst_pad_push_event(pad, caps_event);
    }

    // Send segment event
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    segment.duration = GST_CLOCK_TIME_NONE;
    segment.position = 0;
    segment.rate = 1.0;
    GstEvent *segment_event = gst_event_new_segment(&segment);
    gst_pad_push_event(pad, segment_event);

    // Emit pad-added signal
    g_signal_emit_by_name(G_OBJECT(demux), "pad-added", pad, NULL);

    return pad;
}

static void media_demux_class_init(MediaDemuxClass *klass) {
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    // 设置元数据
    gst_element_class_set_static_metadata(element_class,
                                          "Media Demuxer",
                                          "Demuxer",
                                          "Demuxes media streams using FFmpeg",
                                          "Your Name <youremail@example.com>");

    // 设置状态改变函数
    element_class->change_state = media_demux_change_state;

    // 设置属性设置和获取函数
    gobject_class->set_property = media_demux_set_property_wrapper;
    gobject_class->get_property = media_demux_get_property_wrapper;

    // 注册 location 属性
    g_object_class_install_property(gobject_class, PROP_LOCATION,
        g_param_spec_string("location", "Location", "Location of the media to demux", NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void media_demux_init(MediaDemux *demux) {
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);

    priv->fmt_ctx = NULL;
    priv->video_src_pads = NULL;
    priv->audio_src_pads = NULL;
    priv->started = FALSE;
    priv->task = NULL;
    priv->task_lock = g_new(GRecMutex, 1);
    g_rec_mutex_init(priv->task_lock);
    priv->location = NULL; // 需要通过属性或其他方式设置 location
}

// 状态改变函数
static GstStateChangeReturn media_demux_change_state(GstElement *element, GstStateChange transition) {
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
    MediaDemux *demux = (MediaDemux *)element;
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);

    switch (transition) {
        case GST_STATE_CHANGE_NULL_TO_READY:
            avformat_network_init();
            break;
        case GST_STATE_CHANGE_READY_TO_PAUSED:
            if (!media_demux_start(demux)) {
                return GST_STATE_CHANGE_FAILURE;
            }
            break;
        case GST_STATE_CHANGE_PAUSED_TO_READY:
            media_demux_stop(demux);
            break;
        case GST_STATE_CHANGE_READY_TO_NULL:
            avformat_network_deinit();
            break;
        default:
            break;
    }

    ret = GST_ELEMENT_CLASS(media_demux_parent_class)->change_state(element, transition);

    return ret;
}

// 启动 Demuxer
static gboolean media_demux_start(MediaDemux *demux) {
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);
    g_print("Starting demuxer\n");

    if (!priv->location) {
        g_print("No location set\n");
        return FALSE;
    }

    if (avformat_open_input(&priv->fmt_ctx, priv->location, NULL, NULL) < 0) {
        g_print("Failed to open input\n");
        return FALSE;
    }

    if (avformat_find_stream_info(priv->fmt_ctx, NULL) < 0) {
        g_print("Failed to find stream info\n");
        return FALSE;
    }

    // 遍历所有流，动态创建 Pad
    for (gint i = 0; i < priv->fmt_ctx->nb_streams; i++) {
        AVStream *stream = priv->fmt_ctx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
            stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            GstPad *pad = create_and_add_pad(demux, stream);
            if (!pad) {
                g_print("Failed to create pad for stream %d\n", i);
                continue;
            }
            g_print("Created pad for stream %d\n", i);
        }
    }

    if (priv->video_src_pads == NULL && priv->audio_src_pads == NULL) {
        g_print("No valid video or audio streams found\n");
        return FALSE;
    }

    priv->started = TRUE;

    if (!priv->task) {
        priv->task = gst_task_new((GstTaskFunction)media_demux_loop, demux, NULL);
        gst_task_set_lock(priv->task, priv->task_lock);
    }

    gst_task_start(priv->task);

    return TRUE;
}

// 停止 Demuxer
static gboolean media_demux_stop(MediaDemux *demux) {
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);
    g_print("Stopping demuxer\n");

    priv->started = FALSE;

    if (priv->task) {
        gst_task_stop(priv->task);
        gst_task_join(priv->task);
        gst_object_unref(priv->task);
        priv->task = NULL;
    }

    // 移除并释放所有动态创建的 Pad
    GList *iter;
    for (iter = priv->video_src_pads; iter != NULL; iter = iter->next) {
        GstPad *pad = GST_PAD(iter->data);
        gst_element_remove_pad(GST_ELEMENT(demux), pad);
        gst_object_unref(pad);
    }
    g_list_free(priv->video_src_pads);
    priv->video_src_pads = NULL;

    for (iter = priv->audio_src_pads; iter != NULL; iter = iter->next) {
        GstPad *pad = GST_PAD(iter->data);
        gst_element_remove_pad(GST_ELEMENT(demux), pad);
        gst_object_unref(pad);
    }
    g_list_free(priv->audio_src_pads);
    priv->audio_src_pads = NULL;

    if (priv->fmt_ctx) {
        avformat_close_input(&priv->fmt_ctx);
        priv->fmt_ctx = NULL;
    }

    return TRUE;
}

// 推送数据函数
static GstFlowReturn media_demux_push_data(MediaDemux *demux) {
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);
    AVPacket pkt;

    if (av_read_frame(priv->fmt_ctx, &pkt) >= 0) {
        GstBuffer *buffer = gst_buffer_new_allocate(NULL, pkt.size, NULL);
        gst_buffer_fill(buffer, 0, pkt.data, pkt.size);

        // 根据流索引找到对应的 Pad 并推送数据
        AVStream *stream = priv->fmt_ctx->streams[pkt.stream_index];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            for (GList *iter = priv->video_src_pads; iter != NULL; iter = iter->next) {
                GstPad *pad = GST_PAD(iter->data);
                // 简单地将数据推送到所有视频 Pad
                GstFlowReturn ret = gst_pad_push(pad, gst_buffer_ref(buffer));
                g_print("Push data to video pad returned: %d\n", ret);
                if (ret != GST_FLOW_OK) {
                    g_printerr("Error pushing data to video pad: %d\n", ret);
                }
            }
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            for (GList *iter = priv->audio_src_pads; iter != NULL; iter = iter->next) {
                GstPad *pad = GST_PAD(iter->data);
                // 简单地将数据推送到所有音频 Pad
                GstFlowReturn ret = gst_pad_push(pad, gst_buffer_ref(buffer));
                g_print("Push data to audio pad returned: %d\n", ret);
                if (ret != GST_FLOW_OK) {
                    g_printerr("Error pushing data to audio pad: %d\n", ret);
                }
            }
        }

        gst_buffer_unref(buffer);
        av_packet_unref(&pkt);
    } else {
        // 发送 EOS 事件到所有 Pad
        GList *iter;
        for (iter = priv->video_src_pads; iter != NULL; iter = iter->next) {
            GstPad *pad = GST_PAD(iter->data);
            gchar *stream_id = g_strdup_printf("stream-%d", ((AVStream *)g_list_nth_data(priv->fmt_ctx->streams, 0))->index);
            gst_pad_push_event(pad, gst_event_new_stream_start(stream_id));
            gst_pad_push_event(pad, gst_event_new_eos());
            g_free(stream_id);
        }
        for (iter = priv->audio_src_pads; iter != NULL; iter = iter->next) {
            GstPad *pad = GST_PAD(iter->data);
            gchar *stream_id = g_strdup_printf("stream-%d", ((AVStream *)g_list_nth_data(priv->fmt_ctx->streams, 0))->index);
            gst_pad_push_event(pad, gst_event_new_stream_start(stream_id));
            gst_pad_push_event(pad, gst_event_new_eos());
            g_free(stream_id);
        }
        return GST_FLOW_EOS;
    }

    return GST_FLOW_OK;
}

// Demux 循环函数
static void media_demux_loop(MediaDemux *demux) {
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);
    while (priv->started) {
        GstFlowReturn ret = media_demux_push_data(demux);
        if (ret != GST_FLOW_OK) {
            break;
        }
        g_usleep(10000); // 10ms
    }
}

// 插件初始化函数
gboolean media_demux_plugin_init(GstPlugin *plugin) { // 移除 static
    return gst_element_register(plugin, "media_demux", GST_RANK_NONE, media_demux_get_type());
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    media_demux,
    "Media Demuxer",
    media_demux_plugin_init,
    "1.0",
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/"
)
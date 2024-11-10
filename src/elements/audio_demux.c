#define PACKAGE "my_plugin"
#include "audio_demux.h"
#include <gst/gst.h>
#include <gst/gstsegment.h>
#include <libavformat/avformat.h>
#include <pthread.h>
#include <glib-object.h>
#include <libavcodec/avcodec.h>

// 定义私有结构体
typedef struct _AudioDemuxPrivate {
    AVFormatContext *fmt_ctx;
    gboolean started;
    gint audio_stream_idx;
    GstPad *src_pad;
    gchar *location;
    gboolean is_demuxing;
    pthread_t demux_thread;
    // 新增成员用于AAC转换
    gboolean is_adts;
    // 新增成员用于存储mpeg版本
    gint mpeg_version;
} AudioDemuxPrivate;

// 获取私有数据
G_DEFINE_TYPE_WITH_PRIVATE(AudioDemux, audio_demux, GST_TYPE_ELEMENT)

enum {
    PROP_0,
    PROP_LOCATION,
};

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "audio/mpeg, "
        "mpegversion=(int){2, 4}, "
        "stream-format=(string){raw, adts, adif}"
    )
);


static void audio_demux_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void audio_demux_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static GstStateChangeReturn audio_demux_change_state(GstElement *element, GstStateChange transition);

// 添加ADTS头
static GstBuffer* add_adts_header(const uint8_t *data, size_t size) {
    // 简单的ADTS头部，实际情况可能需要根据AAC配置进行调整
    uint8_t adts_header[7];
    int aac_profile = 2; // AAC LC
    int sample_freq = 4; // 44100 Hz
    int channel_config = 2; // Stereo

    adts_header[0] = 0xFF;
    adts_header[1] = 0xF1;
    adts_header[2] = ((aac_profile) << 6) | (sample_freq << 2) | ((channel_config & 0x4) >> 2);
    adts_header[3] = ((channel_config & 0x3) << 6) | ((size + 7) >> 11);
    adts_header[4] = ((size + 7) & 0x7FF) >> 3;
    adts_header[5] = (((size + 7) & 0x7) << 5) | 0x1F;
    adts_header[6] = 0xFC;

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, size + 7, NULL);
    gst_buffer_fill(buffer, 0, adts_header, 7);
    gst_buffer_fill(buffer, 7, data, size);
    return buffer;
}

static void *demux_thread_func(void *data) {
    AudioDemux *demux = AUDIO_DEMUX(data);
    AudioDemuxPrivate *priv = (AudioDemuxPrivate *)audio_demux_get_instance_private(demux);
    AVPacket packet;

    if (avformat_open_input(&priv->fmt_ctx, priv->location, NULL, NULL) != 0) {
        g_print("Failed to open input file: %s\n", priv->location);
        return NULL;
    }

    if (avformat_find_stream_info(priv->fmt_ctx, NULL) < 0) {
        g_print("Failed to find stream information\n");
        avformat_close_input(&priv->fmt_ctx);
        return NULL;
    }

    priv->audio_stream_idx = -1;
    for (unsigned int i = 0; i < priv->fmt_ctx->nb_streams; i++) {
        if (priv->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            priv->audio_stream_idx = i;
            // 检查是否为ADTS格式
            AVDictionaryEntry *tag = av_dict_get(priv->fmt_ctx->streams[i]->metadata, "StreamFormat", NULL, 0);
            if (tag && g_strcmp0(tag->value, "ADTS") == 0) {
                priv->is_adts = TRUE;
            } else {
                priv->is_adts = FALSE;
            }
            // 获取MPEG版本，假设从codecpar->codec_tag中获取
            // 实际实现可能需要根据具体格式解析
            if (priv->fmt_ctx->streams[i]->codecpar->codec_tag == MKTAG('m', 'p', '4', 'a')) {
                priv->mpeg_version = 4;
            } else {
                priv->mpeg_version = 2;
            }
            break;
        }
    }

    if (priv->audio_stream_idx == -1) {
        g_print("Audio stream not found\n");
        avformat_close_input(&priv->fmt_ctx);
        return NULL;
    }

    priv->started = TRUE;

    // 发送 stream-start 事件
    char stream_id_str[32];
    snprintf(stream_id_str, sizeof(stream_id_str), "stream-%d", priv->audio_stream_idx);
    GstEvent *stream_start_event = gst_event_new_stream_start(stream_id_str);
    if (!gst_pad_push_event(priv->src_pad, stream_start_event)) {
        g_print("Failed to send stream-start event\n");
        avformat_close_input(&priv->fmt_ctx);
        return NULL;
    }

    // 发送 caps 事件
    GstCaps *caps = gst_caps_new_simple(
        "audio/mpeg",
        "mpegversion", G_TYPE_INT, priv->mpeg_version,
        "stream-format", G_TYPE_STRING, priv->is_adts ? "adts" : "raw",
        NULL
    );
    GstEvent *caps_event = gst_event_new_caps(caps);
    gst_caps_unref(caps);
    if (!gst_pad_push_event(priv->src_pad, caps_event)) {
        g_print("Failed to send caps event\n");
        avformat_close_input(&priv->fmt_ctx);
        return NULL;
    }

    // 发送 segment 事件
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    segment.duration = GST_CLOCK_TIME_NONE;
    segment.position = 0;
    segment.rate = 1.0;
    GstEvent *segment_event = gst_event_new_segment(&segment);
    if (!gst_pad_push_event(priv->src_pad, segment_event)) {
        g_print("Failed to send segment event\n");
        avformat_close_input(&priv->fmt_ctx);
        return NULL;
    }

    while (priv->is_demuxing && av_read_frame(priv->fmt_ctx, &packet) >= 0) {
        if (packet.stream_index == priv->audio_stream_idx) {
            GstBuffer *buffer;
            if (priv->is_adts) {
                buffer = gst_buffer_new_allocate(NULL, packet.size, NULL);
                gst_buffer_fill(buffer, 0, packet.data, packet.size);
            } else {
                buffer = add_adts_header(packet.data, packet.size);
            }
            GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;

            GstFlowReturn ret = gst_pad_push(priv->src_pad, buffer);
            if (ret != GST_FLOW_OK) {
                g_print("Failed to push buffer %d\n", ret);
                gst_buffer_unref(buffer);
                break;
            }
        }
        av_packet_unref(&packet); // 清除 AVPacket
    }

    GstEvent *eos_event = gst_event_new_eos();
    gst_pad_send_event(priv->src_pad, eos_event);
    avformat_close_input(&priv->fmt_ctx);
    return NULL;
}

static void audio_demux_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
    AudioDemux *demux = AUDIO_DEMUX(object);
    AudioDemuxPrivate *priv = (AudioDemuxPrivate *)audio_demux_get_instance_private(demux);
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

static void audio_demux_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    AudioDemux *demux = AUDIO_DEMUX(object);
    AudioDemuxPrivate *priv = (AudioDemuxPrivate *)audio_demux_get_instance_private(demux);
    switch (prop_id) {
        case PROP_LOCATION:
            g_value_set_string(value, priv->location);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void audio_demux_class_init(AudioDemuxClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);

    gobject_class->set_property = audio_demux_set_property;
    gobject_class->get_property = audio_demux_get_property;

    g_object_class_install_property(
        gobject_class,
        PROP_LOCATION,
        g_param_spec_string(
            "location",
            "Location",
            "File path to open",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS
        )
    );

    gst_element_class_set_static_metadata(
        gstelement_class,
        "AudioDemux",
        "Demuxer",
        "Custom FFmpeg-based Audio Demuxer with ADTS support",
        "Your Name <your.email@example.com>"
    );

    gst_element_class_add_static_pad_template(gstelement_class, &src_template);

    gstelement_class->change_state = GST_DEBUG_FUNCPTR(audio_demux_change_state);
}

static void audio_demux_init(AudioDemux *demux) {
    AudioDemuxPrivate *priv = (AudioDemuxPrivate *)audio_demux_get_instance_private(demux);

    priv->location = NULL;
    priv->fmt_ctx = NULL;
    priv->started = FALSE;
    priv->audio_stream_idx = -1;
    priv->src_pad = gst_pad_new_from_static_template(&src_template, "src");
    gst_element_add_pad(GST_ELEMENT(demux), priv->src_pad);
    priv->is_demuxing = FALSE;
    priv->is_adts = FALSE;
    priv->mpeg_version = 2; // 默认MPEG版本为2
}

static GstStateChangeReturn audio_demux_change_state(GstElement *element, GstStateChange transition) {
    AudioDemux *demux = AUDIO_DEMUX(element);
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

    AudioDemuxPrivate *priv = (AudioDemuxPrivate *)audio_demux_get_instance_private(demux);

    switch (transition) {
        case GST_STATE_CHANGE_NULL_TO_READY:
            // 初始化资源
            if (priv->location == NULL) {
                GST_ERROR_OBJECT(element, "Location property is not set");
                return GST_STATE_CHANGE_FAILURE;
            }
            break;
        case GST_STATE_CHANGE_READY_TO_PAUSED:
            priv->is_demuxing = TRUE;
            if (pthread_create(&priv->demux_thread, NULL, demux_thread_func, demux) != 0) {
                GST_ERROR_OBJECT(element, "Failed to create demux thread");
                priv->is_demuxing = FALSE;
                return GST_STATE_CHANGE_FAILURE;
            }
            break;
        case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
            // 可以在这里添加额外的逻辑
            break;
        case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
            priv->is_demuxing = FALSE;
            pthread_join(priv->demux_thread, NULL);
            break;
        case GST_STATE_CHANGE_PAUSED_TO_READY:
            // 清理资源
            break;
        case GST_STATE_CHANGE_READY_TO_NULL:
            g_free(priv->location);
            priv->location = NULL;
            break;
        default:
            break;
    }

    ret = GST_ELEMENT_CLASS(audio_demux_parent_class)->change_state(element, transition);
    return ret;
}

// 注册插件
gboolean audio_demux_plugin_init(GstPlugin *plugin) {
    return gst_element_register(plugin, "audio_demux", GST_RANK_NONE, AUDIO_TYPE_DEMUX);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    audio_demux_plugin,
    "Custom Audio Demuxer with ADTS Support",
    audio_demux_plugin_init,
    "1.0",
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/"
)

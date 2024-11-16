#define PACKAGE "mediamdemux"
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
    // gchar *group_id; // 移除此字段
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
                            GST_STATIC_CAPS("audio/mpeg"));

static GstStaticPadTemplate src_template_video =
    GST_STATIC_PAD_TEMPLATE("video_src_%u",
                            GST_PAD_SRC,
                            GST_PAD_SOMETIMES, // 改为 GST_PAD_SOMETIMES
                            GST_STATIC_CAPS("video/x-h264"));

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
            g_print("WARNING: [media_demux] Unknown property ID %u\n", prop_id);
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
            g_print("WARNING: [media_demux] Unknown property ID %u\n", prop_id);
            break;
    }
}

static GstPad* create_and_add_pad(MediaDemux *demux, AVStream *stream) {
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);
    GstPadTemplate *template;
    gchar pad_name[32];
    GstPad *pad;

    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        template = gst_static_pad_template_get(&src_template_video);
        g_snprintf(pad_name, sizeof(pad_name), "video_src_%u", priv->video_src_pads ? g_list_length(priv->video_src_pads) : 0);
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        template = gst_static_pad_template_get(&src_template_audio);
        g_snprintf(pad_name, sizeof(pad_name), "audio_src_%u", priv->audio_src_pads ? g_list_length(priv->audio_src_pads) : 0);
    } else {
        // Unsupported stream type
        return NULL;
    }

    pad = gst_pad_new_from_template(template, pad_name);
    gst_object_unref(template);

    if (!pad) {
        g_print("ERROR: [media_demux] Failed to create pad from template\n");
        return NULL;
    }

    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        priv->video_src_pads = g_list_append(priv->video_src_pads, pad);
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        priv->audio_src_pads = g_list_append(priv->audio_src_pads, pad);
    }

    if (!gst_element_add_pad(GST_ELEMENT(demux), pad)) {
        g_print("ERROR: [media_demux] Failed to add pad to element\n");
        gst_object_unref(pad);
        return NULL;
    }

    gst_pad_set_active(pad, TRUE);

    // 生成唯一的 stream-id
    gchar *stream_id = g_strdup_printf("%s_%u", pad_name, (unsigned int)(g_list_length(priv->video_src_pads) + g_list_length(priv->audio_src_pads)));

    // 发送 stream-start 事件，使用唯一的 stream-id
    GstEvent *stream_start_event = gst_event_new_stream_start(stream_id);
    gst_pad_push_event(pad, stream_start_event);
    g_print("INFO: [media_demux] Sent stream-start with stream-id: %s to pad: %s\n", stream_id, pad_name);
    g_free(stream_id); // 释放独立的 stream-id

    // 定义 Caps
    GstCaps *caps = NULL;
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        const char *pix_fmt = av_get_pix_fmt_name(stream->codecpar->format);
        if (!pix_fmt) {
            pix_fmt = "I420"; // Default format
        }

        // 获取 codec data
        GstBuffer *codec_data = gst_buffer_new_allocate(NULL, stream->codecpar->extradata_size, NULL);
        GstMapInfo map;
        if (gst_buffer_map(codec_data, &map, GST_MAP_WRITE)) {
            memcpy(map.data, stream->codecpar->extradata, stream->codecpar->extradata_size);
            gst_buffer_unmap(codec_data, &map);
        } else {
            g_print("ERROR: [media_demux] Failed to map codec_data buffer\n");
            gst_buffer_unref(codec_data);
            return NULL;
        }

        caps = gst_caps_new_simple("video/x-h264",
                                   "stream-format", G_TYPE_STRING, "avc",
                                   "alignment", G_TYPE_STRING, "au",
                                   "width", G_TYPE_INT, stream->codecpar->width,
                                   "height", G_TYPE_INT, stream->codecpar->height,
                                   "codec_data", GST_TYPE_BUFFER, codec_data,
                                   NULL);
        gst_buffer_unref(codec_data);
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

        gint mpeg_version = 1; // 默认值
        const char *stream_format = "raw"; // 默认值

        if (stream->codecpar->codec_id == AV_CODEC_ID_MP3) {
            mpeg_version = 1;
            stream_format = "mp3";
        } else if (stream->codecpar->codec_id == AV_CODEC_ID_AAC) {
            mpeg_version = 2;
            stream_format = "adts";
        }

        caps = gst_caps_new_simple("audio/mpeg", 
                                   "mpegversion", G_TYPE_INT, mpeg_version,
                                   "stream-format", G_TYPE_STRING, stream_format,
                                   NULL);
    }

    if (caps) {
        // 发送 caps 事件
        GstEvent *caps_event = gst_event_new_caps(caps);
        gst_caps_unref(caps);
        gst_pad_push_event(pad, caps_event);
    }

    // 发送 segment event
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    segment.duration = GST_CLOCK_TIME_NONE;
    segment.position = 0;
    segment.rate = 1.0;
    GstEvent *segment_event = gst_event_new_segment(&segment);
    gst_pad_push_event(pad, segment_event);

    // Emit pad-added signal
    g_signal_emit_by_name(G_OBJECT(demux), "pad-added", pad);

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
    gobject_class->set_property = media_demux_set_property;
    gobject_class->get_property = media_demux_get_property;

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
    priv->task_lock = g_new0(GRecMutex, 1);
    g_rec_mutex_init(priv->task_lock);
    priv->location = NULL; // 需要通过属性或其他方式设置 location

    // 移除 group_id 的生成
    // uuid_t uuid;
    // uuid_generate(uuid);
    // gchar uuid_str[37]; // UUID 长度为36，加上终止符
    // uuid_unparse_lower(uuid, uuid_str);
    // priv->group_id = g_strdup(uuid_str);
    // g_print("INFO: [media_demux] Generated group-id: %s\n", priv->group_id);
}

// 状态改变函数
static GstStateChangeReturn media_demux_change_state(GstElement *element, GstStateChange transition) {
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
    MediaDemux *demux = (MediaDemux *)element;
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);

    switch (transition) {
        case GST_STATE_CHANGE_NULL_TO_READY:
            break;
        case GST_STATE_CHANGE_READY_TO_PAUSED:
            if (!media_demux_start(demux)) {
                g_print("ERROR: [media_demux] Failed to start demuxer\n");
                return GST_STATE_CHANGE_FAILURE;
            }
            break;
        case GST_STATE_CHANGE_PAUSED_TO_READY:
            media_demux_stop(demux);
            break;
        case GST_STATE_CHANGE_READY_TO_NULL:
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
    g_print("INFO: [media_demux] Starting demuxer\n");

    if (!priv->location) {
        g_print("ERROR: [media_demux] No location set\n");
        return FALSE;
    }

    if (avformat_open_input(&priv->fmt_ctx, priv->location, NULL, NULL) < 0) {
        g_print("ERROR: [media_demux] Failed to open input: %s\n", priv->location);
        return FALSE;
    }

    if (avformat_find_stream_info(priv->fmt_ctx, NULL) < 0) {
        g_print("ERROR: [media_demux] Failed to find stream info\n");
        avformat_close_input(&priv->fmt_ctx);
        priv->fmt_ctx = NULL;
        return FALSE;
    }

    // 遍历所有流，动态创建 Pad
    for (gint i = 0; i < priv->fmt_ctx->nb_streams; i++) {
        AVStream *stream = priv->fmt_ctx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
            stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            GstPad *pad = create_and_add_pad(demux, stream);
            if (!pad) {
                g_print("WARNING: [media_demux] Failed to create pad for stream %d\n", i);
                continue;
            }
            g_print("INFO: [media_demux] Created pad for stream %d\n", i);
        }
    }

    if (priv->video_src_pads == NULL && priv->audio_src_pads == NULL) {
        g_print("ERROR: [media_demux] No valid video or audio streams found\n");
        avformat_close_input(&priv->fmt_ctx);
        priv->fmt_ctx = NULL;
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
    g_print("INFO: [media_demux] Stopping demuxer\n");

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

static int get_sample_rate_index(int sample_rate) {
    switch (sample_rate) {
        case 96000: return 0;
        case 88200: return 1;
        case 64000: return 2;
        case 48000: return 3;
        case 44100: return 4;
        case 32000: return 5;
        case 24000: return 6;
        case 22050: return 7;
        case 16000: return 8;
        case 12000: return 9;
        case 11025: return 10;
        case 8000: return 11;
        case 7350: return 12;
        default: return 15; // 15 is reserved
    }
}

static GstBuffer* convert_aac_to_adts(MediaDemux *demux, AVStream *stream, AVPacket *pkt) {
    if (pkt->size <= 0) {
        g_print("WARNING: [media_demux] Invalid packet size: %d\n", pkt->size);
        return NULL;
    }

    int sample_rate_index = get_sample_rate_index(stream->codecpar->sample_rate);
    if (sample_rate_index < 0) {
        g_print("WARNING: [media_demux] Unsupported sample rate: %d\n", stream->codecpar->sample_rate);
        return NULL;
    }

    int channels = stream->codecpar->ch_layout.nb_channels;

    int adts_profile = 0; // 默认使用 AAC-LC
    if (stream->codecpar->profile == FF_PROFILE_AAC_MAIN)
        adts_profile = 0;
    else if (stream->codecpar->profile == FF_PROFILE_AAC_LOW)
        adts_profile = 1;
    else if (stream->codecpar->profile == FF_PROFILE_AAC_SSR)
        adts_profile = 2;

    guint8 adts_header[7];
    int aac_frame_length = pkt->size + 7;

    adts_header[0] = 0xFF;
    adts_header[1] = 0xF1; // 固定值
    adts_header[2] = (adts_profile << 6) | (sample_rate_index << 2) | ((channels >> 2) & 0x1);
    adts_header[3] = ((channels & 0x3) << 6) | ((aac_frame_length >> 11) & 0x3);
    adts_header[4] = (aac_frame_length >> 3) & 0xFF;
    adts_header[5] = ((aac_frame_length & 0x7) << 5) | 0x1F;
    adts_header[6] = 0xFC;

    GstBuffer *adts_buffer = gst_buffer_new_allocate(NULL, aac_frame_length, NULL);
    GstMapInfo map;
    if (!gst_buffer_map(adts_buffer, &map, GST_MAP_WRITE)) {
        g_print("WARNING: [media_demux] Failed to map ADTS buffer\n");
        gst_buffer_unref(adts_buffer);
        return NULL;
    }

    memcpy(map.data, adts_header, 7);
    memcpy(map.data + 7, pkt->data, pkt->size);
    gst_buffer_unmap(adts_buffer, &map);

    return adts_buffer;
}

// 推送数据函数
static GstFlowReturn media_demux_push_data(MediaDemux *demux) {
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);
    AVPacket pkt;

    if (av_read_frame(priv->fmt_ctx, &pkt) >= 0) {
        GstBuffer *buffer = NULL;
        AVStream *stream = priv->fmt_ctx->streams[pkt.stream_index];
        GstMapInfo map;

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            buffer = gst_buffer_new_allocate(NULL, pkt.size, NULL);
            if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
                memcpy(map.data, pkt.data, pkt.size);
                gst_buffer_unmap(buffer, &map);
            } else {
                g_print("ERROR: [media_demux] Failed to map video buffer\n");
                gst_buffer_unref(buffer);
                av_packet_unref(&pkt);
                return GST_FLOW_ERROR;
            }
            g_print("INFO: [media_demux] Push data to video buffer, size: %d, pts: %" PRId64 "\n", pkt.size, pkt.pts);
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (stream->codecpar->codec_id == AV_CODEC_ID_AAC) {
                buffer = convert_aac_to_adts(demux, stream, &pkt);
                if (!buffer) {
                    g_print("WARNING: [media_demux] Failed to convert AAC to ADTS\n");
                    av_packet_unref(&pkt);
                    return GST_FLOW_ERROR;
                }
                g_print("INFO: [media_demux] Push data to audio buffer (AAC to ADTS), size: %zu, pts: %" PRId64 "\n", gst_buffer_get_size(buffer), pkt.pts);
            } else {
                buffer = gst_buffer_new_allocate(NULL, pkt.size, NULL);
                if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
                    memcpy(map.data, pkt.data, pkt.size);
                    gst_buffer_unmap(buffer, &map);
                } else {
                    g_print("ERROR: [media_demux] Failed to map audio buffer\n");
                    gst_buffer_unref(buffer);
                    av_packet_unref(&pkt);
                    return GST_FLOW_ERROR;
                }
                g_print("INFO: [media_demux] Push data to audio buffer, size: %d, pts: %" PRId64 "\n", pkt.size, pkt.pts);
            }
        }

        if (buffer) {
            GList *iter;
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                for (iter = priv->video_src_pads; iter != NULL; iter = iter->next) {
                    GstPad *pad = GST_PAD(iter->data);
                    GstFlowReturn ret = gst_pad_push(pad, gst_buffer_ref(buffer));
                    g_print("LOG: [media_demux] Push data to video pad returned: %d\n", ret);
                    if (ret != GST_FLOW_OK) {
                        g_print("WARNING: [media_demux] Error pushing data to video pad: %d\n", ret);
                    }
                }
            } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                for (iter = priv->audio_src_pads; iter != NULL; iter = iter->next) {
                    GstPad *pad = GST_PAD(iter->data);
                    GstFlowReturn ret = gst_pad_push(pad, gst_buffer_ref(buffer));
                    g_print("LOG: [media_demux] Push data to audio pad returned: %d\n", ret);
                    if (ret != GST_FLOW_OK) {
                        g_print("WARNING: [media_demux] Error pushing data to audio pad: %d\n", ret);
                    }
                }
            }
            gst_buffer_unref(buffer);
        }

        av_packet_unref(&pkt);
    } else {
        // 发送 EOS 事件到所有 Pad，仅发送 eos 事件
        GList *iter;
        for (iter = priv->video_src_pads; iter != NULL; iter = iter->next) {
            GstPad *pad = GST_PAD(iter->data);
            GstEvent *eos_event = gst_event_new_eos();
            gst_pad_push_event(pad, eos_event);
            g_print("INFO: [media_demux] Sent EOS to video pad: %s\n", gst_pad_get_name(pad));
        }
        for (iter = priv->audio_src_pads; iter != NULL; iter = iter->next) {
            GstPad *pad = GST_PAD(iter->data);
            GstEvent *eos_event = gst_event_new_eos();
            gst_pad_push_event(pad, eos_event);
            g_print("INFO: [media_demux] Sent EOS to audio pad: %s\n", gst_pad_get_name(pad));
        }
        return GST_FLOW_EOS;
    }

    return GST_FLOW_OK;
}

// Demux 循环函数
static void media_demux_loop(MediaDemux *demux) {
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);
    g_print("INFO: [media_demux] media_demux_loop enter\n");
    while (priv->started) {
        GstFlowReturn ret = media_demux_push_data(demux);
        if (ret != GST_FLOW_OK) {
            g_print("WARNING: [media_demux] Push data returned: %d, stopping loop\n", ret);
            break;
        }
        // 使用更高效的同步机制代替 g_usleep
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
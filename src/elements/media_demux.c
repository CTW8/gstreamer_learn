#define PACKAGE "my_plugin"
#include "media_demux.h"
#include <gst/gst.h>
#include <gst/gstsegment.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <pthread.h>
#include <string.h>

/* Define private structure */
typedef struct _MediaDemuxPrivate {
    AVFormatContext *fmt_ctx;
    gboolean started;
    GstPad *video_src_pad;
    GstPad *audio_src_pad;
    gchar *location;
    gboolean is_demuxing;
    pthread_t demux_thread;
    GstCaps *video_caps;
    GstCaps *audio_caps;
    gint video_stream_idx;
    gint audio_stream_idx;
    enum AVMediaType media_type;
    /* For audio caps parameters */
    gboolean is_adts;
    gint mpeg_version;
    /* For video caps parameters */
    GstBuffer *codec_data;  // SPS and PPS for H.264/H.265
    gchar *profile;
    gint level;
    guint group_id;  // Added group_id
} MediaDemuxPrivate;

struct _MediaDemux {
    GstElement element;
};

/* Get private data */
G_DEFINE_TYPE_WITH_PRIVATE(MediaDemux, media_demux, GST_TYPE_ELEMENT)

/* Property enumeration */
enum {
    PROP_0,
    PROP_LOCATION,
    /* Add other properties here */
};

/* Pad templates */
static GstStaticPadTemplate video_src_template = GST_STATIC_PAD_TEMPLATE(
    "video_src_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS("video/x-h264")
);

static GstStaticPadTemplate audio_src_template = GST_STATIC_PAD_TEMPLATE(
    "audio_src_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY
);

static GstBuffer* get_codec_data(AVCodecParameters *codecpar);

/* Set property function */
static void media_demux_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
    MediaDemux *demux = MEDIA_DEMUX(object);
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

/* Get property function */
static void media_demux_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    MediaDemux *demux = MEDIA_DEMUX(object);
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

/* Helper function: Get MIME type based on codec_id and media_type */
static const gchar* get_mime_type(enum AVCodecID codec_id, enum AVMediaType media_type) {
    if (media_type == AVMEDIA_TYPE_VIDEO) {
        switch (codec_id) {
            case AV_CODEC_ID_H264:
                return "video/x-h264";
            case AV_CODEC_ID_HEVC:
                return "video/x-h265";
            case AV_CODEC_ID_MPEG4:
                return "video/mpeg";
            /* Add support for other video codecs */
            default:
                return "video/x-unknown";
        }
    } else if (media_type == AVMEDIA_TYPE_AUDIO) {
        switch (codec_id) {
            case AV_CODEC_ID_AAC:
                return "audio/mpeg";
            case AV_CODEC_ID_MP3:
                return "audio/mpeg";
            /* Add support for other audio codecs */
            default:
                return "audio/x-unknown";
        }
    }
    return "application/octet-stream";
}

static gboolean open_input_file(MediaDemuxPrivate *priv) {
    if (avformat_open_input(&priv->fmt_ctx, priv->location, NULL, NULL) != 0) {
        g_print("Failed to open input file: %s\n", priv->location);
        return FALSE;
    }

    if (avformat_find_stream_info(priv->fmt_ctx, NULL) < 0) {
        g_print("Failed to find stream information\n");
        avformat_close_input(&priv->fmt_ctx);
        return FALSE;
    }

    return TRUE;
}

static void select_streams(MediaDemuxPrivate *priv) {
    priv->video_stream_idx = -1;
    priv->audio_stream_idx = -1;
    for (unsigned int i = 0; i < priv->fmt_ctx->nb_streams; i++) {
        enum AVMediaType type = priv->fmt_ctx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && priv->video_stream_idx == -1) {
            priv->video_stream_idx = i;
        } else if (type == AVMEDIA_TYPE_AUDIO && priv->audio_stream_idx == -1) {
            priv->audio_stream_idx = i;
        }
        if (priv->video_stream_idx != -1 && priv->audio_stream_idx != -1) {
            break;
        }
    }
}

static gboolean process_video_stream(MediaDemux *demux, MediaDemuxPrivate *priv) {
    AVStream *stream = priv->fmt_ctx->streams[priv->video_stream_idx];
    GstPadTemplate *video_pad_template = gst_static_pad_template_get(&video_src_template);
    gchar *pad_name = g_strdup_printf("video_src_%u", stream->index);
    priv->video_src_pad = gst_pad_new_from_template(video_pad_template, pad_name);
    gst_object_unref(video_pad_template);

    if (!priv->video_src_pad) {
        g_print("Failed to create video pad from template\n");
        return FALSE;
    }

    gst_pad_set_active(priv->video_src_pad, TRUE);

    const gchar *stream_format = (stream->codecpar->codec_id == AV_CODEC_ID_H264) ? "avc" : "hvc1";
    GstBuffer *codec_data = get_codec_data(stream->codecpar);
    GstCaps *caps = gst_caps_new_simple(
        "video/x-h264",
        "stream-format", G_TYPE_STRING, stream_format,
        "alignment", G_TYPE_STRING, "au",
        "width", G_TYPE_INT, stream->codecpar->width,
        "height", G_TYPE_INT, stream->codecpar->height,
        "framerate", GST_TYPE_FRACTION, stream->avg_frame_rate.num, stream->avg_frame_rate.den,
        "codec_data", GST_TYPE_BUFFER, codec_data,
        NULL
    );
    gst_buffer_unref(codec_data);

    GstEvent *stream_start_event = gst_event_new_stream_start(pad_name);
    g_free(pad_name);

    GstStructure *structure = gst_event_writable_structure(stream_start_event);
    gst_structure_set(structure, "group-id", G_TYPE_UINT, priv->group_id, NULL);  // 增加 group-id 到 stream_start_event
    if (!gst_pad_push_event(priv->video_src_pad, stream_start_event)) {
        g_print("Failed to push stream start event on video pad\n");
        return FALSE;
    }

    if (!gst_pad_set_caps(priv->video_src_pad, caps)) {
        g_printerr("Failed to set caps on video pad\n");
        return FALSE;
    }

    if (!gst_element_add_pad(GST_ELEMENT(demux), priv->video_src_pad)) {
        g_print("Failed to add video pad to element\n");
        gst_object_unref(priv->video_src_pad);
        return FALSE;
    }

    GstEvent *caps_event = gst_event_new_caps(caps);
    if (!gst_pad_push_event(priv->video_src_pad, caps_event)) {
        g_print("Failed to push caps event on video pad\n");
        return FALSE;
    }
    gst_caps_unref(caps);

    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    GstEvent *segment_event = gst_event_new_segment(&segment);
    if (!gst_pad_push_event(priv->video_src_pad, segment_event)) {
        g_print("Failed to push segment event on video pad\n");
        return FALSE;
    }

    return TRUE;
}

static gboolean process_audio_stream(MediaDemux *demux, MediaDemuxPrivate *priv) {
    AVStream *stream = priv->fmt_ctx->streams[priv->audio_stream_idx];
    GstPadTemplate *audio_pad_template = gst_static_pad_template_get(&audio_src_template);
    gchar *pad_name = g_strdup_printf("audio_src_%u", stream->index);
    priv->audio_src_pad = gst_pad_new_from_template(audio_pad_template, pad_name);
    gst_object_unref(audio_pad_template);

    if (!priv->audio_src_pad) {
        g_print("Failed to create audio pad from template\n");
        return FALSE;
    }

    gst_pad_set_active(priv->audio_src_pad, TRUE);

    const gchar *stream_format = (stream->codecpar->codec_id == AV_CODEC_ID_AAC) ? "adts" : "raw";
    GstCaps *caps = gst_caps_new_simple(
        "audio/mpeg",
        "mpegversion", G_TYPE_INT, 4,
        "stream-format", G_TYPE_STRING, stream_format,
        "rate", G_TYPE_INT, stream->codecpar->sample_rate,
        "channels", G_TYPE_INT, stream->codecpar->ch_layout.nb_channels,
        NULL
    );

    // gchar *stream_id = gst_pad_create_stream_id(priv->audio_src_pad, GST_ELEMENT(demux), pad_name);
    // g_print("stream_id: %s\n", stream_id);
    GstEvent *stream_start_event = gst_event_new_stream_start(pad_name);
    // g_free(stream_id);
    g_free(pad_name);

    GstStructure *structure = gst_event_writable_structure(stream_start_event);
    gst_structure_set(structure, "group-id", G_TYPE_UINT, priv->group_id, NULL);  // Added group-id to stream_start_event
    if (!gst_pad_push_event(priv->audio_src_pad, stream_start_event)) {
        g_print("Failed to push stream start event on audio pad\n");
        return FALSE;
    }

    if (!gst_pad_set_caps(priv->audio_src_pad, caps)) {
        g_printerr("Failed to set caps on audio pad\n");
        return FALSE;
    }

    if (!gst_element_add_pad(GST_ELEMENT(demux), priv->audio_src_pad)) {
        g_print("Failed to add audio pad to element\n");
        gst_object_unref(priv->audio_src_pad);
        return FALSE;
    }

    GstEvent *caps_event = gst_event_new_caps(caps);
    if (!gst_pad_push_event(priv->audio_src_pad, caps_event)) {
        g_print("Failed to push caps event on audio pad\n");
        return FALSE;
    }
    gst_caps_unref(caps);

    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    GstEvent *segment_event = gst_event_new_segment(&segment);
    if (!gst_pad_push_event(priv->audio_src_pad, segment_event)) {
        g_print("Failed to push segment event on audio pad\n");
        return FALSE;
    }

    return TRUE;
}

static gboolean media_demux_start(MediaDemux *demux) {
    g_print("media_demux_start\n");
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);

    if (!open_input_file(priv)) {
        return FALSE;
    }

    select_streams(priv);

    if (priv->video_stream_idx == -1 && priv->audio_stream_idx == -1) {
        g_print("No valid audio or video stream found\n");
        avformat_close_input(&priv->fmt_ctx);
        return FALSE;
    }

    if (priv->video_stream_idx != -1) {
        if (!process_video_stream(demux, priv)) {
            return FALSE;
        }
    }

    if (priv->audio_stream_idx != -1) {
        if (!process_audio_stream(demux, priv)) {
            return FALSE;
        }
    }

    priv->started = TRUE;

    return TRUE;
}

static gchar* gst_buffer_to_hex_string(GstBuffer *buffer) {
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        return NULL;
    }

    gchar *hex_str = g_malloc(map.size * 2 + 1); // Each byte becomes two hex characters
    for (gsize i = 0; i < map.size; i++) {
        sprintf(&hex_str[i * 2], "%02x", map.data[i]);
    }
    hex_str[map.size * 2] = '\0';

    gst_buffer_unmap(buffer, &map);
    return hex_str;
}
/* Helper function: Get H.264/H.265 codec data (SPS and PPS) */
static GstBuffer* get_codec_data(AVCodecParameters *codecpar) {
    if (codecpar->extradata && codecpar->extradata_size > 0) {
        GstBuffer *buffer = gst_buffer_new_allocate(NULL, codecpar->extradata_size, NULL);
        gst_buffer_fill(buffer, 0, codecpar->extradata, codecpar->extradata_size);
        return buffer;
    }
    return NULL;
}

/* Helper function: Add ADTS header to AAC frame */
static GstBuffer* add_adts_header(GstBuffer *buffer, AVCodecParameters *codecpar) {
    GstMapInfo map;
    gst_buffer_map(buffer, &map, GST_MAP_READ);

    guint8 adts_header[7];
    guint16 frame_length = map.size + 7;

    static const gint sample_rate_index[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350
    };
    static guint8 sr_index = 0;
    for (guint16 i = 0; i < G_N_ELEMENTS(sample_rate_index); i++) {
        if (sample_rate_index[i] == codecpar->sample_rate) {
            sr_index = i;
            break;
        }
    }

    adts_header[0] = 0xFF;
    adts_header[1] = 0xF1; // Syncword and MPEG-4
    adts_header[2] = ((codecpar->profile + 1) << 6) | (sr_index << 2) | ((codecpar->ch_layout.nb_channels >> 2) & 0x1);
    adts_header[3] = ((codecpar->ch_layout.nb_channels & 0x3) << 6) | ((frame_length >> 11) & 0x3);
    adts_header[4] = (frame_length >> 3) & 0xFF;
    adts_header[5] = ((frame_length & 0x7) << 5) | 0x1F;
    adts_header[6] = 0xFC;

    GstBuffer *adts_buffer = gst_buffer_new_allocate(NULL, frame_length, NULL);
    gst_buffer_fill(adts_buffer, 0, adts_header, 7);
    gst_buffer_fill(adts_buffer, 7, map.data, map.size);

    gst_buffer_unmap(buffer, &map);
    gst_buffer_unref(buffer);

    return adts_buffer;
}

/* Demux thread function */
static void *demux_thread_func(void *data) {
    MediaDemux *demux = MEDIA_DEMUX(data);
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);
    AVPacket packet;
    int ret;

    av_init_packet(&packet);
    while (priv->is_demuxing && (ret = av_read_frame(priv->fmt_ctx, &packet)) >= 0) {
        if (packet.stream_index == priv->video_stream_idx || packet.stream_index == priv->audio_stream_idx) {
            GstBuffer *buffer;

            if (packet.stream_index == priv->audio_stream_idx) {
                /* Convert audio to ADTS format */
                buffer = gst_buffer_new_allocate(NULL, packet.size, NULL);
                gst_buffer_fill(buffer, 0, packet.data, packet.size);
                buffer = add_adts_header(buffer, priv->fmt_ctx->streams[packet.stream_index]->codecpar);
            } else {
                buffer = gst_buffer_new_allocate(NULL, packet.size, NULL);
                gst_buffer_fill(buffer, 0, packet.data, packet.size);
            }

            g_print("Packet type: %s, PTS: %" GST_TIME_FORMAT ", Size: %d\n",
                    (packet.stream_index == priv->video_stream_idx) ? "Video" : "Audio",
                    GST_TIME_ARGS(packet.pts), packet.size);
            /* Set timestamps */
            GstClockTime pts = GST_CLOCK_TIME_NONE;
            GstClockTime dts = GST_CLOCK_TIME_NONE;
            if (packet.pts != AV_NOPTS_VALUE) {
                pts = gst_util_uint64_scale(packet.pts, GST_SECOND * priv->fmt_ctx->streams[packet.stream_index]->time_base.num, priv->fmt_ctx->streams[packet.stream_index]->time_base.den);
            }
            if (packet.dts != AV_NOPTS_VALUE) {
                dts = gst_util_uint64_scale(packet.dts, GST_SECOND * priv->fmt_ctx->streams[packet.stream_index]->time_base.num, priv->fmt_ctx->streams[packet.stream_index]->time_base.den);
            }
            GST_BUFFER_PTS(buffer) = pts;
            GST_BUFFER_DTS(buffer) = dts;
            GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(packet.duration, GST_SECOND * priv->fmt_ctx->streams[packet.stream_index]->time_base.num, priv->fmt_ctx->streams[packet.stream_index]->time_base.den);
            g_print("PTS: %" GST_TIME_FORMAT ", DTS: %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(pts), GST_TIME_ARGS(dts));
            GstFlowReturn flow_ret;
            if (packet.stream_index == priv->video_stream_idx) {
                flow_ret = gst_pad_push(priv->video_src_pad, buffer);
            } else {
                flow_ret = gst_pad_push(priv->audio_src_pad, buffer);
            }

            if (flow_ret != GST_FLOW_OK) {
                g_print("Failed to push buffer: %d\n", flow_ret);
                gst_buffer_unref(buffer);
                break;
            }
        }
        av_packet_unref(&packet);
    }

    /* Check for errors */
    if (ret < 0 && ret != AVERROR_EOF) {
        g_print("Error reading frame: %s\n", av_err2str(ret));
    }

    /* Send EOS event */
    if (priv->video_stream_idx != -1) {
        GstEvent *eos_event = gst_event_new_eos();
        if (!gst_pad_push_event(priv->video_src_pad, eos_event)) {
            g_print("Failed to push EOS event on video pad\n");
        } else {
            g_print("EOS event sent on video pad\n");
        }
    }
    if (priv->audio_stream_idx != -1) {
        GstEvent *eos_event = gst_event_new_eos();
        if (!gst_pad_push_event(priv->audio_src_pad, eos_event)) {
            g_print("Failed to push EOS event on audio pad\n");
        } else {
            g_print("EOS event sent on audio pad\n");
        }
    }
    avformat_close_input(&priv->fmt_ctx);

    /* Free allocated resources */
    if (priv->codec_data) {
        gst_buffer_unref(priv->codec_data);
        priv->codec_data = NULL;
    }

    return NULL;
}

/* State change function */
static GstStateChangeReturn media_demux_change_state(GstElement *element, GstStateChange transition) {
    MediaDemux *demux = MEDIA_DEMUX(element);
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

    g_print("media_demux_change_state: %s\n", gst_element_state_get_name(transition));
    switch (transition) {
        case GST_STATE_CHANGE_NULL_TO_READY:
            g_print("GST_STATE_CHANGE_NULL_TO_READY\n");
            /* Check if location is set */
            if (priv->location == NULL) {
                g_print("Location property is not set\n");
                return GST_STATE_CHANGE_FAILURE;
            }
            break;
        case GST_STATE_CHANGE_READY_TO_PAUSED:
            g_print("GST_STATE_CHANGE_READY_TO_PAUSED\n");
            priv->is_demuxing = TRUE;
            if (!media_demux_start(demux)) {
                g_print("Failed to start demuxing\n");
                return GST_STATE_CHANGE_FAILURE;
            }
            if (pthread_create(&priv->demux_thread, NULL, demux_thread_func, demux) != 0) {
                g_print("Failed to create demux thread\n");
                priv->is_demuxing = FALSE;
                return GST_STATE_CHANGE_FAILURE;
            }
            break;
        case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
            g_print("GST_STATE_CHANGE_PAUSED_TO_PLAYING\n");
            /* Add logic if needed */
            break;
        case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
            g_print("GST_STATE_CHANGE_PLAYING_TO_PAUSED\n");
            /* Stop demuxing */
            priv->is_demuxing = FALSE;
            pthread_join(priv->demux_thread, NULL);
            break;
        case GST_STATE_CHANGE_PAUSED_TO_READY:
            g_print("GST_STATE_CHANGE_PAUSED_TO_READY\n");
            /* Clean up resources */
            if (priv->video_caps) {
                gst_caps_unref(priv->video_caps);
                priv->video_caps = NULL;
            }
            if (priv->audio_caps) {
                gst_caps_unref(priv->audio_caps);
                priv->audio_caps = NULL;
            }
            if (priv->codec_data) {
                gst_buffer_unref(priv->codec_data);
                priv->codec_data = NULL;
            }
            break;
        case GST_STATE_CHANGE_READY_TO_NULL:
            g_print("GST_STATE_CHANGE_READY_TO_NULL\n");
            g_free(priv->location);
            priv->location = NULL;
            break;
        default:
            break;
    }

    ret = GST_ELEMENT_CLASS(media_demux_parent_class)->change_state(element, transition);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_print("State change failed\n");
    }

    return ret;
}

/* Class initialization function */
static void media_demux_class_init(MediaDemuxClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);

    gobject_class->set_property = media_demux_set_property;
    gobject_class->get_property = media_demux_get_property;

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
        "MediaDemux",
        "Demuxer",
        "Custom MP4 Demuxer with my_demux video caps implementation",
        "Your Name <your.email@example.com>"
    );

    gst_element_class_add_static_pad_template(gstelement_class, &video_src_template);
    gst_element_class_add_static_pad_template(gstelement_class, &audio_src_template);

    gstelement_class->change_state = GST_DEBUG_FUNCPTR(media_demux_change_state);
}

/* Instance initialization function */
static void media_demux_init(MediaDemux *demux) {
    MediaDemuxPrivate *priv = media_demux_get_instance_private(demux);

    priv->location = NULL;
    priv->fmt_ctx = NULL;
    priv->started = FALSE;
    priv->is_demuxing = FALSE;
    priv->video_caps = NULL;
    priv->audio_caps = NULL;
    priv->video_stream_idx = -1;
    priv->audio_stream_idx = -1;
    priv->is_adts = FALSE;
    priv->mpeg_version = 2;  // Default MPEG version
    priv->codec_data = NULL;
    priv->profile = NULL;
    priv->level = 0;
    priv->group_id = g_random_int();  // Initialize group_id with random value
}

/* Plugin initialization function */
gboolean media_demux_plugin_init(GstPlugin *plugin) {
    return gst_element_register(plugin, "media_demux", GST_RANK_NONE, MEDIA_TYPE_DEMUX);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    media_demux,
    "Custom MP4 Demuxer with my_demux video caps implementation",
    media_demux_plugin_init,
    "1.0",
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/"
)

// audio_demux2.c

#define PACKAGE "my_plugin"

#include "audio_demux2.h"
#include <gst/gst.h>
#include <gst/gstsegment.h>
#include <gst/audio/audio.h>
#include <gst/gstutils.h> // Include this header for gst_util_create_random_id
#include <libavformat/avformat.h>
#include <glib-object.h>
#include <libavcodec/avcodec.h>

// Define the private struct in the .c file
typedef struct _AudioDemux2Private {
    AVFormatContext *fmt_ctx;
    gboolean started;
    GList *audio_src_pads; // List of dynamic audio source pads
    gchar *location;
    GstTask *task;
    GRecMutex *task_lock;
    guint32 group_id; // For stream-start event group-id
    gboolean stop_task;
} AudioDemux2Private;

struct _AudioDemux2
{
    GstElement element;
};


// GObject boilerplate
G_DEFINE_TYPE_WITH_PRIVATE(AudioDemux2, audio_demux2, GST_TYPE_ELEMENT)

// Function prototypes
static void audio_demux2_set_property(GObject *object, guint prop_id,
                                      const GValue *value, GParamSpec *pspec);
static void audio_demux2_get_property(GObject *object, guint prop_id,
                                      GValue *value, GParamSpec *pspec);
static GstStateChangeReturn audio_demux2_change_state(GstElement *element,
                                                      GstStateChange transition);
static void audio_demux2_loop(AudioDemux2 *demux);
static GstPad* create_and_add_pad(AudioDemux2 *demux, AVStream *stream);

enum {
    PROP_0,
    PROP_LOCATION,
};

// Pad template definition, using GST_PAD_SOMETIMES for dynamic pads
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "audio_%u",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY // We will set the actual caps at runtime
);

static void audio_demux2_class_init(AudioDemux2Class *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);

    gobject_class->set_property = audio_demux2_set_property;
    gobject_class->get_property = audio_demux2_get_property;

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
        "AudioDemux2",
        "Demuxer/Audio",
        "Custom FFmpeg-based Audio Demuxer with dynamic pads",
        "Your Name <your.email@example.com>"
    );

    gst_element_class_add_static_pad_template(gstelement_class, &src_template);

    gstelement_class->change_state =
        GST_DEBUG_FUNCPTR(audio_demux2_change_state);
}

static void audio_demux2_init(AudioDemux2 *demux) {
    AudioDemux2Private *priv = audio_demux2_get_instance_private(demux);

    priv->fmt_ctx = NULL;
    priv->started = FALSE;
    priv->audio_src_pads = NULL;
    priv->location = NULL;
    priv->task = NULL;
    priv->task_lock = g_new0(GRecMutex, 1);
    g_rec_mutex_init(priv->task_lock);

    // Generate group-id
    priv->group_id = g_random_int();
    priv->stop_task = FALSE;
}

static void audio_demux2_set_property(GObject *object, guint prop_id,
                                      const GValue *value, GParamSpec *pspec) {
    AudioDemux2 *demux = AUDIO_DEMUX2(object);
    AudioDemux2Private *priv = audio_demux2_get_instance_private(demux);
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

static void audio_demux2_get_property(GObject *object, guint prop_id,
                                      GValue *value, GParamSpec *pspec) {
    AudioDemux2 *demux = AUDIO_DEMUX2(object);
    AudioDemux2Private *priv = audio_demux2_get_instance_private(demux);
    switch (prop_id) {
        case PROP_LOCATION:
            g_value_set_string(value, priv->location);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

// Function to create and add a dynamic pad
static GstPad* create_and_add_pad(AudioDemux2 *demux, AVStream *stream) {
    AudioDemux2Private *priv = audio_demux2_get_instance_private(demux);
    GstPadTemplate *template;
    gchar *pad_name;
    GstPad *pad;

    template = gst_static_pad_template_get(&src_template);
    pad_name = g_strdup_printf("audio_%u", stream->index);
    pad = gst_pad_new_from_template(template, pad_name);
    gst_object_unref(template);
    g_free(pad_name);

    if (!pad) {
        g_print("Failed to create pad from template\n");
        return NULL;
    }

    if (!gst_element_add_pad(GST_ELEMENT(demux), pad)) {
        g_print("Failed to add pad to element\n");
        gst_object_unref(pad);
        return NULL;
    }

    gst_pad_set_active(pad, TRUE);

    // Send stream-start event, set group-id
    gchar *stream_id = gst_pad_create_stream_id(pad, GST_ELEMENT(demux), pad_name);
    GstEvent *stream_start_event = gst_event_new_stream_start(stream_id);
    g_free(stream_id);

    GstStructure *structure = gst_event_writable_structure(stream_start_event);
    gst_structure_set(structure, "group-id", G_TYPE_UINT, priv->group_id, NULL);
    gst_pad_push_event(pad, stream_start_event);

    // Set Caps based on codec parameters
    GstCaps *caps = NULL;
    AVCodecParameters *codecpar = stream->codecpar;
    if (codecpar->codec_id == AV_CODEC_ID_AAC) {
        caps = gst_caps_new_simple(
            "audio/mpeg",
            "mpegversion", G_TYPE_INT, 4,
            "stream-format", G_TYPE_STRING, "raw",
            "channels", G_TYPE_INT, codecpar->ch_layout.nb_channels,
            "rate", G_TYPE_INT, codecpar->sample_rate,
            NULL
        );
    } else if (codecpar->codec_id == AV_CODEC_ID_MP3) {
        caps = gst_caps_new_simple(
            "audio/mpeg",
            "mpegversion", G_TYPE_INT, 1,
            "layer", G_TYPE_INT, 3,
            "channels", G_TYPE_INT, codecpar->ch_layout.nb_channels,
            "rate", G_TYPE_INT, codecpar->sample_rate,
            NULL
        );
    } else {
        // Other formats, use audio/x-raw
        const gchar *format = NULL;
        switch (codecpar->format) {
            case AV_SAMPLE_FMT_U8:
                format = "U8";
                break;
            case AV_SAMPLE_FMT_S16:
            case AV_SAMPLE_FMT_S16P:
                format = "S16LE";
                break;
            case AV_SAMPLE_FMT_S32:
            case AV_SAMPLE_FMT_S32P:
                format = "S32LE";
                break;
            case AV_SAMPLE_FMT_FLT:
            case AV_SAMPLE_FMT_FLTP:
                format = "F32LE";
                break;
            default:
                format = "S16LE";
                break;
        }
        caps = gst_caps_new_simple(
            "audio/x-raw",
            "format", G_TYPE_STRING, format,
            "layout", G_TYPE_STRING, "interleaved",
            "channels", G_TYPE_INT, codecpar->ch_layout.nb_channels,
            "rate", G_TYPE_INT, codecpar->sample_rate,
            NULL
        );
    }

    // Set pad caps
    if (!gst_pad_set_caps(pad, caps)) {
        g_printerr("Failed to set caps on pad\n");
    } else {
        g_print("Successfully set caps on pad\n");
    }

    // Push caps event
    GstEvent *caps_event = gst_event_new_caps(caps);
    gst_pad_push_event(pad, caps_event);
    gst_caps_unref(caps);

    // Push segment event
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    GstEvent *segment_event = gst_event_new_segment(&segment);
    gst_pad_push_event(pad, segment_event);

    // Add pad to the list
    priv->audio_src_pads = g_list_append(priv->audio_src_pads, pad);

    // Emit pad-added signal
    g_signal_emit_by_name(demux, "pad-added", pad);

    return pad;
}

// Demux loop function running in a separate thread
static void audio_demux2_loop(AudioDemux2 *demux) {
    AudioDemux2Private *priv = audio_demux2_get_instance_private(demux);

    // Open input file
    if (avformat_open_input(&priv->fmt_ctx, priv->location, NULL, NULL) != 0) {
        g_print("Failed to open input file: %s\n", priv->location);
        return;
    }

    // Find stream info
    if (avformat_find_stream_info(priv->fmt_ctx, NULL) < 0) {
        g_print("Failed to find stream information\n");
        avformat_close_input(&priv->fmt_ctx);
        return;
    }

    // Loop through all audio streams, create dynamic pads
    for (unsigned int i = 0; i < priv->fmt_ctx->nb_streams; i++) {
        AVStream *stream = priv->fmt_ctx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            create_and_add_pad(demux, stream);
        }
    }

    // Start reading and pushing data
    AVPacket packet;
    av_init_packet(&packet);

    while (!priv->stop_task && av_read_frame(priv->fmt_ctx, &packet) >= 0) {
        AVStream *stream = priv->fmt_ctx->streams[packet.stream_index];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            // Find corresponding pad
            GstPad *pad = NULL;
            GList *iter;
            gchar *pad_name = g_strdup_printf("audio_%u", packet.stream_index);
            for (iter = priv->audio_src_pads; iter != NULL; iter = iter->next) {
                GstPad *temp_pad = GST_PAD(iter->data);
                if (g_strcmp0(gst_pad_get_name(temp_pad), pad_name) == 0) {
                    pad = temp_pad;
                    break;
                }
            }
            g_free(pad_name);
            if (pad) {
                // Create GstBuffer
                GstBuffer *buffer = gst_buffer_new_allocate(NULL, packet.size, NULL);
                GstMapInfo map;
                gst_buffer_map(buffer, &map, GST_MAP_WRITE);
                memcpy(map.data, packet.data, packet.size);
                gst_buffer_unmap(buffer, &map);

                // Set timestamps
                AVRational time_base = stream->time_base;
                guint64 pts = (packet.pts == AV_NOPTS_VALUE) ? 0 : packet.pts;
                guint64 dts = (packet.dts == AV_NOPTS_VALUE) ? 0 : packet.dts;

                GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(pts,
                    GST_SECOND * time_base.num, time_base.den);
                GST_BUFFER_DTS(buffer) = gst_util_uint64_scale(dts,
                    GST_SECOND * time_base.num, time_base.den);
                GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(packet.duration,
                    GST_SECOND * time_base.num, time_base.den);

                // Push buffer
                GstFlowReturn ret = gst_pad_push(pad, buffer);
                if (ret != GST_FLOW_OK) {
                    g_print("Failed to push buffer: %d\n", ret);
                    break;
                }
            }
        }
        av_packet_unref(&packet);
    }

    // Send EOS event
    GList *iter;
    for (iter = priv->audio_src_pads; iter != NULL; iter = iter->next) {
        GstPad *pad = GST_PAD(iter->data);
        gst_pad_push_event(pad, gst_event_new_eos());
    }

    // Close input file
    avformat_close_input(&priv->fmt_ctx);
    priv->fmt_ctx = NULL;
}

static GstStateChangeReturn audio_demux2_change_state(GstElement *element,
                                                      GstStateChange transition) {
    AudioDemux2 *demux = AUDIO_DEMUX2(element);
    AudioDemux2Private *priv = audio_demux2_get_instance_private(demux);
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

    switch (transition) {
        case GST_STATE_CHANGE_NULL_TO_READY:
            // Initialize resources
            if (priv->location == NULL) {
                g_print("Location property is not set\n");
                return GST_STATE_CHANGE_FAILURE;
            }
            break;
        case GST_STATE_CHANGE_READY_TO_PAUSED:
            // Start task
            priv->started = TRUE;
            priv->stop_task = FALSE;
            if (!priv->task) {
                priv->task = gst_task_new((GstTaskFunction)audio_demux2_loop,
                                          demux, NULL);
                gst_task_set_lock(priv->task, priv->task_lock);
            }
            gst_task_start(priv->task);
            break;
        case GST_STATE_CHANGE_PAUSED_TO_READY:
            // Stop task
            priv->started = FALSE;
            priv->stop_task = TRUE;
            if (priv->task) {
                gst_task_stop(priv->task);
                gst_task_join(priv->task);
                g_clear_object(&priv->task);
            }
            // Remove and free all dynamic pads
            GList *iter;
            for (iter = priv->audio_src_pads; iter != NULL; iter = iter->next) {
                GstPad *pad = GST_PAD(iter->data);
                gst_element_remove_pad(GST_ELEMENT(demux), pad);
                gst_object_unref(pad);
            }
            g_list_free(priv->audio_src_pads);
            priv->audio_src_pads = NULL;
            // Close input file
            if (priv->fmt_ctx) {
                avformat_close_input(&priv->fmt_ctx);
                priv->fmt_ctx = NULL;
            }
            break;
        default:
            break;
    }

    ret = GST_ELEMENT_CLASS(audio_demux2_parent_class)
              ->change_state(element, transition);
    return ret;
}

// Plugin initialization function
gboolean audio_demux2_plugin_init(GstPlugin *plugin) {
    return gst_element_register(plugin, "audio_demux2", GST_RANK_NONE,
                                AUDIO_TYPE_DEMUX2);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    audio_demux2,
    "Custom Audio Demuxer with dynamic pads using FFmpeg",
    audio_demux2_plugin_init,
    "1.0",
    "LGPL",
    "GStreamer",
    "https://gstreamer.freedesktop.org/"
)
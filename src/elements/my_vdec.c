// Start of Selection
#define PACKAGE "my_plugin"
#include "my_vdec.h"
#include <gst/gst.h>
#include <libavcodec/avcodec.h>
#include <glib.h>
#include <pthread.h>

// Define maximum queue size to prevent excessive memory usage
#define MAX_QUEUE_SIZE 30

struct _MyVdec {
    GstElement element;
};

typedef struct _MyVdecPrivate {
    AVCodecContext *codec_ctx;    // Codec context
    pthread_t decode_thread;      // Decode thread
    gboolean stop;                // Stop flag
    GstPad *sink_pad;             // Sink pad, used for receiving data
    GstPad *src_pad;              // Source pad, used for sending decoded data
    GMutex mutex;                 // Mutex for thread safety
    GCond cond;                   // Condition variable for thread synchronization
    GQueue *packet_queue;         // Queue to store received packets
    GstCaps *caps;                // Negotiated capabilities
} MyVdecPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(MyVdec, my_vdec, GST_TYPE_ELEMENT)

// Define static pad templates
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
);

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
);

// Declare chain function
static GstFlowReturn my_vdec_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer);

// Convert AVFrame to GstBuffer and push downstream
static gboolean push_decoded_buffer(MyVdec *vdec, AVFrame *frame) {
    MyVdecPrivate *priv = my_vdec_get_instance_private(vdec);
    GstBuffer *buffer;
    GstMapInfo map;
    guint size;

    // Calculate the size of raw video data (assuming YUV420 format)
    size = frame->width * frame->height * 3 / 2;

    // Allocate a new GstBuffer
    buffer = gst_buffer_new_allocate(NULL, size, NULL);
    if (!buffer) {
        g_print("Failed to allocate GstBuffer\n");
        return FALSE;
    }

    // Map the buffer for writing
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        g_print("Failed to map GstBuffer\n");
        gst_buffer_unref(buffer);
        return FALSE;
    }

    // Copy video data from AVFrame to GstBuffer
    memcpy(map.data, frame->data[0], frame->width * frame->height); // Y
    memcpy(map.data + frame->width * frame->height, frame->data[1], frame->width * frame->height / 4); // U
    memcpy(map.data + frame->width * frame->height * 5 / 4, frame->data[2], frame->width * frame->height / 4); // V
    gst_buffer_unmap(buffer, &map);

    // Set buffer metadata (timestamps, etc.) if available
    // For example:
    // GST_BUFFER_PTS(buffer) = ...;
    // GST_BUFFER_DTS(buffer) = ...;

    // Push the buffer downstream
    GstFlowReturn ret = gst_pad_push(priv->src_pad, buffer);
    if (ret != GST_FLOW_OK) {
        g_print("Failed to push buffer downstream\n");
        gst_buffer_unref(buffer);
        return FALSE;
    }

    return TRUE;
}

// Decode thread function
static gpointer decode_thread_func(gpointer data) {
    MyVdec *vdec = MY_VDEC(data);
    MyVdecPrivate *priv = my_vdec_get_instance_private(vdec);
    AVFrame *frame = NULL;
    AVPacket *packet = NULL;
    const AVCodec *codec = NULL;

    // Determine decoder type based on negotiated caps
    if (priv->caps) {
        GstStructure *structure = gst_caps_get_structure(priv->caps, 0);
        const gchar *media_type = gst_structure_get_name(structure);

        if (g_str_has_prefix(media_type, "video/x-h264")) {
            codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        }
        // Additional codec types can be added as needed
        else {
            g_print("Unsupported codec type: %s\n", media_type);
            return NULL;
        }
    } else {
        g_print("Missing caps, unable to determine codec type\n");
        return NULL;
    }

    if (!codec) {
        g_print("Decoder for the specified codec not found\n");
        return NULL;
    }

    priv->codec_ctx = avcodec_alloc_context3(codec);
    if (!priv->codec_ctx) {
        g_print("Failed to allocate AVCodecContext\n");
        return NULL;
    }

    if (avcodec_open2(priv->codec_ctx, codec, NULL) < 0) {
        g_print("Failed to open decoder\n");
        avcodec_free_context(&priv->codec_ctx);
        return NULL;
    }

    frame = av_frame_alloc();
    if (!frame) {
        g_print("Failed to allocate AVFrame\n");
        avcodec_free_context(&priv->codec_ctx);
        return NULL;
    }

    while (TRUE) {
        g_mutex_lock(&priv->mutex);
        while (g_queue_is_empty(priv->packet_queue) && !priv->stop) {
            g_cond_wait(&priv->cond, &priv->mutex);
        }
        if (priv->stop && g_queue_is_empty(priv->packet_queue)) {
            g_mutex_unlock(&priv->mutex);
            break;
        }

        packet = g_queue_pop_head(priv->packet_queue);
        g_mutex_unlock(&priv->mutex);

        if (!packet)
            continue;

        // Send packet to decoder
        if (avcodec_send_packet(priv->codec_ctx, packet) < 0) {
            g_print("Failed to send packet to decoder\n");
            av_packet_free(&packet);
            continue;
        }

        av_packet_free(&packet);

        // Receive decoded frame
        while (avcodec_receive_frame(priv->codec_ctx, frame) == 0) {
            // Push decoded frame downstream
            if (!push_decoded_buffer(vdec, frame)) {
                g_print("Failed to push decoded frame downstream\n");
            }
        }
    }

    av_frame_free(&frame);
    avcodec_free_context(&priv->codec_ctx);
    return NULL;
}

// Chain function implementation
static GstFlowReturn my_vdec_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer) {
    MyVdec *vdec = MY_VDEC(parent);
    MyVdecPrivate *priv = my_vdec_get_instance_private(vdec);
    AVPacket *packet = NULL;
    GstMapInfo map;

    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        g_print("Failed to map GstBuffer\n");
        return GST_FLOW_ERROR;
    }

    // Allocate and copy data to AVPacket
    packet = av_packet_alloc();
    if (!packet) {
        g_print("Failed to allocate AVPacket\n");
        gst_buffer_unmap(buffer, &map);
        return GST_FLOW_ERROR;
    }

    packet->data = g_malloc(map.size);
    memcpy(packet->data, map.data, map.size);
    packet->size = map.size;
    gst_buffer_unmap(buffer, &map);

    // Get caps if not already obtained
    if (!priv->caps) {
        priv->caps = gst_pad_get_current_caps(pad);
    }

    // Lock mutex and access queue
    g_mutex_lock(&priv->mutex);
    if (g_queue_get_length(priv->packet_queue) >= MAX_QUEUE_SIZE) {
        g_print("Packet queue is full, dropping packet\n");
        g_mutex_unlock(&priv->mutex);
        gst_buffer_unref(buffer);
        av_packet_free(&packet);
        return GST_FLOW_FLUSHING;
    }

    // Push packet to queue
    g_queue_push_tail(priv->packet_queue, packet);
    g_cond_signal(&priv->cond);
    g_mutex_unlock(&priv->mutex);

    gst_buffer_unref(buffer);
    return GST_FLOW_OK;
}

// Class initialization function
static void my_vdec_class_init(MyVdecClass *klass) {
    GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);

    // Set element metadata
    gst_element_class_set_static_metadata(gstelement_class,
        "My Video Decoder",
        "Decoder/Video",
        "Decode video frames using avcodec, supporting multiple encoding formats",
        "Your Name <youremail@example.com>"
    );

    // Add pad templates to the element class
    gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&sink_template));
    gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&src_template));

    // 注释掉错误的函数调用，并将添加 Pads 的逻辑移到实例初始化函数中
    /*
    // Set chain function
    GstPad *sink_pad = gst_pad_new_from_static_template(&sink_template, "sink");
    gst_pad_set_chain_function(sink_pad, GST_DEBUG_FUNCPTR(my_vdec_chain));
    gst_element_class_add_pad(gstelement_class, sink_pad);

    GstPad *src_pad = gst_pad_new_from_static_template(&src_template, "src");
    gst_element_class_add_pad(gstelement_class, src_pad);
    */
}

// Instance initialization function
static void my_vdec_init(MyVdec *vdec) {
    MyVdecPrivate *priv = my_vdec_get_instance_private(vdec);

    // Initialize private member variables
    priv->codec_ctx = NULL;
    priv->stop = FALSE;
    priv->caps = NULL;

    // 创建并添加 sink pad
    priv->sink_pad = gst_pad_new_from_static_template(&sink_template, "sink");
    gst_pad_set_chain_function(priv->sink_pad, GST_DEBUG_FUNCPTR(my_vdec_chain));
    gst_element_add_pad(GST_ELEMENT(vdec), priv->sink_pad);

    // 创建并添加 src pad
    priv->src_pad = gst_pad_new_from_static_template(&src_template, "src");
    gst_element_add_pad(GST_ELEMENT(vdec), priv->src_pad);

    // Initialize mutex and condition variables
    g_mutex_init(&priv->mutex);
    g_cond_init(&priv->cond);

    // Initialize packet queue
    priv->packet_queue = g_queue_new();

    // Create decode thread
    if (pthread_create(&priv->decode_thread, NULL, decode_thread_func, vdec) != 0) {
        g_print("Failed to create decode thread\n");
    }
}

// Plugin initialization function
gboolean my_vdec_plugin_init(GstPlugin *plugin) {
    return gst_element_register(plugin, "my_vdec", GST_RANK_NONE, MY_TYPE_VDEC);
}

// Define plugin
GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    my_vdec_plugin,
    "Plugin for decoding video frames using avcodec",
    my_vdec_plugin_init,
    "1.0",
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/"
)

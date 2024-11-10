#define PACKAGE "my_plugin"
#include <gst/gst.h>
#include <libavcodec/bsf.h>
#include "my_h264parse.h"

// Private data structure
typedef struct _MyH264ParsePrivate {
    AVBSFContext *bsf_ctx;  // FFmpeg Bitstream Filter context
    GstPad *sinkpad;
    GstPad *srcpad;
} MyH264ParsePrivate;

// Define type and associate private data
G_DEFINE_TYPE_WITH_PRIVATE(MyH264Parse, my_h264parse, GST_TYPE_ELEMENT)

// Chain function to handle incoming data on the sink pad
static GstFlowReturn my_h264parse_chain(GstPad *pad, GstObject *parent, GstBuffer *buf) {
    MyH264Parse *parse = MY_H264PARSE(parent);
    MyH264ParsePrivate *priv = my_h264parse_get_instance_private(parse);
    GstFlowReturn ret = GST_FLOW_OK;
    GstBuffer *outbuf = NULL;

    if (!priv->bsf_ctx) {
        GST_ERROR_OBJECT(parse, "Bitstream Filter context is not initialized");
        return GST_FLOW_ERROR;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
        GST_ERROR_OBJECT(parse, "Failed to map GstBuffer");
        return GST_FLOW_ERROR;
    }

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = (uint8_t *)map.data;
    pkt.size = map.size;

    if (av_bsf_send_packet(priv->bsf_ctx, &pkt) < 0) {
        GST_ERROR_OBJECT(parse, "Failed to send packet to BSF");
        gst_buffer_unmap(buf, &map);
        return GST_FLOW_ERROR;
    }

    gst_buffer_unmap(buf, &map);

    if (av_bsf_receive_packet(priv->bsf_ctx, &pkt) == 0) {
        // Allocate a new GstBuffer with the filtered data
        outbuf = gst_buffer_new_and_alloc(pkt.size);
        gst_buffer_fill(outbuf, 0, pkt.data, pkt.size);
        av_packet_unref(&pkt);

        // Push the filtered buffer to the src pad
        ret = gst_pad_push(priv->srcpad, outbuf);
    }

    return ret;
}

// Finalize function
static void my_h264parse_finalize(GObject *object) {
    MyH264Parse *parse = MY_H264PARSE(object);
    MyH264ParsePrivate *priv = my_h264parse_get_instance_private(parse);

    if (priv->bsf_ctx) {
        av_bsf_free(&priv->bsf_ctx);
    }

    G_OBJECT_CLASS(my_h264parse_parent_class)->finalize(object);
}

// State change function
static GstStateChangeReturn my_h264parse_change_state(GstElement *element, GstStateChange transition) {
    MyH264Parse *parse = MY_H264PARSE(element);
    MyH264ParsePrivate *priv = my_h264parse_get_instance_private(parse);
    GstStateChangeReturn ret;

    switch (transition) {
        case GST_STATE_CHANGE_NULL_TO_READY:
            // Initialize BSF context
            {
                const AVBitStreamFilter *filter = av_bsf_get_by_name("h264_mp4toannexb");
                if (!filter) {
                    GST_ERROR_OBJECT(parse, "Failed to get h264_mp4toannexb Bitstream Filter");
                    return GST_STATE_CHANGE_FAILURE;
                }
                if (av_bsf_alloc(filter, &priv->bsf_ctx) < 0) {
                    GST_ERROR_OBJECT(parse, "Failed to allocate BSF context");
                    priv->bsf_ctx = NULL;
                    return GST_STATE_CHANGE_FAILURE;
                }

                if (av_bsf_init(priv->bsf_ctx) < 0) {
                    GST_ERROR_OBJECT(parse, "Failed to initialize BSF context");
                    av_bsf_free(&priv->bsf_ctx);
                    return GST_STATE_CHANGE_FAILURE;
                }
            }
            break;
        case GST_STATE_CHANGE_READY_TO_NULL:
            // Free BSF context
            if (priv->bsf_ctx) {
                av_bsf_free(&priv->bsf_ctx);
                priv->bsf_ctx = NULL;
            }
            break;
        default:
            break;
    }

    ret = GST_ELEMENT_CLASS(my_h264parse_parent_class)->change_state(element, transition);
    return ret;
}

// Class initialization function
static void my_h264parse_class_init(MyH264ParseClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

    // Set finalize function
    gobject_class->finalize = my_h264parse_finalize;

    // Set state change function
    element_class->change_state = my_h264parse_change_state;

    // Set element metadata
    gst_element_class_set_static_metadata(element_class,
        "My H264 Parser",
        "Filter/Parser",
        "Converts H264 AVCC format to Annex-B format using FFmpeg's BSF",
        "Your Name <youremail@example.com>");

    // Define sink pad template
    GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink",
        GST_PAD_SINK,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS("video/x-h264, stream-format=(string)avc"));

    // Define src pad template
    GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS("video/x-h264, stream-format=(string)annex-b"));

    // Add pad templates to the element class
    gst_element_class_add_pad_template(element_class, &sink_template);
    gst_element_class_add_pad_template(element_class, &src_template);
}

// Instance initialization function
static void my_h264parse_init(MyH264Parse *parse) {
    MyH264ParsePrivate *priv = my_h264parse_get_instance_private(parse);

    GstPadTemplate *sink_templ = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(parse), "sink");
    priv->sinkpad = gst_pad_new_from_template(sink_templ, "sink");
    gst_pad_set_chain_function(priv->sinkpad, GST_DEBUG_FUNCPTR(my_h264parse_chain));
    gst_element_add_pad(GST_ELEMENT(parse), priv->sinkpad);

    GstPadTemplate *src_templ = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(parse), "src");
    priv->srcpad = gst_pad_new_from_template(src_templ, "src");
    gst_element_add_pad(GST_ELEMENT(parse), priv->srcpad);
}

// Plugin initialization function
static gboolean plugin_init(GstPlugin *plugin) {
    return gst_element_register(plugin, "my_h264parse", GST_RANK_NONE, MY_TYPE_H264PARSE);
}

// Define plugin
GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    my_h264parse_plugin,
    "Converts H264 AVCC format to Annex-B format using FFmpeg's BSF",
    plugin_init,
    "1.0",
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/"
)

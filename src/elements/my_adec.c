    // Start of Selection
    #define PACKAGE "my_plugin"
    #include "my_adec.h"
    #include <gst/gst.h>
    #include <libavcodec/avcodec.h>
    #include <glib.h>
    #include <pthread.h>
    
    // Define maximum queue size to prevent excessive memory usage
    #define MAX_QUEUE_SIZE 30
    
    typedef struct _MyAdecPrivate {
        AVCodecContext *codec_ctx;    // Codec context
        pthread_t decode_thread;      // Decode thread
        gboolean stop;                // Stop flag
        GstPad *sink_pad;             // Sink pad
        GstPad *src_pad;              // Source pad
        GMutex mutex;                 // Mutex for thread safety
        GCond cond;                   // Condition variable
        GQueue *packet_queue;         // Queue to hold incoming packets
        GstCaps *caps;                // Negotiated capabilities
    } MyAdecPrivate;
    
    G_DEFINE_TYPE_WITH_PRIVATE(MyAdec, my_adec, GST_TYPE_ELEMENT)
    
    // Forward declaration of chain function
    static GstFlowReturn my_adec_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer);
    
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
        GST_STATIC_CAPS("audio/raw")
    );
    
    // Function to convert AVFrame to GstBuffer and push it downstream
    static gboolean push_decoded_buffer(MyAdec *adec, AVFrame *frame) {
        MyAdecPrivate *priv = my_adec_get_instance_private(adec);
        GstBuffer *buffer;
        GstMapInfo map;
        guint size;
    
        // Calculate the size of the raw audio data
        size = frame->nb_samples * av_get_bytes_per_sample(priv->codec_ctx->sample_fmt) * priv->codec_ctx->ch_layout.nb_channels;
    
        // Allocate a new GstBuffer with the required size
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
    
        // Copy raw audio data from AVFrame to GstBuffer
        memcpy(map.data, frame->data[0], size);
        gst_buffer_unmap(buffer, &map);
    
        // Set buffer metadata (timestamps, etc.) if available
        // Example:
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
        MyAdec *adec = MY_ADEC(data);
        MyAdecPrivate *priv = my_adec_get_instance_private(adec);
        AVFrame *frame = NULL;
        AVPacket *packet = NULL;
        const AVCodec *codec = NULL;
    
        // Determine codec type based on negotiated caps
        enum AVCodecID codec_type = AV_CODEC_ID_NONE;
        if (priv->caps) {
            const gchar *media_type = gst_structure_get_name(gst_caps_get_structure(priv->caps, 0));
            if (g_str_has_suffix(media_type, "aac")) {
                codec_type = AV_CODEC_ID_AAC;
            } else if (g_str_has_suffix(media_type, "mp3")) {
                codec_type = AV_CODEC_ID_MP3;
            }
            // Add more codec types as needed
        }
    
        // Initialize decoder based on codec type
        switch (codec_type) {
            case AV_CODEC_ID_AAC:
                codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
                break;
            case AV_CODEC_ID_MP3:
                codec = avcodec_find_decoder(AV_CODEC_ID_MP3);
                break;
            default:
                g_print("Unsupported codec type\n");
                return NULL;
        }
    
        if (!codec) {
            g_print("Decoder not found for the specified codec\n");
            return NULL;
        }
    
        priv->codec_ctx = avcodec_alloc_context3(codec);
        if (!priv->codec_ctx) {
            g_print("Failed to allocate codec context\n");
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
    
            if (!packet) {
                continue;
            }
    
            // Send packet to decoder
            if (avcodec_send_packet(priv->codec_ctx, packet) < 0) {
                g_print("Failed to send packet to decoder\n");
                av_packet_free(&packet);
                continue;
            }
    
            av_packet_free(&packet);
    
            // Receive frames from decoder
            while (avcodec_receive_frame(priv->codec_ctx, frame) == 0) {
                // Push decoded frame downstream
                if (!push_decoded_buffer(adec, frame)) {
                    g_print("Failed to push decoded buffer\n");
                    break;
                }
            }
        }
    
        av_frame_free(&frame);
        avcodec_close(priv->codec_ctx);
        avcodec_free_context(&priv->codec_ctx);
    
        return NULL;
    }
    
    // Clean up function
    static void my_adec_finalize(GObject *object) {
        MyAdec *adec = MY_ADEC(object);
        MyAdecPrivate *priv = my_adec_get_instance_private(adec);
    
        // Stop decode thread
        g_mutex_lock(&priv->mutex);
        priv->stop = TRUE;
        g_cond_signal(&priv->cond);
        g_mutex_unlock(&priv->mutex);
        pthread_join(priv->decode_thread, NULL);
    
        // Free packet queue
        g_mutex_lock(&priv->mutex);
        while (!g_queue_is_empty(priv->packet_queue)) {
            AVPacket *packet = g_queue_pop_head(priv->packet_queue);
            av_packet_free(&packet);
        }
        g_queue_free(priv->packet_queue);
        g_mutex_unlock(&priv->mutex);
    
        g_mutex_clear(&priv->mutex);
        g_cond_clear(&priv->cond);
    
        G_OBJECT_CLASS(my_adec_parent_class)->finalize(object);
    }
    
    // Class initialization function
    static void my_adec_class_init(MyAdecClass *klass) {
        GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
        gobject_class->finalize = my_adec_finalize;
    
        GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);
        gst_element_class_set_static_metadata(gstelement_class,
            "My Audio Decoder",
            "Codec/Decoder",
            "Decodes various audio streams based on codec type",
            "Your Name <youremail@example.com>");
    
        // Add sink pad template
        GstPadTemplate *sink_pad_template_ptr = gst_static_pad_template_get(&sink_template);
        gst_element_class_add_pad_template(gstelement_class, sink_pad_template_ptr);
    
        // Add src pad template
        GstPadTemplate *src_pad_template_ptr = gst_static_pad_template_get(&src_template);
        gst_element_class_add_pad_template(gstelement_class, src_pad_template_ptr);
    }
    
    // Instance initialization function
    static void my_adec_init(MyAdec *adec) {
        MyAdecPrivate *priv = my_adec_get_instance_private(adec);
    
        priv->codec_ctx = NULL;
        priv->stop = FALSE;
        priv->packet_queue = g_queue_new();
        g_mutex_init(&priv->mutex);
        g_cond_init(&priv->cond);
        priv->caps = NULL;
    
        priv->sink_pad = gst_pad_new_from_static_template(&sink_template, "sink");
        gst_pad_set_chain_function(priv->sink_pad, GST_DEBUG_FUNCPTR(my_adec_chain));
        gst_element_add_pad(GST_ELEMENT(adec), priv->sink_pad);
    
        priv->src_pad = gst_pad_new_from_static_template(&src_template, "src");
        gst_element_add_pad(GST_ELEMENT(adec), priv->src_pad);
    
        // Create decode thread
        if (pthread_create(&priv->decode_thread, NULL, decode_thread_func, adec) != 0) {
            g_print("Failed to create decode thread\n");
        }
    }
    
    // Chain function to handle incoming buffers
    static GstFlowReturn my_adec_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer) {
        MyAdec *adec = MY_ADEC(parent);
        MyAdecPrivate *priv = my_adec_get_instance_private(adec);
    
        AVPacket *packet = av_packet_alloc();
        if (!packet) {
            g_print("Failed to allocate AVPacket\n");
            return GST_FLOW_ERROR;
        }
    
        // Convert GstBuffer to AVPacket
        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            g_print("Failed to map GstBuffer\n");
            gst_buffer_unref(buffer);
            av_packet_free(&packet);
            return GST_FLOW_ERROR;
        }
    
        packet->data = g_malloc(map.size);
        memcpy(packet->data, map.data, map.size);
        packet->size = map.size;
        gst_buffer_unmap(buffer, &map);
    
        // Retrieve and store caps if not already done
        if (!priv->caps) {
            priv->caps = gst_pad_get_current_caps(pad);
        }
    
        // Lock mutex before accessing the queue
        g_mutex_lock(&priv->mutex);
        if (g_queue_get_length(priv->packet_queue) >= MAX_QUEUE_SIZE) {
            g_print("Packet queue is full, dropping packet\n");
            g_mutex_unlock(&priv->mutex);
            gst_buffer_unref(buffer);
            av_packet_free(&packet);
            return GST_FLOW_FLUSHING;
        }
    
        // Push packet to the queue
        g_queue_push_tail(priv->packet_queue, packet);
        g_cond_signal(&priv->cond);
        g_mutex_unlock(&priv->mutex);
    
        gst_buffer_unref(buffer);
        return GST_FLOW_OK;
    }
    
    // Plugin initialization function
    gboolean myadec_plugin_init(GstPlugin *plugin) {
        return gst_element_register(plugin, "myadec", GST_RANK_NONE, MY_TYPE_ADEC);
    }
    
    GST_PLUGIN_DEFINE(
        GST_VERSION_MAJOR,
        GST_VERSION_MINOR,
        myadec,
        "My Audio Decoder Plugin",
        myadec_plugin_init,
        "1.0",
        "LGPL",
        "GStreamer",
        "http://example.com/"
    )

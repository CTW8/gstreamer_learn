#define PACKAGE "my_plugin"
#include "my_videorender.h"
#include <gst/video/video.h>
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdbool.h>

// Define private structure
typedef struct _MyVideoRenderPrivate {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture; // Texture for rendering video frames
    pthread_t render_thread; // Render thread
    pthread_mutex_t frame_mutex; // Mutex for frame access
    GstBuffer *current_frame; // Current video frame
    bool stop_rendering; // Flag to stop rendering thread
} MyVideoRenderPrivate;

struct _MyVideoRender {
    GstElement parent;
};

G_DEFINE_TYPE_WITH_PRIVATE(MyVideoRender, my_videorender, GST_TYPE_ELEMENT)

// Render thread function
static void* render_loop(void *data) {
    MyVideoRender *render = MY_VIDEORENDER(data);
    MyVideoRenderPrivate *priv = my_videorender_get_instance_private(render);

    while (!priv->stop_rendering) {
        pthread_mutex_lock(&priv->frame_mutex);
        if (priv->current_frame) {
            // Map GST buffer to access frame data
            GstMapInfo map;
            if (gst_buffer_map(priv->current_frame, &map, GST_MAP_READ)) {
                // Update texture with new frame data
                SDL_UpdateTexture(priv->texture, NULL, map.data, map.size);
                gst_buffer_unmap(priv->current_frame, &map);
            }
            // Unref current frame
            gst_buffer_unref(priv->current_frame);
            priv->current_frame = NULL;
        }
        pthread_mutex_unlock(&priv->frame_mutex);

        // Clear renderer
        SDL_RenderClear(priv->renderer);

        // Copy texture to renderer
        SDL_RenderCopy(priv->renderer, priv->texture, NULL, NULL);

        // Present renderer content
        SDL_RenderPresent(priv->renderer);

        // Delay to control frame rate
        SDL_Delay(16); // Approximately 60 FPS
    }

    return NULL;
}

// Class initialization function
static void my_videorender_class_init(MyVideoRenderClass *klass) {
    GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);

    // Set element metadata
    gst_element_class_set_static_metadata(gstelement_class,
        "My Video Render",
        "Render/Video",
        "Video rendering using SDL",
        "Your Name <youremail@example.com>"
    );

    // Define and add sink pad template
    GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
        "sink",
        GST_PAD_SINK,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS(
            "video/x-raw, "
            "format=(string)YV12, "
            "width=(int)800, "
            "height=(int)600, "
            "framerate=(fraction)60/1"
        )
    );

    gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&sink_template));
}

// Instance initialization function
static void my_videorender_init(MyVideoRender *render) {
    MyVideoRenderPrivate *priv = my_videorender_get_instance_private(render);

    // Initialize SDL and create window and renderer
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        g_print("SDL could not initialize: %s\n", SDL_GetError());
        return;
    }

    priv->window = SDL_CreateWindow("Video Render",
                                    SDL_WINDOWPOS_UNDEFINED,
                                    SDL_WINDOWPOS_UNDEFINED,
                                    800, 600,
                                    0);
    if (!priv->window) {
        g_print("Could not create window: %s\n", SDL_GetError());
        SDL_Quit();
        return;
    }

    priv->renderer = SDL_CreateRenderer(priv->window, -1, SDL_RENDERER_ACCELERATED);
    if (!priv->renderer) {
        g_print("Could not create renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(priv->window);
        SDL_Quit();
        return;
    }

    // Create texture (assuming YUV format, adjust as needed)
    priv->texture = SDL_CreateTexture(priv->renderer,
                                        SDL_PIXELFORMAT_YV12,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        800, 600);
    if (!priv->texture) {
        g_print("Could not create texture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(priv->renderer);
        SDL_DestroyWindow(priv->window);
        SDL_Quit();
        return;
    }

    // Initialize mutex
    pthread_mutex_init(&priv->frame_mutex, NULL);
    priv->current_frame = NULL;
    priv->stop_rendering = false;

    // Start render thread
    if (pthread_create(&priv->render_thread, NULL, render_loop, render) != 0) {
        g_print("Could not create render thread\n");
        SDL_DestroyTexture(priv->texture);
        SDL_DestroyRenderer(priv->renderer);
        SDL_DestroyWindow(priv->window);
        SDL_Quit();
        pthread_mutex_destroy(&priv->frame_mutex);
        return;
    }
}

// Plugin initialization function
gboolean my_videorender_plugin_init(GstPlugin *plugin) {
    return gst_element_register(plugin, "myvideorender", GST_RANK_NONE, MY_TYPE_VIDEORENDER);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    myvideorender,
    "My Video Render Plugin",
    my_videorender_plugin_init,
    "1.0",
    "LGPL",
    "GStreamer",
    "http://example.com/"
)

#define PACKAGE "my_plugin"
#include "my_audiorender.h"
#include <gst/audio/audio.h>
#include <SDL2/SDL.h>

struct _MyAudioRender {
    GstElement element;
};

// 定义私有结构体
typedef struct _MyAudioRenderPrivate {
    SDL_AudioDeviceID device_id;    // SDL 音频设备ID
    SDL_AudioSpec desired_spec;     // 期望的音频规格
} MyAudioRenderPrivate;


G_DEFINE_TYPE_WITH_PRIVATE(MyAudioRender, my_audiorender, GST_TYPE_ELEMENT)

// 接受sink数据的处理函数
static GstFlowReturn my_audiorender_chain(GstPad *pad, GstObject *parent, GstBuffer *buffer) {
    MyAudioRender *render = MY_AUDIORENDER(parent);
    MyAudioRenderPrivate *priv = my_audiorender_get_instance_private(render);
    GstMapInfo map;

    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        g_printerr("无法映射GstBuffer\n");
        return GST_FLOW_ERROR;
    }

    // 将音频数据发送到SDL音频设备
    if (SDL_QueueAudio(priv->device_id, map.data, map.size) < 0) {
        g_printerr("无法发送音频数据到SDL: %s\n", SDL_GetError());
        gst_buffer_unmap(buffer, &map);
        return GST_FLOW_ERROR;
    }

    gst_buffer_unmap(buffer, &map);
    return GST_FLOW_OK;
}

// 类初始化函数
static void my_audiorender_class_init(MyAudioRenderClass *klass) {
    GstElementClass *gstelement_class = GST_ELEMENT_CLASS(klass);

    // 设置元素的元数据
    gst_element_class_set_static_metadata(gstelement_class,
        "My Audio Render",
        "Render/Audio",
        "使用SDL进行音频渲染",
        "Your Name <youremail@example.com>"
    );

    // 定义并添加sink pad模板
    GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
        "sink",
        GST_PAD_SINK,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS(
            "audio/x-raw, "
            "format=(string)F32LE, "
            "channels=(int)2, "
            "layout=(string)interleaved, "
            "rate=(int)44100"
        )
    );

    gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&sink_template));
}

// 实例初始化函数
static void my_audiorender_init(MyAudioRender *render) {
    MyAudioRenderPrivate *priv = my_audiorender_get_instance_private(render);

    // 初始化 SDL 音频设备
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        g_error("SDL 无法初始化! SDL_Error: %s", SDL_GetError());
    }

    SDL_zero(priv->desired_spec);
    priv->desired_spec.freq = 44100;
    priv->desired_spec.format = AUDIO_F32LSB;
    priv->desired_spec.channels = 2;
    priv->desired_spec.samples = 4096;
    priv->desired_spec.callback = NULL; // 不使用回调
    priv->desired_spec.userdata = NULL;

    priv->device_id = SDL_OpenAudioDevice(NULL, 0, &priv->desired_spec, NULL, 0);
    if (priv->device_id == 0) {
        g_error("无法打开音频设备: %s", SDL_GetError());
    } else {
        SDL_PauseAudioDevice(priv->device_id, 0); // 开始播放
    }

    // 创建并设置sink pad
    GstPadTemplate *pad_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(render), "sink");
    GstPad *sinkpad = gst_pad_new_from_template(pad_template, "sink");
    gst_pad_set_chain_function(sinkpad, GST_DEBUG_FUNCPTR(my_audiorender_chain));
    gst_element_add_pad(GST_ELEMENT(render), sinkpad);
}

// 插件初始化函数
gboolean my_audiorender_plugin_init(GstPlugin *plugin) {
    return gst_element_register(plugin, "myaudiorender", GST_RANK_NONE, MY_TYPE_AUDIORENDER);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    myaudiorender,
    "My Audio Render Plugin",
    my_audiorender_plugin_init,
    "1.0",
    "LGPL",
    "GStreamer",
    "http://example.com/"
)


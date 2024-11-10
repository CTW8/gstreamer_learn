#ifndef MY_AUDIORENDER_H
#define MY_AUDIORENDER_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define MY_TYPE_AUDIORENDER my_audiorender_get_type()
G_DECLARE_FINAL_TYPE(MyAudioRender, my_audiorender, MY, AUDIORENDER, GstElement)
gboolean my_audiorender_plugin_init(GstPlugin *plugin);
G_END_DECLS

#endif // MY_AUDIORENDER_H

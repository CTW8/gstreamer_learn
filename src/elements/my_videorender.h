#ifndef MY_VIDEORENDER_H
#define MY_VIDEORENDER_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define MY_TYPE_VIDEORENDER my_videorender_get_type()
G_DECLARE_FINAL_TYPE(MyVideoRender, my_videorender, MY, VIDEORENDER, GstElement)
gboolean my_videorender_plugin_init(GstPlugin *plugin);
G_END_DECLS

#endif // MY_VIDEORENDER_H

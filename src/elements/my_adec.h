#ifndef MY_ADEC_H
#define MY_ADEC_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define MY_TYPE_ADEC my_adec_get_type()
G_DECLARE_FINAL_TYPE(MyAdec, my_adec, MY, ADEC, GstElement)

gboolean myadec_plugin_init(GstPlugin *plugin);
struct _MyAdec{
    GstElement parent;
};

G_END_DECLS

#endif // MY_ADEC_H

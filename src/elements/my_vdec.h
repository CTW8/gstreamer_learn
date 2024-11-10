#ifndef MY_VDEC_H
#define MY_VDEC_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define MY_TYPE_VDEC my_vdec_get_type()
G_DECLARE_FINAL_TYPE(MyVdec, my_vdec, MY, VDEC, GstElement)
gboolean my_vdec_plugin_init(GstPlugin *plugin);
G_END_DECLS

#endif // MY_VDEC_H

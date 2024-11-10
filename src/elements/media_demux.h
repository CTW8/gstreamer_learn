#ifndef MEDIA_DEMUX_H
#define MEDIA_DEMUX_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define MEDIA_TYPE_DEMUX media_demux_get_type()
G_DECLARE_FINAL_TYPE(MediaDemux, media_demux, MEDIA, DEMUX, GstElement)

gboolean media_demux_plugin_init(GstPlugin *plugin);

G_END_DECLS

#endif // MEDIA_DEMUX_H




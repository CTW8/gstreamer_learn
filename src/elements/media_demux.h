#ifndef MEDIA_DEMUX_H
#define MEDIA_DEMUX_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define MEDIA_TYPE_DEMUX (media_demux_get_type())
G_DECLARE_FINAL_TYPE(MediaDemux, media_demux, MEDIA, DEMUX, GstElement)

/**
 * media_demux_plugin_init:
 * @plugin: 指向插件对象的指针
 *
 * 初始化并注册 `media_demux` 元素到 GStreamer 系统。
 *
 * Returns: TRUE 如果成功注册元素，否则 FALSE。
 */
gboolean media_demux_plugin_init(GstPlugin *plugin);

G_END_DECLS

#endif /* MEDIA_DEMUX_H */
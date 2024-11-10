#ifndef AUDIO_DEMUX_H
#define AUDIO_DEMUX_H

#include <gst/gst.h>
#include <libavformat/avformat.h>

G_BEGIN_DECLS

#define AUDIO_TYPE_DEMUX (audio_demux_get_type())
G_DECLARE_FINAL_TYPE(AudioDemux, audio_demux, AUDIO, DEMUX, GstElement)

struct _AudioDemux {
    GstElement parent;
    gpointer priv;
};

gboolean audio_demux_plugin_init(GstPlugin *plugin);

G_END_DECLS

#endif /* AUDIO_DEMUX_H */

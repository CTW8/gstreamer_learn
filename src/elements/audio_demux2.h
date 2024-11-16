// audio_demux2.h

#ifndef __AUDIO_DEMUX2_H__
#define __AUDIO_DEMUX2_H__

#include <gst/gst.h>

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(AudioDemux2, audio_demux2, AUDIO, DEMUX2, GstElement)


#define AUDIO_TYPE_DEMUX2            (audio_demux2_get_type())
#define AUDIO_DEMUX2(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), AUDIO_TYPE_DEMUX2, AudioDemux2))
#define AUDIO_IS_DEMUX2(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), AUDIO_TYPE_DEMUX2))


gboolean audio_demux2_plugin_init(GstPlugin *plugin);

G_END_DECLS

#endif /* __AUDIO_DEMUX2_H__ */
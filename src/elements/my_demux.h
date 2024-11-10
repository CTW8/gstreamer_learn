#ifndef MY_DEMUX_H
#define MY_DEMUX_H

#include <gst/gst.h>
#include <libavformat/avformat.h>
#include <glib.h>

G_BEGIN_DECLS

#define MY_TYPE_DEMUX (my_demux_get_type())
G_DECLARE_FINAL_TYPE(MyDemux, my_demux, MY, DEMUX, GstElement)

struct _MyDemux {
    GstElement parent;
    AVFormatContext *fmt_ctx;    // FFmpeg format context
    gboolean started;            // Whether the demuxer has started
    gint video_stream_idx;       // Index of video stream
    gint audio_stream_idx;       // Index of audio stream
    GstPad *video_src_pad;       // Video source pad
    GstPad *audio_src_pad;       // Audio source pad

    GstTask *task;               // 线程任务，用于异步处理数据
    GRecMutex *task_lock;        // 任务的锁，用于线程同步
    gchar *location;             // File path
};

gboolean plugin_init(GstPlugin *plugin);

G_END_DECLS

#endif /* MY_DEMUX_H */
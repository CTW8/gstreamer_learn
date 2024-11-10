#ifndef MY_H264PARSE_H
#define MY_H264PARSE_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define MY_TYPE_H264PARSE my_h264parse_get_type()
G_DECLARE_FINAL_TYPE(MyH264Parse, my_h264parse, MY, H264PARSE, GstElement)

struct _MyH264Parse{
    GstElement parent;
};

G_END_DECLS

#endif // MY_H264PARSE_H

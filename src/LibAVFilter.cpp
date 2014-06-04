/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2012-2014 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include "QtAV/LibAVFilter.h"
#include "QtAV/private/Filter_p.h"
#include "QtAV/VideoFrame.h"
#include "QtAV/QtAV_Compat.h"

namespace QtAV {

class LibAVFilterPrivate : public FilterPrivate
{
public:
    LibAVFilterPrivate()
        : filter_graph(0)
        , in_filter_ctx(0)
        , out_filter_ctx(0)
        , pixfmt(QTAV_PIX_FMT_C(NONE))
        , width(0)
        , height(0)
        , avframe(0)
        , filter_frame(0)
        , options_changed(false)
    {
        avfilter_register_all();
    }
    virtual ~LibAVFilterPrivate() {
        avfilter_graph_free(&filter_graph);
        if (avframe) {
            av_frame_free(&avframe);
            avframe = 0;
        }
        if (filter_frame) {
            av_frame_free(&filter_frame);
            filter_frame = 0;
        }
    }

    bool setOptions(const QString& opt) {
        options_changed = options != opt;
        if (options_changed)
            options = opt;
        return true;
    }

    bool push(Frame *frame) {
        VideoFrame *vf = static_cast<VideoFrame*>(frame);
        if (width != vf->width() || height != vf->height() || pixfmt != vf->pixelFormatFFmpeg() || options_changed) {
            width = vf->width();
            height = vf->height();
            pixfmt = (AVPixelFormat)vf->pixelFormatFFmpeg();
            options_changed = false;
            if (!setup()) {
                qWarning("setup filter graph error");
                enabled = false; // skip this filter and avoid crash
                return false;
            }
        }
        Q_ASSERT(avframe);
        avframe->width = vf->width();
        avframe->height = vf->height();
        avframe->format = pixfmt = (AVPixelFormat)vf->pixelFormatFFmpeg();
        for (int i = 0; i < vf->planeCount(); ++i) {
            avframe->data[i] =vf->bits(i);
            avframe->linesize[i] = vf->bytesPerLine(i);
        }
        int ret = av_buffersrc_add_frame_flags(in_filter_ctx, avframe, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret != 0) {
            qWarning("av_buffersrc_add_frame error: %s", av_err2str(ret));
            return false;
        }
        return true;
    }

    bool pull(Frame *f) {
        int ret = av_buffersink_get_frame(out_filter_ctx, filter_frame);
        if (ret < 0) {
            qWarning("av_buffersink_get_frame error: %s", av_err2str(ret));
            return false;
        }
        VideoFrame vf(filter_frame->width, filter_frame->height, VideoFormat(filter_frame->format));
        vf.setBits(filter_frame->data);
        vf.setBytesPerLine(filter_frame->linesize);
        *f = vf.clone();
        av_frame_unref(filter_frame);
        return true;
    }

    bool setup() {
        avfilter_graph_free(&filter_graph);
        filter_graph = avfilter_graph_alloc();
        //QString sws_flags_str;
        QString buffersrc_args = QString("video_size=%1x%2:pix_fmt=%3:time_base=%4/%5:sar=1")
                .arg(width).arg(height).arg(pixfmt).arg(1).arg(AV_TIME_BASE);
        qDebug("buffersrc_args=%s", buffersrc_args.toUtf8().constData());
        AVFilter *buffersrc  = avfilter_get_by_name("buffer");
        Q_ASSERT(buffersrc);
        int ret = avfilter_graph_create_filter(&in_filter_ctx,
                                               buffersrc,
                                               "in", buffersrc_args.toUtf8().constData(), NULL,
                                               filter_graph);
        if (ret < 0) {
            qWarning("Can not create buffer source: %s", av_err2str(ret));
            return false;
        }
        /* buffer video sink: to terminate the filter chain. */
        AVFilter *buffersink = avfilter_get_by_name("buffersink");
        Q_ASSERT(buffersink);
        if ((ret = avfilter_graph_create_filter(&out_filter_ctx, buffersink, "out",
                                           NULL, NULL, filter_graph)) < 0) {
            qWarning("Can not create buffer sink: %s", av_err2str(ret));
            return false;
        }

        /* Endpoints for the filter graph. */
        AVFilterInOut *outputs = avfilter_inout_alloc();
        AVFilterInOut *inputs  = avfilter_inout_alloc();
        outputs->name       = av_strdup("in");
        outputs->filter_ctx = in_filter_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name       = av_strdup("out");
        inputs->filter_ctx = out_filter_ctx;
        inputs->pad_idx    = 0;
        inputs->next       = NULL;

        //avfilter_graph_parse, avfilter_graph_parse2?
        if ((ret = avfilter_graph_parse_ptr(filter_graph, options.toUtf8().constData(),
                                        &inputs, &outputs, NULL)) < 0) {
            qWarning("avfilter_graph_parse_ptr fail: %s", av_err2str(ret));
            return false;
        }
        if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0) {
            qWarning("avfilter_graph_config fail: %s", av_err2str(ret));
            return false;
        }
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        avframe = av_frame_alloc();
        filter_frame = av_frame_alloc();
        return true;
    }


    AVFilterGraph *filter_graph;
    AVFilterContext *in_filter_ctx;
    AVFilterContext *out_filter_ctx;

    AVPixelFormat pixfmt;
    int width, height;
    AVFrame *avframe;
    AVFrame *filter_frame;

    QString options;
    bool options_changed;
};

LibAVFilter::LibAVFilter():
    Filter(*new LibAVFilterPrivate())
{
}

LibAVFilter::~LibAVFilter()
{
}

bool LibAVFilter::setOptions(const QString &options)
{
    return d_func().setOptions(options);
}

QString LibAVFilter::options() const
{
    return d_func().options;
}

void LibAVFilter::process(Statistics *statistics, Frame *frame)
{
    Q_UNUSED(statistics);
    DPTR_D(LibAVFilter);
    if (!d.push(frame))
        return;
    d.pull(frame);
}

} //namespace QtAV

#include "Bioscope.hpp"

struct Bioscope::Detail {
    static bool avInitialized;
    /* av structures */
    AVFormatContext * formatContext;
    AVCodecContext * codecContext;
    AVCodec * codec;
    int vStreamIndex;
    AVFrame * frame;
    AVFrame * frameRGB;
    QByteArray frameBytes;
    SwsContext * convertContext;

    /* cached metadata */
    qint64 duration;
    int width, height;

    Detail() :
        formatContext(0), codecContext(0), codec(0),
        vStreamIndex(-1),
        frame(0), frameRGB(0), convertContext(0),
        duration(0), width(0), height(0)
    {
        if (!avInitialized) {
            av_register_all();
            avInitialized = true;
        }
    }

    ~Detail() {
        if (convertContext) sws_freeContext(convertContext);
        if (frameRGB) av_free(frameRGB);
        if (frame) av_free(frame);
        if (codecContext) avcodec_close(codecContext);
        if (formatContext) av_close_input_file(formatContext);
    }

    static QString fferror(int av_err) {
        char buff[1024];
        return av_strerror(av_err, buff, sizeof(buff)) == 0
                ? buff
                : "Unknown ffmpeg error";
    }
};

struct AVCheck {
    AVCheck(QString path) : m_path(path) {}
    void operator<<(int av_err) {
        if (av_err) {
            throw Bioscope::AVError(m_path, av_err);
        }
    }
private:
    QString m_path;
};

bool Bioscope::Detail::avInitialized = false;

Bioscope::AVError::AVError(const QString &path, int av_err) :
    Error(QString("FFMpeg error at %1: '%2'").arg(path).arg( Detail::fferror(av_err) ))
{}

Bioscope::Bioscope(const QString & _path, QObject *parent) :
    QObject(parent),
    m_detail( new Detail )
{
    QString path = QFileInfo(_path).canonicalFilePath();

    AVCheck check(path);

    check << av_open_input_file( &m_detail->formatContext,
                                 qPrintable(path), NULL, 0, NULL);

    Q_ASSERT(m_detail->formatContext);
    // HACK this solves "max_analyze_duration reached" warning
    m_detail->formatContext->max_analyze_duration *= 10;

    av_find_stream_info( m_detail->formatContext );

    for (unsigned int i = 0; i < m_detail->formatContext->nb_streams; i++) {
        if ( m_detail->formatContext->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO ) {
            m_detail->vStreamIndex=(int)i;
            break;
        }
    }
    if (m_detail->vStreamIndex < 0) throw UnsupportedFile(path);

    m_detail->codecContext = m_detail->formatContext->streams[m_detail->vStreamIndex]->codec;
    m_detail->codec = avcodec_find_decoder(m_detail->codecContext->codec_id);
    if (m_detail->codec == 0) throw UnsupportedFile(path);

    check << avcodec_open(m_detail->codecContext, m_detail->codec);

    m_detail->width = m_detail->codecContext->width;
    m_detail->height = m_detail->codecContext->height;

    // HACK to correct wrong frame rates that seem to be generated by some codecs
    // cf.: http://web.me.com/dhoerl/Home/Tech_Blog/Entries/2009/1/
    // 22_Revised_avcodec_sample.c.html
    if (m_detail->codecContext->time_base.num > 1000
            && m_detail->codecContext->time_base.den == 1)
        m_detail->codecContext->time_base.den=1000;
    {
        AVRational q_duration = {
            m_detail->formatContext->streams[m_detail->vStreamIndex]->duration,
            1 };

        AVRational seconds = av_mul_q(
                    q_duration,
                    m_detail->formatContext->streams[m_detail->vStreamIndex]->time_base );

        m_detail->duration = 1000 * (qint64)seconds.num / (qint64)seconds.den;
    }

    m_detail->frame = avcodec_alloc_frame();
    m_detail->frameRGB = avcodec_alloc_frame();
    int bufSize = avpicture_get_size(PIX_FMT_RGB24,
                                     m_detail->width,
                                     m_detail->height);

    m_detail->frameBytes = QByteArray(bufSize, 0);
    avpicture_fill((AVPicture *)m_detail->frameRGB,
                   (uint8_t*) m_detail->frameBytes.data(),
                   PIX_FMT_RGB24,
                   m_detail->width,
                   m_detail->height);

    m_detail->convertContext = sws_getContext(m_detail->width, m_detail->height,
                                              m_detail->codecContext->pix_fmt,
                                              m_detail->width, m_detail->height,
                                              PIX_FMT_RGB24, SWS_BICUBIC, 0, 0, 0);


}

Bioscope::~Bioscope()
{

}


bool Bioscope::supportedFile(const QString & path)
{
    try {
        Bioscope tmp(path);
        return true;
    } catch (...) {
        return false;
    }
}

qint64 Bioscope::duration() const
{
    return m_detail->duration;
}

int Bioscope::width() const
{
    return m_detail->width;
}

int Bioscope::height() const
{
    return m_detail->height;
}

QImage Bioscope::frame()
{
    AVPacket packet;
    int done = 0;
    while (av_read_frame(m_detail->formatContext, &packet) >= 0) {
        if (packet.stream_index == m_detail->vStreamIndex) {
            avcodec_decode_video2(
                        m_detail->codecContext,
                        m_detail->frame,
                        &done,
                        &packet);
            if (done) {
                sws_scale(m_detail->convertContext,
                          m_detail->frame->data, m_detail->frame->linesize, 0,
                          m_detail->codecContext->height,
                          m_detail->frameRGB->data, m_detail->frameRGB->linesize);
                // convert to QImage and return
                return QImage((uchar*)m_detail->frameBytes.data(),
                               m_detail->codecContext->width,
                               m_detail->codecContext->height,
                               QImage::Format_RGB888);
            }
        }
    }

    return QImage();
}

void Bioscope::seek(qint64 ms)
{
    // ms to time_base units...
    AVRational sec = { (int)ms, 1000};
    AVRational ts = av_div_q(sec,
                             m_detail->formatContext->streams[m_detail->vStreamIndex]->time_base);
    av_seek_frame(m_detail->formatContext, m_detail->vStreamIndex, ts.num/ts.den, 0);
}

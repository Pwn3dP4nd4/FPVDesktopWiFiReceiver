// Linux (y cualquier sistema con FFmpeg): libavcodec + libavformat de la distribucion.
// H.264 con libx264 si esta; si no, libopenh264; y si la distro no trae ningun H.264, MPEG-4 Part 2 (tambien MP4).
#include "video_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#include <cstring>

#include "log.h"

namespace {

QString averr(const char *what, int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof buf);
    return QStringLiteral("%1: %2").arg(QString::fromLatin1(what), QString::fromLocal8Bit(buf));
}

const AVCodec *findEncoder() {
    for (const char *n : {"libx264", "libopenh264", "mpeg4"})
        if (const AVCodec *c = avcodec_find_encoder_by_name(n)) return c;
    return nullptr;
}

bool supportsNv12(const AVCodec *c) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void *cfg = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, &n) < 0 || !cfg) return false;
    const auto *f = static_cast<const AVPixelFormat *>(cfg);
    for (int i = 0; i < n; ++i)
        if (f[i] == AV_PIX_FMT_NV12) return true;
    return false;
#else
    for (const AVPixelFormat *f = c->pix_fmts; f && *f != AV_PIX_FMT_NONE; ++f)
        if (*f == AV_PIX_FMT_NV12) return true;
    return false;
#endif
}

class AvEncoder final : public VideoEncoder {
public:
    ~AvEncoder() override { cleanup(); }

    bool open(const QString &path, const Params &p, QString *err) override {
        p_ = p;
        const AVCodec *codec = findEncoder();
        if (!codec) {
            *err = QStringLiteral("FFmpeg has no H.264 or MPEG-4 encoder");
            return false;
        }
        const QByteArray file = path.toUtf8();
        int e = avformat_alloc_output_context2(&fmt_, nullptr, "mp4", file.constData());
        if (e < 0) return fail(err, "MP4 muxer", e);
        st_ = avformat_new_stream(fmt_, nullptr);
        ctx_ = avcodec_alloc_context3(codec);
        if (!st_ || !ctx_) return fail(err, "out of memory", AVERROR(ENOMEM));
        nv12_ = supportsNv12(codec);
        ctx_->width = p.width;
        ctx_->height = p.height;
        ctx_->time_base = AVRational{p.fpsDen, p.fpsNum};
        ctx_->framerate = AVRational{p.fpsNum, p.fpsDen};
        ctx_->sample_aspect_ratio = AVRational{p.sarNum, p.sarDen};
        ctx_->pix_fmt = nv12_ ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        ctx_->bit_rate = p.bitrate;
        ctx_->gop_size = 2 * p.fpsNum / p.fpsDen;
        ctx_->colorspace = AVCOL_SPC_SMPTE170M;
        ctx_->color_primaries = AVCOL_PRI_SMPTE170M;
        ctx_->color_trc = AVCOL_TRC_SMPTE170M;
        ctx_->color_range = AVCOL_RANGE_MPEG;
        if (fmt_->oformat->flags & AVFMT_GLOBALHEADER) ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary *opts = nullptr;
        if (std::strcmp(codec->name, "libx264") == 0) av_dict_set(&opts, "preset", "veryfast", 0);
        e = avcodec_open2(ctx_, codec, &opts);
        av_dict_free(&opts);
        if (e < 0) return fail(err, "encoder", e);
        e = avcodec_parameters_from_context(st_->codecpar, ctx_);
        if (e < 0) return fail(err, "stream", e);
        st_->time_base = ctx_->time_base;
        st_->sample_aspect_ratio = ctx_->sample_aspect_ratio;
        e = avio_open(&fmt_->pb, file.constData(), AVIO_FLAG_WRITE);
        if (e < 0) return fail(err, "could not create the file", e);
        // MP4 fragmentado: indice por trozos (uno por fotograma clave, ~2 s) junto a los datos; un cierre brusco
        // solo pierde el ultimo trozo y no todo el archivo
        AVDictionary *mux = nullptr;
        av_dict_set(&mux, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
        e = avformat_write_header(fmt_, &mux);
        av_dict_free(&mux);
        if (e < 0) return fail(err, "MP4 header", e);
        headerWritten_ = true;
        frame_ = av_frame_alloc();
        pkt_ = av_packet_alloc();
        if (!frame_ || !pkt_) return fail(err, "out of memory", AVERROR(ENOMEM));
        frame_->format = ctx_->pix_fmt;
        frame_->width = p.width;
        frame_->height = p.height;
        e = av_frame_get_buffer(frame_, 0);
        if (e < 0) return fail(err, "frame", e);
        KP_LOGI("video: FFmpeg %s %dx%d %d/%d fps", codec->name, p.width, p.height, p.fpsNum, p.fpsDen);
        return true;
    }

    bool write(const uint8_t *nv12, int64_t pts, QString *err) override {
        int e = av_frame_make_writable(frame_);
        if (e < 0) return fail(err, "frame", e);
        const int w = p_.width, h = p_.height;
        for (int y = 0; y < h; ++y) std::memcpy(frame_->data[0] + y * frame_->linesize[0], nv12 + y * w, w);
        const uint8_t *uv = nv12 + w * h;
        for (int y = 0; y < h / 2; ++y) {
            const uint8_t *s = uv + y * w;
            if (nv12_) {
                std::memcpy(frame_->data[1] + y * frame_->linesize[1], s, w);
            } else {
                uint8_t *u = frame_->data[1] + y * frame_->linesize[1], *v = frame_->data[2] + y * frame_->linesize[2];
                for (int x = 0; x < w / 2; ++x) {
                    u[x] = s[2 * x];
                    v[x] = s[2 * x + 1];
                }
            }
        }
        frame_->pts = pts;
        return encode(frame_, err);
    }

    bool close(QString *err) override {
        bool ok = true;
        if (headerWritten_) {
            ok = encode(nullptr, err);  // vaciar el codificador
            const int e = av_write_trailer(fmt_);
            if (ok && e < 0) ok = fail(err, "could not finish the MP4 file", e);
        }
        cleanup();
        return ok;
    }

private:
    bool fail(QString *err, const char *what, int e) {
        *err = averr(what, e);
        return false;
    }

    bool encode(AVFrame *f, QString *err) {
        int e = avcodec_send_frame(ctx_, f);
        if (e < 0) return fail(err, "encoding", e);
        for (;;) {
            e = avcodec_receive_packet(ctx_, pkt_);
            if (e == AVERROR(EAGAIN) || e == AVERROR_EOF) return true;
            if (e < 0) return fail(err, "encoding", e);
            av_packet_rescale_ts(pkt_, ctx_->time_base, st_->time_base);
            pkt_->stream_index = st_->index;
            e = av_interleaved_write_frame(fmt_, pkt_);
            if (e < 0) return fail(err, "writing the video failed", e);
        }
    }

    void cleanup() {
        av_frame_free(&frame_);
        av_packet_free(&pkt_);
        avcodec_free_context(&ctx_);
        if (fmt_) {
            if (fmt_->pb) avio_closep(&fmt_->pb);
            avformat_free_context(fmt_);
            fmt_ = nullptr;
        }
        headerWritten_ = false;
    }

    Params p_;
    AVFormatContext *fmt_ = nullptr;
    AVStream *st_ = nullptr;
    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *pkt_ = nullptr;
    bool nv12_ = false, headerWritten_ = false;
};

}  // namespace

std::unique_ptr<VideoEncoder> VideoEncoder::create() {
    if (!findEncoder()) return nullptr;
    return std::make_unique<AvEncoder>();
}
QString VideoEncoder::backendName() { return QStringLiteral("FFmpeg"); }

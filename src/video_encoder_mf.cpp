// Windows: Media Foundation Sink Writer (H.264 en MP4). Viene con Windows (salvo ediciones N sin el
// "Media Feature Pack"): no anade ninguna DLL a la version portable.
#include "video_encoder.h"

#include <windows.h>

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstring>

#include "log.h"

namespace {

template <class T> void release(T *&p) {
    if (p) p->Release();
    p = nullptr;
}

QString hr(const char *what, HRESULT h) {
    return QStringLiteral("%1 (0x%2)").arg(QString::fromLatin1(what)).arg(quint32(h), 8, 16, QLatin1Char('0'));
}

class MfEncoder final : public VideoEncoder {
public:
    ~MfEncoder() override {
        release(writer_);
        if (mfStarted_) MFShutdown();
        if (comInit_) CoUninitialize();
    }

    bool open(const QString &path, const Params &p, QString *err) override {
        p_ = p;
        const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comInit_ = SUCCEEDED(ci);
        HRESULT h = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(h)) {
            *err = hr("Media Foundation is not available (Windows N edition without the Media Feature Pack?)", h);
            return false;
        }
        mfStarted_ = true;
        // Primero con el codificador por hardware de la grafica; si ese no acepta el formato, por software.
        for (const bool hw : {true, false}) {
            h = tryOpen(path, hw);
            if (SUCCEEDED(h)) {
                KP_LOGI("video: Media Foundation H.264 %dx%d %d/%d fps, %s", p.width, p.height, p.fpsNum, p.fpsDen,
                        hw ? "hardware permitido" : "software");
                return true;
            }
            release(writer_);
            DeleteFileW(reinterpret_cast<const wchar_t *>(path.utf16()));
            KP_LOGW("video: Media Foundation (%s) fallo: 0x%08lx", hw ? "hardware" : "software", static_cast<unsigned long>(h));
        }
        *err = hr("Could not start the H.264 encoder of Windows", h);
        return false;
    }

    bool write(const uint8_t *nv12, int64_t pts, QString *err) override {
        const DWORD size = DWORD(p_.width) * p_.height * 3 / 2;
        IMFMediaBuffer *buf = nullptr;
        IMFSample *sample = nullptr;
        HRESULT h = MFCreateMemoryBuffer(size, &buf);
        BYTE *dst = nullptr;
        if (SUCCEEDED(h)) h = buf->Lock(&dst, nullptr, nullptr);
        if (SUCCEEDED(h)) {
            std::memcpy(dst, nv12, size);
            buf->Unlock();
            h = buf->SetCurrentLength(size);
        }
        if (SUCCEEDED(h)) h = MFCreateSample(&sample);
        if (SUCCEEDED(h)) h = sample->AddBuffer(buf);
        if (SUCCEEDED(h)) h = sample->SetSampleTime(to100ns(pts));
        if (SUCCEEDED(h)) h = sample->SetSampleDuration(to100ns(pts + 1) - to100ns(pts));
        if (SUCCEEDED(h)) h = writer_->WriteSample(stream_, sample);
        release(sample);
        release(buf);
        if (FAILED(h)) *err = hr("Writing the video failed", h);
        return SUCCEEDED(h);
    }

    bool close(QString *err) override {
        if (!writer_) return true;
        const HRESULT h = writer_->Finalize();
        release(writer_);
        if (FAILED(h)) *err = hr("Could not finish the MP4 file", h);
        return SUCCEEDED(h);
    }

private:
    LONGLONG to100ns(int64_t pts) const { return LONGLONG(pts) * 10000000LL * p_.fpsDen / p_.fpsNum; }

    HRESULT setVideoType(IMFMediaType *t, const GUID &sub) {
        HRESULT h = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(h)) h = t->SetGUID(MF_MT_SUBTYPE, sub);
        if (SUCCEEDED(h)) h = t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(h)) h = MFSetAttributeSize(t, MF_MT_FRAME_SIZE, p_.width, p_.height);
        if (SUCCEEDED(h)) h = MFSetAttributeRatio(t, MF_MT_FRAME_RATE, p_.fpsNum, p_.fpsDen);
        if (SUCCEEDED(h)) h = MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, p_.sarNum, p_.sarDen);
        if (SUCCEEDED(h)) h = t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601);
        if (SUCCEEDED(h)) h = t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        return h;
    }

    HRESULT tryOpen(const QString &path, bool hw) {
        IMFAttributes *attr = nullptr;
        IMFMediaType *out = nullptr, *in = nullptr;
        HRESULT h = MFCreateAttributes(&attr, 3);
        if (SUCCEEDED(h)) h = attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hw ? TRUE : FALSE);
        if (SUCCEEDED(h)) h = attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        // Sin limitar: el grabador ya lleva su propia cola, y WriteSample solo bloquea su hilo.
        if (SUCCEEDED(h)) h = attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        if (SUCCEEDED(h))
            h = MFCreateSinkWriterFromURL(reinterpret_cast<const wchar_t *>(path.utf16()), nullptr, attr, &writer_);
        if (SUCCEEDED(h)) h = MFCreateMediaType(&out);
        if (SUCCEEDED(h)) h = setVideoType(out, MFVideoFormat_H264);
        if (SUCCEEDED(h)) h = out->SetUINT32(MF_MT_AVG_BITRATE, p_.bitrate);
        if (SUCCEEDED(h)) h = out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        if (SUCCEEDED(h)) h = writer_->AddStream(out, &stream_);
        if (SUCCEEDED(h)) h = MFCreateMediaType(&in);
        if (SUCCEEDED(h)) h = setVideoType(in, MFVideoFormat_NV12);
        if (SUCCEEDED(h)) h = in->SetUINT32(MF_MT_DEFAULT_STRIDE, p_.width);
        if (SUCCEEDED(h)) h = writer_->SetInputMediaType(stream_, in, nullptr);
        if (SUCCEEDED(h)) h = writer_->BeginWriting();
        release(in);
        release(out);
        release(attr);
        return h;
    }

    Params p_;
    IMFSinkWriter *writer_ = nullptr;
    DWORD stream_ = 0;
    bool comInit_ = false, mfStarted_ = false;
};

}  // namespace

std::unique_ptr<VideoEncoder> VideoEncoder::create() { return std::make_unique<MfEncoder>(); }
QString VideoEncoder::backendName() { return QStringLiteral("Media Foundation"); }

#include <jni.h>
#include <string>
#include <unistd.h>
#include <thread>
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/mediacodec.h>
#include <libavformat/avformat.h>
#include "libavutil/ffversion.h"
#include "libavutil/imgutils.h"
#include <libswscale/swscale.h>
#include <libavfilter/avfilter.h>
#include <libavcodec/jni.h>
}

#include <android/native_window_jni.h>
#include "AVPacketQueue.h"
#include "Logger.h"

#define RTMP_ADDR "rtmp://192.168.1.103/live/livestream"
std::shared_ptr<AVPacketQueue> mVideoPacketQueue = std::make_shared<AVPacketQueue>(5000);
std::thread *mVideoThread = nullptr;
int64_t mRetryReceiveCount = 7;

bool pushPacketToQueue(AVPacket *packet, const std::shared_ptr<AVPacketQueue>& queue) {
    if (queue == nullptr) {
        return false;
    }

    bool suc = false;
    while (queue->isFull()) {
        queue->wait(10);
        LOGE("queue is full, wait 10ms, packet index: %d", packet->stream_index)
    }
    queue->push(packet);
    suc = true;
    return suc;
}

static enum AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
static enum AVPixelFormat get_hw_format(
        AVCodecContext *ctx,
        const enum AVPixelFormat *pix_fmts) {
    const enum AVPixelFormat *p;
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == hw_pix_fmt) {
            LOGD("get HW surface format: %d", *p);
            return *p;
        }
    }

    LOGD("Failed to get HW surface format");
    return AV_PIX_FMT_NONE;
}

int swsScale(AVFrame *srcFrame, AVFrame *swFrame) {
    SwsContext *mSwsContext = nullptr;
    mSwsContext = sws_getContext(srcFrame->width, srcFrame->height, AVPixelFormat(srcFrame->format),
                                 srcFrame->width, srcFrame->height, AV_PIX_FMT_RGBA,
                                 SWS_BICUBIC,nullptr, nullptr, nullptr);
    if (!mSwsContext) {
        return -1;
    }

    // transform
    int ret = sws_scale(mSwsContext,
                        reinterpret_cast<const uint8_t *const *>(srcFrame->data),
                        srcFrame->linesize,
                        0,
                        srcFrame->height,
                        swFrame->data,
                        swFrame->linesize
    );

    // buffer is right, but some property lost
    if (swFrame->format == AV_PIX_FMT_NONE) {
        swFrame->format = AV_PIX_FMT_RGBA;
        swFrame->width = srcFrame->width;
        swFrame->height = srcFrame->height;
    }

    return ret;
}

int decode(AVCodecContext *avCodecContext, AVFrame *avFrame, int video_idx) {
    AVPacket *avPacket = av_packet_alloc();
    mVideoPacketQueue->popTo(avPacket);
    int receiveRes = 0;
    LOGD("avPacket stream index %d", avPacket->stream_index);
    if (avPacket->stream_index == video_idx) {
        bool isEof = avPacket->size == 0 && avPacket->data == nullptr;
        int sendRes = avcodec_send_packet(avCodecContext, avPacket);

        bool isKeyFrame = avPacket->flags & AV_PKT_FLAG_KEY;
        LOGI("[video] avcodec_send_packet...pts: %" PRId64 ", dts: %" PRId64 ", isKeyFrame: %d, res: %d, isEof: %d", avPacket->pts, avPacket->dts, isKeyFrame, sendRes, isEof)

        // avcodec_send_packet的-11表示要先读output，然后pkt需要重发
        bool mNeedResent = sendRes == AVERROR(EAGAIN);

        // avcodec_receive_frame的-11，表示需要发新帧
        LOGD("mNeedResent, %d", mNeedResent);
//        sleep(1);
        receiveRes = avcodec_receive_frame(avCodecContext, avFrame);

        if (isEof && receiveRes != AVERROR_EOF && mRetryReceiveCount >= 0) {
            mNeedResent = true;
            mRetryReceiveCount--;
            LOGE("[video] send eof, not receive eof...retry count: %" PRId64, mRetryReceiveCount)
        }

        if (receiveRes != 0) {
            LOGE("[video] avcodec_receive_frame err: %d, resent: %d", receiveRes, mNeedResent)
            av_packet_unref(avPacket);
            av_packet_free(&avPacket);
            av_frame_unref(avFrame);
            // force EOF
            if (isEof && mRetryReceiveCount < 0) {
                receiveRes = AVERROR_EOF;
            }
            return receiveRes;
        }
    }

    LOGD("avframe format %d", avFrame->format);
    return receiveRes;
}

void getPacketLoop(AVFormatContext *ifmt_ctx) {
    AVPacket *avPacket = av_packet_alloc();
//    LOGD("ifmt_ctx.audio_codec_id = %d", ifmt_ctx->audio_codec_id);
    while (av_read_frame(ifmt_ctx, avPacket) >= 0) {
        pushPacketToQueue(avPacket, mVideoPacketQueue);
    }
//    av_packet_free(&avPacket);
//    av_freep(&avPacket);
}

void VideoDecodeLoop(int video_idx, AVCodecContext *avCodecContext) {


    while(true) {
        if (!mVideoPacketQueue->isEmpty()) {
            mVideoPacketQueue->notify();
        }

        while (mVideoPacketQueue->isEmpty()) {
            LOGE("[video] no packet, wait...")
            mVideoPacketQueue->wait();
        }

        AVFrame *avFrame = av_frame_alloc();
        AVFrame *avFrameRGBA = av_frame_alloc();
        decode(avCodecContext, avFrame, video_idx);

        LOGD("frame content ");
        if (avFrame->format != AV_PIX_FMT_MEDIACODEC) return;
        int size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, avFrame->width, avFrame->height, 1);
        auto *buffer = static_cast<uint8_t *>(av_malloc(size * sizeof(uint8_t)));
        av_image_fill_arrays(avFrameRGBA->data, avFrameRGBA->linesize, buffer, AV_PIX_FMT_RGBA, avFrame->width, avFrame->height, 1);
        if (swsScale(avFrame, avFrameRGBA) > 0) {
            av_mediacodec_release_buffer((AVMediaCodecBuffer *)avFrameRGBA->data[3], 1);
        }
    }




//            ANativeWindow *nativeWindow = ANativeWindow_fromSurface(env,surface);
//            int videoWidth = avCodecContext->width;
//            int videoHeight = avCodecContext->height;
//            ANativeWindow_setBuffersGeometry(nativeWindow, videoWidth, videoHeight,
//                                             WINDOW_FORMAT_RGBA_8888);
//            ANativeWindow_Buffer windowBuffer;

}

void codec(JNIEnv* env, jobject surface) {
    // 输入rtmp url
    avformat_network_init();
    AVFormatContext *ifmt_ctx = avformat_alloc_context();

    // 初始化输入格式上下文
    if (ifmt_ctx == nullptr) {
        LOGD("avformat_alloc_context failed.");
        exit(1);
    }
    int ret = avformat_open_input(&ifmt_ctx, RTMP_ADDR, nullptr, nullptr);
    LOGD("avformat_open_input result %d.", ret);
    if (ret < 0) {
        LOGD("failed to open input file.");
        avformat_close_input(&ifmt_ctx);
        exit(1);
    }
    LOGD("open input.");

    if (avformat_find_stream_info(ifmt_ctx, nullptr) < 0) {
        LOGD("failed to find stream info");
        exit(1);
    }
//    LOGD("ffmpeg configuration %s", avcodec_configuration());
    LOGD("find stream info");

//    av_dump_format(ifmt_ctx, -1, RTMP_ADDR, 0);

    int video_idx = -1;
    video_idx = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_idx < 0) {
        LOGD("failed to find stream_index");
        exit(1);
    }
    LOGD("video index %d", video_idx);

    AVHWDeviceType type = av_hwdevice_find_type_by_name("mediacodec");
//    const AVCodec *avCodec = avcodec_find_decoder(avCodecContext->codec_id);
    const AVCodec *avCodec = avcodec_find_decoder_by_name("h264_mediacodec");

    for (int i = 0; ; ++i) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(avCodec, i);
        if (!config) {
            LOGD("Decoder: %s does not support device type: %s", avCodec->name,
                 av_hwdevice_get_type_name(type));
            break;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && config->device_type == type) {
            // AV_PIX_FMT_MEDIACODEC(165)
            hw_pix_fmt = config->pix_fmt;
            LOGD("Decoder: %s support device type: %s, hw_pix_fmt: %d, AV_PIX_FMT_MEDIACODEC: %d", avCodec->name,
                 av_hwdevice_get_type_name(type), hw_pix_fmt, AV_PIX_FMT_MEDIACODEC);
            break;
        }
    }

    AVBufferRef *mHwDeviceCtx = nullptr;
    ret = av_hwdevice_ctx_create(&mHwDeviceCtx, type, nullptr, nullptr, 0);
    if (ret != 0) {
        LOGD("av_hwdevice_ctx_create err: %d", ret);
    }

    AVCodecContext *avCodecContext = avcodec_alloc_context3(nullptr);
    if (avcodec_parameters_to_context(avCodecContext, ifmt_ctx->streams[video_idx]->codecpar) < 0) {
        LOGD("copy stream failed");
        exit(1);
    }

    if (mHwDeviceCtx) {
        avCodecContext->get_format = get_hw_format;
        avCodecContext->hw_device_ctx = av_buffer_ref(mHwDeviceCtx);

        if (surface != nullptr) {
            AVMediaCodecContext *mMediaCodecContext = av_mediacodec_alloc_context();
            av_mediacodec_default_init(avCodecContext, mMediaCodecContext, surface);
        }
    }

    if (avcodec_open2(avCodecContext, avCodec, nullptr) != 0) {
        LOGD("open codec failed.");
        exit(1);
    }

//    LOGD("ifmt_ctx.audio_codec_id = %d", ifmt_ctx->audio_codec_id);
    std::thread producer(getPacketLoop, ifmt_ctx);
    std::thread consumer(VideoDecodeLoop, video_idx, avCodecContext);

    producer.join();
    consumer.join();

}


extern "C" jstring JNICALL Java_com_fox_ffmpegtest_MainActivity_stringFromJNI(
        JNIEnv* env, jobject thiz, jobject surface) {
    codec(env, surface);
//    av_dump_format(ifmt_ctx, 0, RTMP_ADDR, 0);
    return env->NewStringUTF(FFMPEG_VERSION);

}
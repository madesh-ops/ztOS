/*
 * ztOS: Android's VP9, H.264 and HEVC decoders on the M20's MFC.
 *
 * Waydroid's vendor image decodes video only in software, with the 32-bit
 * OMX.google components (OMX service, libstagefright_soft_*.so): Instagram's
 * reels stalled on them. This library stands in for one of those (built
 * three times, ZT_LIB = vpxdec, avcdec or hevcdec) and decodes with FFmpeg:
 * its V4L2 mem2mem decoders on the MFC (/dev/video0, passed into the
 * container by Waydroid) for up to ZT_MAX_HW streams at a time, FFmpeg's own
 * multithreaded software decoders for the rest. A stream whose hardware
 * decoder fails goes on in software from its last key frame.
 *
 * The original component, its soname renamed (libstagefright_soft_*dsw.so),
 * still serves what this one does not: VP8, and everything when
 * persist.ztos.hwdec is 0.
 *
 * The component itself is Android's SoftVideoDecoderOMXComponent, as in
 * SoftVPX.cpp: this only feeds it frames.
 */
#define LOG_TAG "ztos-hwdec"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <atomic>

#include <utils/Log.h>
#include <utils/List.h>
#include <utils/misc.h>
#include <media/stagefright/omx/SoftVideoDecoderOMXComponent.h>
#include <media/stagefright/foundation/ADebug.h>
#include "OMX_VideoExt.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
}

#define STR2(x) #x
#define STR(x) STR2(x)
/* the original library, as installed beside this one */
#define SW_LIB "libstagefright_soft_" STR(ZT_SW) ".so"

namespace android {

static const CodecProfileLevel kVP9ProfileLevels[] = {
	/* 8 bits only: the MFC has no 10-bit output, and a player that is told
	 * so picks the 8-bit stream of a video that has both */
	{ OMX_VIDEO_VP9Profile0, OMX_VIDEO_VP9Level5 },
};
static const CodecProfileLevel kAVCProfileLevels[] = {
	{ OMX_VIDEO_AVCProfileConstrainedBaseline, OMX_VIDEO_AVCLevel52 },
	{ OMX_VIDEO_AVCProfileBaseline, OMX_VIDEO_AVCLevel52 },
	{ OMX_VIDEO_AVCProfileMain, OMX_VIDEO_AVCLevel52 },
	{ OMX_VIDEO_AVCProfileConstrainedHigh, OMX_VIDEO_AVCLevel52 },
	{ OMX_VIDEO_AVCProfileHigh, OMX_VIDEO_AVCLevel52 },
};
static const CodecProfileLevel kHEVCProfileLevels[] = {
	{ OMX_VIDEO_HEVCProfileMain, OMX_VIDEO_HEVCMainTierLevel51 },
};

/* hardware streams open now, in this process (the OMX service has them all) */
static std::atomic<int> gHwActive(0);

static int propInt(const char *name, int dflt) {
	char value[PROP_VALUE_MAX] = "";
	if (__system_property_get(name, value) <= 0) {
		return dflt;
	}
	return atoi(value);
}

static void avLog(void *, int level, const char *fmt, va_list args) {
	if (level > AV_LOG_WARNING) {
		return;
	}
	char line[512];
	vsnprintf(line, sizeof(line), fmt, args);
	LOG_PRI(level <= AV_LOG_ERROR ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN, LOG_TAG, "ffmpeg: %s", line);
}

struct ZtHwDec : public SoftVideoDecoderOMXComponent {
	ZtHwDec(const char *name, const char *role, OMX_VIDEO_CODINGTYPE coding,
			const CodecProfileLevel *levels, size_t numLevels, const char *mime,
			AVCodecID id, const char *hwName,
			const OMX_CALLBACKTYPE *callbacks, OMX_PTR appData, OMX_COMPONENTTYPE **component)
		: SoftVideoDecoderOMXComponent(name, role, coding, levels, numLevels,
				320, 240, callbacks, appData, component),
		  mId(id), mHwName(hwName) {
		const size_t kMaxOutputBufferSize = 2048 * 2048 * 3 / 2;
		initPorts(kNumBuffers, kMaxOutputBufferSize / 4, kNumBuffers, mime, 4);
		mPkt = av_packet_alloc();
		mFrame = av_frame_alloc();
	}

protected:
	virtual ~ZtHwDec() {
		closeDecoder();
		dropReplay();
		av_packet_free(&mPkt);
		av_frame_free(&mFrame);
		free(mExtra);
	}

	virtual void onQueueFilled(OMX_U32 portIndex);
	virtual void onPortFlushCompleted(OMX_U32 portIndex);
	virtual void onReset();
	/* the colours the decoder found win over the container's: a 10-bit
	 * frame given out as 8 bits is no longer HDR (see outputFrames) */
	virtual int getColorAspectPreference() { return kPreferBitstream; }

private:
	enum { kNumBuffers = 8, kMaxReplay = 240 };
	enum { INPUT_DATA_AVAILABLE, INPUT_EOS_SEEN, OUTPUT_FRAMES_FLUSHED } mEOSStatus = INPUT_DATA_AVAILABLE;

	AVCodecID mId;
	const char *mHwName;
	AVCodecContext *mCtx = nullptr;
	bool mHw = false;		/* mCtx is the MFC's */
	bool mHwFailed = false;		/* this stream is in software from now on */
	bool mPending = false;		/* mFrame holds a frame not yet in an output buffer */
	bool mDraining = false;
	bool mSaid = false;
	bool mSaidIn = false;
	AVPacket *mPkt;
	AVFrame *mFrame;
	uint8_t *mExtra = nullptr;	/* the codec-specific data (SPS/PPS, VPS) */
	int mExtraSize = 0;
	bool mExtraDue = false;		/* new codec-specific data since the decoder opened */
	/* what went in since the last key frame: a decoder that fails starts
	 * again from there, in software */
	AVPacket *mReplay[kMaxReplay];
	int mReplayCount = 0;

	bool openDecoder();
	void closeDecoder();
	void dropReplay();
	bool isKeyFrame(const uint8_t *data, size_t size) const;
	int sendPacket(AVPacket *pkt);
	bool fallBack();
	bool outputFrames(bool *portWillReset);
	void copyFrame(uint8_t *dst);

	DISALLOW_EVIL_CONSTRUCTORS(ZtHwDec);
};

bool ZtHwDec::openDecoder() {
	const AVCodec *codec = nullptr;
	mHw = false;
	if (!mHwFailed && gHwActive.load() < propInt("persist.ztos.hwdec.max", ZT_MAX_HW)) {
		codec = avcodec_find_decoder_by_name(mHwName);
		mHw = codec != nullptr;
	}
	for (int attempt = 0; attempt < 2; attempt++) {
		if (!mHw) {
			codec = avcodec_find_decoder(mId);
		}
		mCtx = avcodec_alloc_context3(codec);
		if (!mCtx) {
			return false;
		}
		mCtx->pkt_timebase = AVRational{1, 1000000};
		mCtx->time_base = AVRational{1, 1000000};
		/* the size the player gave (its container's): the V4L2 decoder sizes
		 * its bitstream buffers by it, and with none the MFC got a stream
		 * header cut short and failed to parse it */
		const OMX_VIDEO_PORTDEFINITIONTYPE &in = editPortInfo(kInputPortIndex)->mDef.format.video;
		mCtx->width = mCtx->coded_width = in.nFrameWidth;
		mCtx->height = mCtx->coded_height = in.nFrameHeight;
		/* (the MFC's own decoder gets them ahead of the first frame instead:
		 * as extradata, sent alone, they failed its header parse) */
		if (mExtraSize > 0 && !mHw) {
			mCtx->extradata = (uint8_t *)av_mallocz(mExtraSize + AV_INPUT_BUFFER_PADDING_SIZE);
			memcpy(mCtx->extradata, mExtra, mExtraSize);
			mCtx->extradata_size = mExtraSize;
		}
		if (!mHw) {
			/* frame threads: four frames in flight, one a core of the A73 pair
			 * and the A53s each; slices too, for H.264/HEVC streams with them */
			mCtx->thread_count = propInt("persist.ztos.hwdec.threads", 4);
			mCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
		}
		if (avcodec_open2(mCtx, codec, nullptr) == 0) {
			if (mHw) {
				gHwActive++;
			}
			ALOGI("%s: %s (%d on the MFC)", mHwName, mHw ? "hardware" : "software", gHwActive.load());
			/* the V4L2 decoder does not pass extradata on: the MFC needs the
			 * SPS/PPS (VPS) ahead of the first frame, as in the stream itself */
			mExtraDue = mHw && mExtraSize > 0;
			return true;
		}
		ALOGW("%s: could not open %s", mHwName, codec->name);
		avcodec_free_context(&mCtx);
		if (!mHw) {
			return false;
		}
		mHw = false;
		mHwFailed = true;
	}
	return false;
}

void ZtHwDec::closeDecoder() {
	if (mCtx) {
		if (mHw) {
			gHwActive--;
		}
		avcodec_free_context(&mCtx);
	}
	mHw = false;
	mPending = false;
	mDraining = false;
	av_frame_unref(mFrame);
}

void ZtHwDec::dropReplay() {
	for (int i = 0; i < mReplayCount; i++) {
		av_packet_free(&mReplay[i]);
	}
	mReplayCount = 0;
}

bool ZtHwDec::isKeyFrame(const uint8_t *d, size_t size) const {
	if (mId == AV_CODEC_ID_VP9) {
		if (size < 1 || (d[0] >> 6) != 2) {
			return false;
		}
		int profile = ((d[0] >> 5) & 1) | (((d[0] >> 4) & 1) << 1);
		int bit = profile == 3 ? 2 : 3;	/* profile 3 has a reserved bit more */
		bool showExisting = (d[0] >> bit) & 1;
		return !showExisting && !((d[0] >> (bit - 1)) & 1);
	}
	/* Annex B: an IDR slice (H.264) or an IRAP picture (HEVC) */
	for (size_t i = 0; i + 3 < size; i++) {
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
			int type = mId == AV_CODEC_ID_H264 ? d[i + 3] & 0x1f : (d[i + 3] >> 1) & 0x3f;
			if (mId == AV_CODEC_ID_H264 ? type == 5 : type >= 16 && type <= 21) {
				return true;
			}
			i += 2;
		}
	}
	return false;
}

/* The MFC failed this stream: the same stream, in software, from its last
 * key frame */
bool ZtHwDec::fallBack() {
	ALOGW("%s: the MFC failed; on in software", mHwName);
	closeDecoder();
	mHwFailed = true;
	if (!openDecoder()) {
		return false;
	}
	for (int i = 0; i < mReplayCount; i++) {
		int err;
		while ((err = avcodec_send_packet(mCtx, mReplay[i])) == AVERROR(EAGAIN)) {
			/* frames from these packets were shown already */
			if (avcodec_receive_frame(mCtx, mFrame) < 0) {
				break;
			}
			av_frame_unref(mFrame);
		}
		if (err < 0 && err != AVERROR(EAGAIN) && err != AVERROR_INVALIDDATA) {
			return false;
		}
		while (avcodec_receive_frame(mCtx, mFrame) == 0) {
			av_frame_unref(mFrame);
		}
	}
	return true;
}

/* 0, or a negative AVERROR other than EAGAIN */
int ZtHwDec::sendPacket(AVPacket *pkt) {
	for (;;) {
		int err = avcodec_send_packet(mCtx, pkt);
		if (err != AVERROR(EAGAIN)) {
			if (err < 0 && mHw && err != AVERROR_EOF) {
				return fallBack() ? sendPacket(pkt) : err;
			}
			return err == AVERROR_INVALIDDATA ? 0 : err;
		}
		/* the decoder wants its frames taken first; with no output buffer
		 * free one is held back (mPending) and the packet waits */
		if (mPending) {
			return AVERROR(EAGAIN);
		}
		int r = avcodec_receive_frame(mCtx, mFrame);
		if (r == 0) {
			mPending = true;
		} else if (r != AVERROR(EAGAIN)) {
			return r;
		}
	}
}

void ZtHwDec::copyFrame(uint8_t *dst) {
	size_t dstYStride = outputBufferWidth();
	size_t dstUVStride = dstYStride / 2;
	size_t dstHeight = outputBufferHeight();
	uint8_t *dstU = dst + dstYStride * dstHeight;
	uint8_t *dstV = dst + (5 * dstYStride * dstHeight) / 4;
	int w = mFrame->width, h = mFrame->height;
	int fmt = mFrame->format;
	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((AVPixelFormat)fmt);
	bool deep = desc && desc->comp[0].depth > 8;
	if (fmt == AV_PIX_FMT_NV12 || fmt == AV_PIX_FMT_NV21) {
		uint8_t *u = fmt == AV_PIX_FMT_NV12 ? dstU : dstV;
		uint8_t *v = fmt == AV_PIX_FMT_NV12 ? dstV : dstU;
		for (int y = 0; y < h; y++) {
			memcpy(dst + y * dstYStride, mFrame->data[0] + y * mFrame->linesize[0], w);
		}
		for (int y = 0; y < h / 2; y++) {
			const uint8_t *s = mFrame->data[1] + y * mFrame->linesize[1];
			uint8_t *du = u + y * dstUVStride, *dv = v + y * dstUVStride;
			for (int x = 0; x < w / 2; x++) {
				du[x] = s[2 * x];
				dv[x] = s[2 * x + 1];
			}
		}
	} else if (!deep) {	/* yuv420p, yuvj420p */
		for (int y = 0; y < h; y++) {
			memcpy(dst + y * dstYStride, mFrame->data[0] + y * mFrame->linesize[0], w);
		}
		for (int y = 0; y < h / 2; y++) {
			memcpy(dstU + y * dstUVStride, mFrame->data[1] + y * mFrame->linesize[1], w / 2);
			memcpy(dstV + y * dstUVStride, mFrame->data[2] + y * mFrame->linesize[2], w / 2);
		}
	} else {		/* 10 bits, from software: down to 8 */
		int shift = desc->comp[0].depth - 8;
		for (int p = 0; p < 3; p++) {
			int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
			uint8_t *d = p == 0 ? dst : p == 1 ? dstU : dstV;
			size_t ds = p ? dstUVStride : dstYStride;
			for (int y = 0; y < ph; y++) {
				const uint16_t *s = (const uint16_t *)(mFrame->data[p] + y * mFrame->linesize[p]);
				for (int x = 0; x < pw; x++) {
					d[y * ds + x] = s[x] >> shift;
				}
			}
		}
	}
}

/* Decoded frames into free output buffers; false on a decoder error */
bool ZtHwDec::outputFrames(bool *portWillReset) {
	List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
	*portWillReset = false;
	while (!outQueue.empty()) {
		if (!mPending) {
			if (!mCtx) {
				break;
			}
			int err = avcodec_receive_frame(mCtx, mFrame);
			if (err == AVERROR(EAGAIN)) {
				break;
			}
			if (err == AVERROR_EOF) {
				if (mEOSStatus == INPUT_EOS_SEEN) {
					BufferInfo *outInfo = *outQueue.begin();
					outQueue.erase(outQueue.begin());
					OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;
					outHeader->nTimeStamp = 0;
					outHeader->nFilledLen = 0;
					outHeader->nFlags = OMX_BUFFERFLAG_EOS;
					outInfo->mOwnedByUs = false;
					notifyFillBufferDone(outHeader);
					mEOSStatus = OUTPUT_FRAMES_FLUSHED;
				}
				break;
			}
			if (err < 0) {
				if (mHw && fallBack()) {
					continue;
				}
				return false;
			}
			mPending = true;
		}
		if (!mSaid) {
			ALOGI("%s: %dx%d %s; container colours: range %d primaries %d transfer %d matrix %d",
					mHwName, mFrame->width, mFrame->height,
					av_get_pix_fmt_name((AVPixelFormat)mFrame->format),
					mDefaultColorAspects.mRange, mDefaultColorAspects.mPrimaries,
					mDefaultColorAspects.mTransfer, mDefaultColorAspects.mMatrixCoeffs);
			mSaid = true;
		}
		/* Waydroid draws an HDR surface black: Instagram's HDR reels (BT.2020
		 * HLG), 10-bit or their 8-bit renditions, which the container still
		 * calls HLG. They go out as BT.709 SDR (10-bit frames as 8 bits, in
		 * copyFrame), which shows them (HLG is made to be seen so). */
		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((AVPixelFormat)mFrame->format);
		ColorAspects aspects = {};	/* unspecified: the container's */
		ColorAspects::Transfer given = mDefaultColorAspects.mTransfer;
		if ((desc && desc->comp[0].depth > 8) || given == ColorAspects::TransferHLG ||
				given == ColorAspects::TransferST2084 ||
				mDefaultColorAspects.mPrimaries == ColorAspects::PrimariesBT2020 ||
				mDefaultColorAspects.mMatrixCoeffs == ColorAspects::MatrixBT2020 ||
				mDefaultColorAspects.mMatrixCoeffs == ColorAspects::MatrixBT2020Constant) {
			aspects.mRange = ColorAspects::RangeLimited;
			aspects.mPrimaries = ColorAspects::PrimariesBT709_5;
			aspects.mTransfer = ColorAspects::TransferSMPTE170M;
			aspects.mMatrixCoeffs = ColorAspects::MatrixBT709_5;
		}
		if (colorAspectsDiffer(aspects, mBitstreamColorAspects)) {
			mBitstreamColorAspects = aspects;
			handleColorAspectsChange();
		}
		handlePortSettingsChange(portWillReset, mFrame->width & ~1, mFrame->height & ~1);
		if (*portWillReset) {
			return true;	/* the frame stays pending until the port is back */
		}
		BufferInfo *outInfo = *outQueue.begin();
		OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;
		outHeader->nOffset = 0;
		outHeader->nFlags = 0;
		outHeader->nFilledLen = outputBufferWidth() * outputBufferHeight() * 3 / 2;
		outHeader->nTimeStamp = mFrame->best_effort_timestamp != AV_NOPTS_VALUE ?
				mFrame->best_effort_timestamp : mFrame->pts;
		if (outHeader->nAllocLen >= outHeader->nFilledLen) {
			copyFrame(outHeader->pBuffer);
		} else {
			ALOGE("%s: output buffer too small", mHwName);
			outHeader->nFilledLen = 0;
		}
		av_frame_unref(mFrame);
		mPending = false;
		outInfo->mOwnedByUs = false;
		outQueue.erase(outQueue.begin());
		notifyFillBufferDone(outHeader);
	}
	return true;
}

void ZtHwDec::onQueueFilled(OMX_U32) {
	if (mOutputPortSettingsChange != NONE || mEOSStatus == OUTPUT_FRAMES_FLUSHED) {
		return;
	}
	List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
	bool portWillReset = false;

	for (;;) {
		if (!outputFrames(&portWillReset)) {
			ALOGE("%s: decoding failed", mHwName);
			notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
			return;
		}
		/* a frame held back waits for an output buffer, and the input with it */
		if (portWillReset || mPending || mEOSStatus != INPUT_DATA_AVAILABLE || inQueue.empty()) {
			return;
		}
		BufferInfo *inInfo = *inQueue.begin();
		OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;
		const uint8_t *data = inHeader->pBuffer + inHeader->nOffset;
		size_t size = inHeader->nFilledLen;

		if (inHeader->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
			/* VP9 has none to give (vpcC is not needed to decode) */
			if (mId != AV_CODEC_ID_VP9 && size > 0) {
				if (mCtx && !mExtraDue) {
					mExtraSize = 0;	/* a new set replaces the one in use */
				}
				mExtra = (uint8_t *)realloc(mExtra, mExtraSize + size);
				memcpy(mExtra + mExtraSize, data, size);
				mExtraSize += size;
				/* (sent ahead of the next frame; before the decoder opens
				 * it is the decoder's extradata) */
				mExtraDue = mCtx != nullptr;
			}
			inQueue.erase(inQueue.begin());
			inInfo->mOwnedByUs = false;
			notifyEmptyBufferDone(inHeader);
			continue;
		}
		/* VP9 profile 2 and 3 (10 and 12 bits): not the MFC's */
		if (!mCtx && mId == AV_CODEC_ID_VP9 && size > 0 && (data[0] >> 6) == 2 &&
				((data[0] >> 4) & 1)) {
			mHwFailed = true;
		}
		if (!mCtx && !openDecoder()) {
			notify(OMX_EventError, OMX_ErrorInsufficientResources, 0, NULL);
			return;
		}
		int err = 0;
		if (size > 0) {
			av_packet_unref(mPkt);
			size_t extra = mExtraDue ? mExtraSize : 0;
			if (av_new_packet(mPkt, extra + size) < 0) {
				notify(OMX_EventError, OMX_ErrorInsufficientResources, 0, NULL);
				return;
			}
			if (extra) {
				memcpy(mPkt->data, mExtra, extra);
			}
			memcpy(mPkt->data + extra, data, size);
			mPkt->pts = inHeader->nTimeStamp;
			if (isKeyFrame(data, size)) {
				mPkt->flags |= AV_PKT_FLAG_KEY;
				dropReplay();
			}
			if (!mSaidIn) {
				ALOGI("%s: first input %d bytes: %02x %02x %02x %02x %02x %02x %02x %02x", mHwName,
						mPkt->size, mPkt->data[0], mPkt->data[1], mPkt->data[2], mPkt->data[3],
						mPkt->size > 4 ? mPkt->data[4] : 0, mPkt->size > 5 ? mPkt->data[5] : 0,
						mPkt->size > 6 ? mPkt->data[6] : 0, mPkt->size > 7 ? mPkt->data[7] : 0);
				mSaidIn = true;
			}
			err = sendPacket(mPkt);
			if (err == AVERROR(EAGAIN)) {
				continue;	/* the input waits for an output buffer */
			}
			if (err < 0) {
				ALOGE("%s: decoding failed (%d)", mHwName, err);
				notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
				return;
			}
			mExtraDue = false;
			if (mHw && mReplayCount < kMaxReplay) {
				mReplay[mReplayCount++] = av_packet_clone(mPkt);
			}
		}
		if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
			avcodec_send_packet(mCtx, nullptr);	/* drain */
			mEOSStatus = INPUT_EOS_SEEN;
		}
		inQueue.erase(inQueue.begin());
		inInfo->mOwnedByUs = false;
		notifyEmptyBufferDone(inHeader);
		if (mEOSStatus == INPUT_EOS_SEEN) {
			if (!outputFrames(&portWillReset)) {
				notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
			}
			return;
		}
	}
}

void ZtHwDec::onPortFlushCompleted(OMX_U32 portIndex) {
	if (portIndex == kInputPortIndex) {
		/* a seek, or the player taking this decoder for another video: the
		 * MFC's slot goes back (a player keeps decoders it is not using), and
		 * the next input, a key frame, opens one again if there is one free */
		if (mHw) {
			closeDecoder();
		} else if (mCtx) {
			avcodec_flush_buffers(mCtx);
		}
		av_frame_unref(mFrame);
		mPending = false;
		dropReplay();
		mEOSStatus = INPUT_DATA_AVAILABLE;
	}
}

void ZtHwDec::onReset() {
	SoftVideoDecoderOMXComponent::onReset();
	closeDecoder();
	dropReplay();
	mHwFailed = false;
	mSaid = false;
	mEOSStatus = INPUT_DATA_AVAILABLE;
}

}  // namespace android

typedef android::SoftOMXComponent *(*CreateFunc)(const char *, const OMX_CALLBACKTYPE *,
		OMX_PTR, OMX_COMPONENTTYPE **);

/* the original component, for what this one does not decode */
static android::SoftOMXComponent *original(const char *name, const OMX_CALLBACKTYPE *callbacks,
		OMX_PTR appData, OMX_COMPONENTTYPE **component) {
	void *lib = dlopen(SW_LIB, RTLD_NOW | RTLD_NODELETE);
	CreateFunc create = lib ? (CreateFunc)dlsym(lib,
			"_Z22createSoftOMXComponentPKcPK16OMX_CALLBACKTYPEPvPP17OMX_COMPONENTTYPE") : nullptr;
	if (!create) {
		ALOGE("no %s: %s", SW_LIB, dlerror());
		return nullptr;
	}
	return create(name, callbacks, appData, component);
}

__attribute__((visibility("default")))
android::SoftOMXComponent *createSoftOMXComponent(
		const char *name, const OMX_CALLBACKTYPE *callbacks,
		OMX_PTR appData, OMX_COMPONENTTYPE **component) {
	using namespace android;
	if (propInt("persist.ztos.hwdec", 1) == 0) {
		return original(name, callbacks, appData, component);
	}
	av_log_set_callback(avLog);
	if (!strcmp(name, "OMX.google.vp9.decoder")) {
		return new ZtHwDec(name, "video_decoder.vp9", OMX_VIDEO_CodingVP9,
				kVP9ProfileLevels, NELEM(kVP9ProfileLevels), "video/x-vnd.on2.vp9",
				AV_CODEC_ID_VP9, "vp9_v4l2m2m", callbacks, appData, component);
	}
	if (!strcmp(name, "OMX.google.h264.decoder")) {
		return new ZtHwDec(name, "video_decoder.avc", OMX_VIDEO_CodingAVC,
				kAVCProfileLevels, NELEM(kAVCProfileLevels), "video/avc",
				AV_CODEC_ID_H264, "h264_v4l2m2m", callbacks, appData, component);
	}
	if (!strcmp(name, "OMX.google.hevc.decoder")) {
		return new ZtHwDec(name, "video_decoder.hevc", OMX_VIDEO_CodingHEVC,
				kHEVCProfileLevels, NELEM(kHEVCProfileLevels), "video/hevc",
				AV_CODEC_ID_HEVC, "hevc_v4l2m2m", callbacks, appData, component);
	}
	return original(name, callbacks, appData, component);
}

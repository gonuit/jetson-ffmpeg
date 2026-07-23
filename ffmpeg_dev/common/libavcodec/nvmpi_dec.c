#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#include <nvmpi.h>
#include "avcodec.h"
#include "decode.h"
#include "internal.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"

#if LIBAVCODEC_VERSION_MAJOR >= 60
#include "codec_internal.h"
#endif

#define OPT_frame_pool_size_MIN 1
#define OPT_frame_pool_size_MAX 32
#define OPT_frame_pool_size_DEFAULT 5

typedef struct {
	//the AVClass pointer must be the first member: libavcodec writes
	//codec->priv_class to the start of priv_data (the old layout with
	//eos_reached first got silently clobbered by that pointer)
	AVClass *av_class;
	nvmpictx* ctx;
	AVFrame *bufFrame;
	char *resize_expr;
	char *crop_expr;
	int rotate;
	int flip;
	int frame_pool_size;
	char eos_reached;
} nvmpiDecodeContext;

//the closed-source nv v4l2 plugin prints a banner to stdout when the decoder
//device is opened, which corrupts piped output (-f rawvideo pipe:1). Shadow
//stdout with stderr while the device is created.
static int nvmpi_shadow_stdout(void)
{
	int fd;
	fflush(stdout);
	fd = dup(1);
	if(fd >= 0) dup2(2, 1);
	return fd;
}

static void nvmpi_restore_stdout(int fd)
{
	if(fd < 0) return;
	fflush(stdout);
	dup2(fd, 1);
	close(fd);
}

static nvCodingType nvmpi_get_codingtype(AVCodecContext *avctx)
{
	switch (avctx->codec_id) {
		case AV_CODEC_ID_H264:          return NV_VIDEO_CodingH264;
		case AV_CODEC_ID_HEVC:          return NV_VIDEO_CodingHEVC;
		case AV_CODEC_ID_AV1:           return NV_VIDEO_CodingAV1;
		case AV_CODEC_ID_VP8:           return NV_VIDEO_CodingVP8;
		case AV_CODEC_ID_VP9:           return NV_VIDEO_CodingVP9;
		case AV_CODEC_ID_MPEG4:		return NV_VIDEO_CodingMPEG4;
		case AV_CODEC_ID_MPEG2VIDEO:    return NV_VIDEO_CodingMPEG2;
		default:                        return NV_VIDEO_CodingUnused;
	}
};


static int nvmpi_init_decoder(AVCodecContext *avctx)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	nvDecParam param={0};
	
	param.codingType =nvmpi_get_codingtype(avctx);
	if (param.codingType == NV_VIDEO_CodingUnused)
	{
		av_log(avctx, AV_LOG_ERROR, "Unknown codec type (%d).\n", avctx->codec_id);
		return AVERROR_UNKNOWN;
	}
	
	param.frame_pool_size = nvmpi_context->frame_pool_size;
	if(param.frame_pool_size < OPT_frame_pool_size_MIN || param.frame_pool_size > OPT_frame_pool_size_MAX)
	{
		av_log(avctx, AV_LOG_WARNING, "Incorrect frame_pool_size specified: %d. Default (%d) will be used.\n", param.frame_pool_size, OPT_frame_pool_size_DEFAULT);
		param.frame_pool_size = OPT_frame_pool_size_DEFAULT;
	}

	//Workaround for default pix_fmt not being set, so check if it isnt set and set it,
	//or if it is set, but isnt set to something we can work with.
	if(avctx->pix_fmt ==AV_PIX_FMT_NONE)
	{
		 avctx->pix_fmt=AV_PIX_FMT_YUV420P;
		 param.pixFormat = NV_PIX_YUV420;
	}
	else if(avctx->pix_fmt == AV_PIX_FMT_YUV420P10LE || avctx->pix_fmt == AV_PIX_FMT_P010LE)
	{
		//10-bit stream (HEVC Main 10 / VP9 profile 2): decode to P010
		avctx->pix_fmt = AV_PIX_FMT_P010LE;
		param.pixFormat = NV_PIX_P010;
	}
	else if((avctx->pix_fmt != AV_PIX_FMT_YUV420P) && (avctx->pix_fmt != AV_PIX_FMT_YUVJ420P))
	{
		av_log(avctx, AV_LOG_ERROR, "Invalid Pix_FMT for NVMPI: Only YUV420P, YUVJ420P and 10-bit (P010) input are supported\n");
		return AVERROR_INVALIDDATA;
	}
	else
	{
		param.pixFormat = NV_PIX_YUV420;
	}

    if (nvmpi_context->resize_expr && sscanf(nvmpi_context->resize_expr, "%dx%d",
                                             &param.resized.width, &param.resized.height) != 2)
	{
        av_log(avctx, AV_LOG_ERROR, "Invalid resize expressions\n");
        return AVERROR(EINVAL);
    }

	//4:2:0 output needs even geometry; it also keeps the wrapper and the
	//library agreeing on the exact output size
	if((param.resized.width & 1) || (param.resized.height & 1))
	{
		param.resized.width &= ~1;
		param.resized.height &= ~1;
		av_log(avctx, AV_LOG_INFO, "resize adjusted to even geometry: %ux%u\n",
		       param.resized.width, param.resized.height);
	}

	if(nvmpi_context->crop_expr)
	{
		int top, bottom, left, right, cw, ch;
		if(sscanf(nvmpi_context->crop_expr, "%dx%dx%dx%d", &top, &bottom, &left, &right) != 4 ||
		   top < 0 || bottom < 0 || left < 0 || right < 0)
		{
			av_log(avctx, AV_LOG_ERROR, "Invalid cropping expressions\n");
			return AVERROR(EINVAL);
		}
		if(avctx->width <= 0 || avctx->height <= 0)
		{
			av_log(avctx, AV_LOG_ERROR, "crop requires known stream dimensions\n");
			return AVERROR(EINVAL);
		}
		if(left + right >= avctx->width || top + bottom >= avctx->height)
		{
			av_log(avctx, AV_LOG_ERROR, "crop %dx%dx%dx%d exceeds stream dimensions %dx%d\n",
			       top, bottom, left, right, avctx->width, avctx->height);
			return AVERROR(EINVAL);
		}
		cw = avctx->width - left - right;
		ch = avctx->height - top - bottom;
		param.srcCrop.left = left & ~1;
		param.srcCrop.top = top & ~1;
		param.srcCrop.width = cw & ~1;
		param.srcCrop.height = ch & ~1;
		if((left | top | cw | ch) & 1)
			av_log(avctx, AV_LOG_INFO, "crop rect adjusted to even geometry: %ux%u+%u+%u\n",
			       param.srcCrop.width, param.srcCrop.height, param.srcCrop.left, param.srcCrop.top);
	}

	switch(nvmpi_context->rotate)
	{
		case 0: break;
		case 90: param.transform = NV_TRANSFORM_ROTATE90; break;
		case 180: param.transform = NV_TRANSFORM_ROTATE180; break;
		case 270: param.transform = NV_TRANSFORM_ROTATE270; break;
		default:
			av_log(avctx, AV_LOG_ERROR, "rotate must be one of 0, 90, 180, 270\n");
			return AVERROR(EINVAL);
	}
	if(nvmpi_context->flip)
	{
		if(param.transform != NV_TRANSFORM_NONE)
		{
			av_log(avctx, AV_LOG_ERROR, "rotate and flip are mutually exclusive\n");
			return AVERROR(EINVAL);
		}
		param.transform = (nvmpi_context->flip == 1) ? NV_TRANSFORM_FLIP_H : NV_TRANSFORM_FLIP_V;
	}

	//final output size: crop selects the source region, resize scales it,
	//rotation/flip is applied last (90/270 swap the dimensions)
	{
		unsigned int out_w = avctx->width, out_h = avctx->height;
		if(param.srcCrop.width)
		{
			out_w = param.srcCrop.width;
			out_h = param.srcCrop.height;
		}
		if(param.resized.width && param.resized.height)
		{
			out_w = param.resized.width;
			out_h = param.resized.height;
		}
		if(param.transform == NV_TRANSFORM_ROTATE90 || param.transform == NV_TRANSFORM_ROTATE270)
			FFSWAP(unsigned int, out_w, out_h);
		if(out_w && out_h)
		{
			avctx->width = out_w;
			avctx->height = out_h;
		}
	}

	nvmpi_context->bufFrame = av_frame_alloc();
	nvmpi_context->bufFrame->width = avctx->width;
	nvmpi_context->bufFrame->height = avctx->height;
	if (ff_get_buffer(avctx, nvmpi_context->bufFrame, 0) < 0)
	{
		av_frame_free(&(nvmpi_context->bufFrame));
		nvmpi_context->bufFrame = NULL;
		return AVERROR(ENOMEM);
	}

	int saved_stdout = nvmpi_shadow_stdout();
	nvmpi_context->ctx=nvmpi_create_decoder(&param);
	nvmpi_restore_stdout(saved_stdout);

	if(!nvmpi_context->ctx)
	{
		av_frame_free(&(nvmpi_context->bufFrame));
		nvmpi_context->bufFrame = NULL;
		av_log(avctx, AV_LOG_ERROR, "Failed to nvmpi_create_decoder (code = %d).\n", AVERROR_EXTERNAL);
		return AVERROR_EXTERNAL;
	}
	
   return 0;
}

static int nvmpi_close(AVCodecContext *avctx)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	if(nvmpi_context->bufFrame)
	{
		av_frame_free(&(nvmpi_context->bufFrame));
		nvmpi_context->bufFrame = NULL;
	}
	return nvmpi_decoder_close(nvmpi_context->ctx);
}

#if LIBAVCODEC_VERSION_MAJOR >= 60
static int nvmpi_decode(AVCodecContext *avctx, AVFrame *data, int *got_frame, AVPacket *avpkt)
#else
static int nvmpi_decode(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
#endif
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	AVFrame *frame = data;
	AVFrame *bufFrame = nvmpi_context->bufFrame;
	nvFrame _nvframe={0};
	nvPacket packet;
	int res;
	int decode_ret = avpkt->size;

	_nvframe.payload[0] = bufFrame->data[0];
	_nvframe.payload[1] = bufFrame->data[1];
	_nvframe.payload[2] = bufFrame->data[2];
	_nvframe.linesize[0] = bufFrame->linesize[0];
	_nvframe.linesize[1] = bufFrame->linesize[1];
	_nvframe.linesize[2] = bufFrame->linesize[2];

	if(avpkt->size)
	{
		packet.payload_size=avpkt->size;
		packet.payload=avpkt->data;
		packet.pts=avpkt->pts;

		res=nvmpi_decoder_put_packet(nvmpi_context->ctx,&packet);
		if(res < 0)
		{
			if(res == -1)
			{
				decode_ret = AVERROR(EAGAIN); //TODO log
			}
			//TODO error handling
		}

		res=nvmpi_decoder_get_frame(nvmpi_context->ctx,&_nvframe,avctx->flags & AV_CODEC_FLAG_LOW_DELAY);
	}
	else
	{
		//draining. Sweep already-decoded frames non-blocking first: sending the
		//flush packet below can block on a free output-plane buffer, which the
		//decoder can only release once the frame pool has space, so the flush
		//must only be queued when the pool is empty (deadlock otherwise).
		res=nvmpi_decoder_get_frame(nvmpi_context->ctx,&_nvframe,false);
		if(res<0)
		{
			if(!nvmpi_context->eos_reached)
			{
				//zero-sized packet starts the decoder flush; the capture side
				//signals completion (V4L2_BUF_FLAG_LAST) through get_frame
				packet.payload_size=0;
				packet.payload=NULL;
				packet.pts=0;
				nvmpi_decoder_put_packet(nvmpi_context->ctx,&packet);
				nvmpi_context->eos_reached=1;
			}
			//block until the flush delivers the next remaining frame or ends
			res=nvmpi_decoder_get_frame(nvmpi_context->ctx,&_nvframe,true);
		}
	}

	if(res<0)
	{
		return decode_ret;
	}

	bufFrame->format=avctx->pix_fmt;
	bufFrame->pts=_nvframe.timestamp;
	bufFrame->pkt_dts = AV_NOPTS_VALUE;
	av_frame_move_ref(frame, bufFrame);
	
	*got_frame = 1;
	
	bufFrame->width = avctx->width;
	bufFrame->height = avctx->height;
	if (ff_get_buffer(avctx, bufFrame, 0) < 0)
	{
		av_log(avctx, AV_LOG_ERROR, "ff_get_buffer failed\n");
		return AVERROR(ENOMEM);
	}
	
	frame->metadata = bufFrame->metadata;
	bufFrame->metadata = NULL;

	return decode_ret;
}



#define OFFSET(x) offsetof(nvmpiDecodeContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "resize",   "Resize (width)x(height)", OFFSET(resize_expr), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VD, "resize" },
    { "crop",     "Crop (top)x(bottom)x(left)x(right) of the source before scaling", OFFSET(crop_expr), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VD, "crop" },
    { "rotate",   "Rotate output clockwise, applied after crop and resize (90/270 swap the output dimensions)", OFFSET(rotate), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 270, VD, "rotate" },
    { "0",   "no rotation", 0, AV_OPT_TYPE_CONST, { .i64 = 0 },   0, 0, VD, "rotate" },
    { "90",  "",            0, AV_OPT_TYPE_CONST, { .i64 = 90 },  0, 0, VD, "rotate" },
    { "180", "",            0, AV_OPT_TYPE_CONST, { .i64 = 180 }, 0, 0, VD, "rotate" },
    { "270", "",            0, AV_OPT_TYPE_CONST, { .i64 = 270 }, 0, 0, VD, "rotate" },
    { "flip",     "Mirror output, mutually exclusive with rotate", OFFSET(flip), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 2, VD, "flip" },
    { "none", "no mirroring",      0, AV_OPT_TYPE_CONST, { .i64 = 0 }, 0, 0, VD, "flip" },
    { "h",    "horizontal mirror", 0, AV_OPT_TYPE_CONST, { .i64 = 1 }, 0, 0, VD, "flip" },
    { "v",    "vertical mirror",   0, AV_OPT_TYPE_CONST, { .i64 = 2 }, 0, 0, VD, "flip" },
    { "frame_pool_size", "Number of frames that could be buffered in the decoder before user must read it with avcodec_receive_frame()", OFFSET(frame_pool_size), AV_OPT_TYPE_INT, {.i64 = OPT_frame_pool_size_DEFAULT }, OPT_frame_pool_size_MIN, OPT_frame_pool_size_MAX, VD, "frame_pool_size" },
    { NULL }
};

#define NVMPI_DEC_CLASS(NAME) \
	static const AVClass nvmpi_##NAME##_dec_class = { \
		.class_name = "nvmpi_" #NAME "_dec", \
		.option     = options, \
		.version    = LIBAVUTIL_VERSION_INT, \
	};

#if LIBAVCODEC_VERSION_MAJOR >= 60
	#define NVMPI_DEC(NAME, ID, BSFS) \
		NVMPI_DEC_CLASS(NAME) \
		FFCodec ff_##NAME##_nvmpi_decoder = { \
			.p.name           = #NAME "_nvmpi", \
			CODEC_LONG_NAME(#NAME " (nvmpi)"), \
			.p.type           = AVMEDIA_TYPE_VIDEO, \
			.p.id             = ID, \
			.priv_data_size = sizeof(nvmpiDecodeContext), \
			.init           = nvmpi_init_decoder, \
			.close          = nvmpi_close, \
			FF_CODEC_DECODE_CB(nvmpi_decode), \
			.p.priv_class     = &nvmpi_##NAME##_dec_class, \
			.p.capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
			.p.pix_fmts	=(const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P,AV_PIX_FMT_NV12,AV_PIX_FMT_P010LE,AV_PIX_FMT_NONE},\
			.bsfs           = BSFS, \
			.p.wrapper_name   = "nvmpi", \
		};
#else
	#define NVMPI_DEC(NAME, ID, BSFS) \
		NVMPI_DEC_CLASS(NAME) \
		AVCodec ff_##NAME##_nvmpi_decoder = { \
			.name           = #NAME "_nvmpi", \
			.long_name      = NULL_IF_CONFIG_SMALL(#NAME " (nvmpi)"), \
			.type           = AVMEDIA_TYPE_VIDEO, \
			.id             = ID, \
			.priv_data_size = sizeof(nvmpiDecodeContext), \
			.init           = nvmpi_init_decoder, \
			.close          = nvmpi_close, \
			.decode         = nvmpi_decode, \
			.priv_class     = &nvmpi_##NAME##_dec_class, \
			.capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
			.pix_fmts	=(const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P,AV_PIX_FMT_NV12,AV_PIX_FMT_P010LE,AV_PIX_FMT_NONE},\
			.bsfs           = BSFS, \
			.wrapper_name   = "nvmpi", \
		};
#endif


NVMPI_DEC(h264,  AV_CODEC_ID_H264,"h264_mp4toannexb");
NVMPI_DEC(hevc,  AV_CODEC_ID_HEVC,"hevc_mp4toannexb");
NVMPI_DEC(mpeg2, AV_CODEC_ID_MPEG2VIDEO,NULL);
NVMPI_DEC(mpeg4, AV_CODEC_ID_MPEG4,NULL);
NVMPI_DEC(vp9,  AV_CODEC_ID_VP9,NULL);
NVMPI_DEC(vp8, AV_CODEC_ID_VP8,NULL);
NVMPI_DEC(av1, AV_CODEC_ID_AV1,NULL);


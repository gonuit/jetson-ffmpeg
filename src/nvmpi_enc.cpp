#include "nvmpi.h"
#include "NvVideoEncoder.h"
#include "nvUtils2NvBuf.h"
#include "NVMPI_bufPool.hpp"
#include <fcntl.h>
#include <malloc.h>
#include <vector>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <unistd.h>

#define MAX_BUFFERS 32
#define TEST_ERROR(condition, message, errorCode)    \
	if (condition)                               \
{                                                    \
	std::cerr<< message;                         \
}

#define OUTPLANE_MEMTYPE_MMAP 0
#define OUTPLANE_MEMTYPE_DMA 1
#define OUTPLANE_MEMTYPE OUTPLANE_MEMTYPE_MMAP

using namespace std;

struct nvmpictx
{
	uint32_t index;
	uint32_t width;
	uint32_t height;
	uint32_t profile;
	uint32_t bitrate;
	uint32_t peak_bitrate;
	uint32_t raw_pixfmt;
	uint32_t encoder_pixfmt;
	uint32_t iframe_interval;
	uint32_t idr_interval;
	uint32_t fps_n;
	uint32_t fps_d;
	uint32_t qmax;
	uint32_t qmin;
	uint32_t num_b_frames;
	uint32_t num_reference_frames;
	uint32_t vbv_buffer_size; //virtual buffer size of the encoder
	uint32_t packets_num;

	bool insert_sps_pps_at_idr;
	bool max_perf; //enable max performance mode
	bool enable_extended_colorformat;
	bool enableLossless;
	bool blocking_mode;
	//shared between the DQ thread and the API threads
	std::atomic<bool> capPlaneGotEOS;
	std::atomic<bool> flushing;
	std::atomic<bool> enc_shutdown;   //set by nvmpi_encoder_close to release a waiting DQ callback
	std::atomic<bool> dqThreadRunning;

	enum v4l2_mpeg_video_bitrate_mode ratecontrol;
	enum v4l2_mpeg_video_h264_level level;
	enum v4l2_enc_hw_preset_type hw_preset_type;

	NvVideoEncoder *enc;
	NVMPI_bufPool<nvPacket*>* pktPool;
	int *output_plane_fd; //array to store dmabuf fd's
};


static bool encoder_capture_plane_dq_callback(struct v4l2_buffer *v4l2_buf, NvBuffer * buffer, NvBuffer * shared_buffer __attribute__((unused)), void *arg)
{
	nvmpictx *ctx = (nvmpictx*)arg;

	if (v4l2_buf == NULL)
	{
		cerr << "Error while dequeing buffer from output plane" << endl;
		ctx->dqThreadRunning = false;
		return false;
	}

	if (buffer->planes[0].bytesused == 0)
	{
		ctx->capPlaneGotEOS = true;
		ctx->dqThreadRunning = false;
		return false;
	}

	v4l2_ctrl_videoenc_outputbuf_metadata enc_metadata;
	ctx->enc->getMetadata(v4l2_buf->index, enc_metadata);

	//nvPacket.payload --> AVPacket->data
	//nvPacket.privData --> AVPacket
	nvPacket* pkt = ctx->pktPool->dqEmptyBuf();
	if(!pkt)
	{
		//Backpressure: hold this capture buffer (stalling the HW encoder) until
		//the consumer frees a packet slot. The wait is time-capped: a consumer
		//that abandons the stream and goes straight to close would otherwise
		//deadlock against put_frame(NULL) blocking in DQBUF before
		//enc_shutdown can ever be set.
		const int max_wait_ms = 10000;
		int waited_ms = 0;
		while(!pkt && !ctx->enc_shutdown && !ctx->enc->isInError() && waited_ms < max_wait_ms)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			waited_ms++;
			pkt = ctx->pktPool->dqEmptyBuf();
		}
		if(!pkt && (ctx->enc_shutdown || ctx->enc->isInError()))
		{
			//teardown or hard error: this packet is lost by design
			ctx->dqThreadRunning = false;
			return false;
		}
		if(!pkt)
		{
			fprintf(stderr, "[libnvmpi][W]: packet pool empty for %d ms, dropping packet. There may be artifacts in the output video.\n", max_wait_ms);
		}
	}
	if(pkt)
	{
		pkt->pts = (v4l2_buf->timestamp.tv_usec % 1000000) + (v4l2_buf->timestamp.tv_sec * 1000000UL);
		//AV_PKT_FLAG_KEY 0x0001. if current packet is keyframe then enc_metadata.KeyFrame should be 0x1, so it should be OK to just assign value
		pkt->flags = enc_metadata.KeyFrame;
		pkt->payload_size = buffer->planes[0].bytesused;
		memcpy(pkt->payload, buffer->planes[0].data, pkt->payload_size);

		ctx->pktPool->qFilledBuf(pkt);
	}

	if (ctx->enc->capture_plane.qBuffer(*v4l2_buf, NULL) < 0)
	{
		ERROR_MSG("Error while Qing buffer at capture plane");
		ctx->dqThreadRunning = false;
		return false;
	}

	return true;
}

#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)
static int setup_output_dmabuf(nvmpictx *ctx, uint32_t num_buffers )
{
    int ret=0;
    NvBufSurf::NvCommonAllocateParams cParams;
    int fd;
    ret = ctx->enc->output_plane.reqbufs(V4L2_MEMORY_DMABUF,num_buffers);
    if(ret)
    {
        cerr << "reqbufs failed for output plane V4L2_MEMORY_DMABUF" << endl;
        return ret;
    }
    for (uint32_t i = 0; i < ctx->enc->output_plane.getNumBuffers(); i++)
    {
        cParams.width = ctx->width;
        cParams.height = ctx->height;
        cParams.layout = NVBUF_LAYOUT_PITCH;
        
        /*
        switch (ctx->cs)
        {
            case V4L2_COLORSPACE_REC709:
                cParams.colorFormat = ctx->enable_extended_colorformat ?
                    NVBUF_COLOR_FORMAT_YUV420_709_ER : NVBUF_COLOR_FORMAT_YUV420_709;
                break;
            case V4L2_COLORSPACE_SMPTE170M:
            default:
                cParams.colorFormat = ctx->enable_extended_colorformat ?
                    NVBUF_COLOR_FORMAT_YUV420_ER : NVBUF_COLOR_FORMAT_YUV420;
        }
        if (ctx->is_semiplanar)
        {
            cParams.colorFormat = NVBUF_COLOR_FORMAT_NV12;
        }
        if (ctx->encoder_pixfmt == V4L2_PIX_FMT_H264)
        {
            if (ctx->enableLossless)
            {
                if (ctx->is_semiplanar)
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_NV24;
                else
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_YUV444;
            }
        }
        
        
        else if (ctx->encoder_pixfmt == V4L2_PIX_FMT_H265)
        {
            if (ctx->chroma_format_idc == 3)
            {
                if (ctx->is_semiplanar)
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_NV24;
                else
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_YUV444;

                if (ctx->bit_depth == 10)
                    cParams.colorFormat = NVBUF_COLOR_FORMAT_NV24_10LE;
            }
            if (ctx->profile == V4L2_MPEG_VIDEO_H265_PROFILE_MAIN10 && (ctx->bit_depth == 10))
            {
                cParams.colorFormat = NVBUF_COLOR_FORMAT_NV12_10LE;
            }
        }
        */
        
        cParams.colorFormat = NVBUF_COLOR_FORMAT_YUV420;
        cParams.memtag = NvBufSurfaceTag_VIDEO_ENC;
        cParams.memType = NVBUF_MEM_SURFACE_ARRAY;
        // Create output plane fd for DMABUF io-mode
        ret = NvBufSurf::NvAllocate(&cParams, 1, &fd);
        if(ret < 0)
        {
            cerr << "Failed to create NvBuffer" << endl;
            return ret;
        }
        ctx->output_plane_fd[i]=fd;
    }
    return ret;
}
#endif


//teardown of a partially initialized encoder context; returns NULL so
//creation sites can fail in one expression
static nvmpictx* nvmpi_enc_create_fail(nvmpictx* ctx)
{
	std::cerr << "[libnvmpi][E]: encoder creation failed" << std::endl;
	ctx->enc_shutdown = true;
	if(ctx->enc) { delete ctx->enc; ctx->enc = nullptr; }
	if(ctx->pktPool) { delete ctx->pktPool; ctx->pktPool = nullptr; }
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)
	delete[] ctx->output_plane_fd;
#endif
	delete ctx;
	return NULL;
}

#define ENC_CHECK(condition, message)                        \
	if (condition)                                       \
{                                                            \
	std::cerr << message << std::endl;                   \
	return nvmpi_enc_create_fail(ctx);                   \
}

nvmpictx* nvmpi_create_encoder(nvEncParam* param)
{
	int ret;
	log_level = LOG_LEVEL_INFO;
	//log_level = LOG_LEVEL_DEBUG;
	nvmpictx *ctx=new nvmpictx;
	ctx->enc=NULL;
	ctx->index=0;
	ctx->width=param->width;
	ctx->height=param->height;
	ctx->enableLossless=false;
	ctx->bitrate=param->bitrate;
	ctx->ratecontrol = V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;	
	ctx->idr_interval = param->idr_interval;
	ctx->fps_n = param->fps_n;
	ctx->fps_d = param->fps_d;
	ctx->iframe_interval = param->iframe_interval;
	ctx->pktPool = new NVMPI_bufPool<nvPacket*>();
	ctx->enable_extended_colorformat=false;
	ctx->packets_num=param->capture_num;
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)
	ctx->output_plane_fd = new int[ctx->packets_num];
#endif
	ctx->qmax=param->qmax;
	ctx->qmin=param->qmin;
	ctx->num_b_frames=param->max_b_frames;
	ctx->num_reference_frames=param->refs;
	ctx->insert_sps_pps_at_idr=(param->insert_spspps_idr==1)?true:false;
	ctx->capPlaneGotEOS = false;
	ctx->flushing = false;
	ctx->enc_shutdown = false;
	ctx->dqThreadRunning = false;
	ctx->blocking_mode = true; //TODO non-blocking mode support
	ctx->max_perf = true; //TODO invistigate why encoder is slow without max_perf even with MAXN power mode
	ctx->vbv_buffer_size = param->vbv_buffer_size;
	
	switch(param->profile)
	{
		case 77://FF_PROFILE_H264_MAIN
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
			break;
		case 66://FF_PROFILE_H264_BASELINE
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
			break;
		case 100://FF_PROFILE_H264_HIGH
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
			break;

		default:
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
			break;
	}

	switch(param->level)
	{
		case 10:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_0;
			break;
		case 11:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_1;
			break;
		case 12:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_2;
			break;
		case 13:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_3;
			break;
		case 20:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_0;
			break;
		case 21:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_1;
			break;
		case 22:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_2;
			break;
		case 30:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_0;
			break;
		case 31:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_1;
			break;
		case 32:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_2;
			break;
		case 40:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
			break;
		case 41:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_1;
			break;
		case 42:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
			break;
		case 50:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_0;
			break;
		case 51:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
			break;
		default:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
			break;	
	}

	switch(param->hw_preset_type)
	{
		case 1:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_ULTRAFAST;
			break;
		case 2:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_FAST;
			break;
		case 3:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_MEDIUM;
			break;
		case 4:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_SLOW;
			break;
		default:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_MEDIUM;
			break;
	}

	if(param->enableLossless)
		ctx->enableLossless=true;

	if(param->mode_vbr)
		ctx->ratecontrol=V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;

	if(param->codingType==NV_VIDEO_CodingH264)
	{
		ctx->encoder_pixfmt=V4L2_PIX_FMT_H264;
	}else if(param->codingType==NV_VIDEO_CodingHEVC)
	{
		ctx->encoder_pixfmt=V4L2_PIX_FMT_H265;
	}
#ifdef V4L2_PIX_FMT_AV1
	else if(param->codingType==NV_VIDEO_CodingAV1)
	{
		ctx->encoder_pixfmt=V4L2_PIX_FMT_AV1;
	}
#endif
	if(ctx->blocking_mode)
	{
		ctx->enc=NvVideoEncoder::createVideoEncoder("enc0");
	}
	else
	{
		ctx->enc = NvVideoEncoder::createVideoEncoder("enc0", O_NONBLOCK);
	}
	ENC_CHECK(!ctx->enc, "Could not create encoder");

	ret = ctx->enc->setCapturePlaneFormat(ctx->encoder_pixfmt, ctx->width,ctx->height, NVMPI_ENC_CHUNK_SIZE);

	ENC_CHECK(ret < 0, "Could not set output plane format");

#ifdef V4L2_CID_MPEG_VIDEOENC_AV1_HEADERS_WITH_FRAME
	if(param->codingType==NV_VIDEO_CodingAV1)
	{
		//the HW emits IVF-wrapped output by default; disable it to get raw OBUs.
		//gst nvv4l2av1enc (enable-headers=false) sets this control right after the
		//capture plane S_FMT, before the output plane format — same order here.
		//NvVideoEncoder has no wrapper method for this control, so set it directly.
		struct v4l2_ext_control ectrl;
		struct v4l2_ext_controls ectrls;
		memset(&ectrl, 0, sizeof(ectrl));
		memset(&ectrls, 0, sizeof(ectrls));
		ectrls.count = 1;
		ectrls.controls = &ectrl;
		ectrls.ctrl_class = V4L2_CTRL_CLASS_MPEG;
		ectrl.id = V4L2_CID_MPEG_VIDEOENC_AV1_HEADERS_WITH_FRAME;
		ectrl.value = 0;
		ret = ctx->enc->setExtControls(ectrls);
		ENC_CHECK(ret < 0, "Could not disable AV1 IVF headers");
	}
#endif

	switch (ctx->profile)
	{
		case V4L2_MPEG_VIDEO_H265_PROFILE_MAIN10:
			ctx->raw_pixfmt = V4L2_PIX_FMT_P010M;
			break;
		case V4L2_MPEG_VIDEO_H265_PROFILE_MAIN:
		default:
			ctx->raw_pixfmt = V4L2_PIX_FMT_YUV420M;
	}

	//10-bit input implies HEVC Main 10 (the only 10-bit-capable codec of the
	//encoder: see the format table in v4l2_nv_extensions.h; AV1 is 8-bit only)
	if(param->inputPixFormat==NV_PIX_P010 && param->codingType==NV_VIDEO_CodingHEVC)
	{
		ctx->profile = V4L2_MPEG_VIDEO_H265_PROFILE_MAIN10;
		ctx->raw_pixfmt = V4L2_PIX_FMT_P010M;
	}

	if (ctx->enableLossless && param->codingType == NV_VIDEO_CodingH264)
	{
		ctx->profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE;
		ret = ctx->enc->setOutputPlaneFormat(V4L2_PIX_FMT_YUV444M, ctx->width,ctx->height);
	}
	else
	{
		ret = ctx->enc->setOutputPlaneFormat(ctx->raw_pixfmt, ctx->width,ctx->height);
	}

	ENC_CHECK(ret < 0, "Could not set output plane format");

	ret = ctx->enc->setBitrate(ctx->bitrate);
	ENC_CHECK(ret < 0, "Could not set encoder bitrate");

	if(ctx->vbv_buffer_size)
	{
		/* Set virtual buffer size value for encoder */
		ret = ctx->enc->setVirtualBufferSize(ctx->vbv_buffer_size);
		ENC_CHECK(ret < 0, "Could not set virtual buffer size");
	}

	ret=ctx->enc->setHWPresetType(ctx->hw_preset_type);
	ENC_CHECK(ret < 0, "Could not set encoder HW Preset Type");

	if(ctx->num_reference_frames)
	{
		ret = ctx->enc->setNumReferenceFrames(ctx->num_reference_frames);
		ENC_CHECK(ret < 0, "Could not set num reference frames");
	}

	if(ctx->num_b_frames != (uint32_t) -1 && param->codingType == NV_VIDEO_CodingH264)
	{
		ret = ctx->enc->setNumBFrames(ctx->num_b_frames);
		ENC_CHECK(ret < 0, "Could not set number of B Frames");
	}


	if(param->codingType == NV_VIDEO_CodingH264 || param->codingType == NV_VIDEO_CodingHEVC)
	{
		ret = ctx->enc->setProfile(ctx->profile);
		ENC_CHECK(ret < 0, "Could not set encoder profile");
	}

	if(param->codingType== NV_VIDEO_CodingH264)
	{
		ret = ctx->enc->setLevel(ctx->level);
		ENC_CHECK(ret < 0, "Could not set encoder level");
	}


	if (ctx->enableLossless)
	{
		ret = ctx->enc->setConstantQp(0);
		ENC_CHECK(ret < 0, "Could not set encoder constant qp=0");

	}
	else
	{
		ret = ctx->enc->setRateControlMode(ctx->ratecontrol);
		ENC_CHECK(ret < 0, "Could not set encoder rate control mode");

		if (ctx->ratecontrol == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR)
		{
			uint32_t peak_bitrate;
			//TODO log warning?
			if (ctx->peak_bitrate < ctx->bitrate)
				peak_bitrate = 1.2f * ctx->bitrate;
			else
				peak_bitrate = ctx->peak_bitrate;
			ret = ctx->enc->setPeakBitrate(peak_bitrate);
			ENC_CHECK(ret < 0, "Could not set encoder peak bitrate");
		}
	}

	ret = ctx->enc->setIDRInterval(ctx->idr_interval);
	ENC_CHECK(ret < 0, "Could not set encoder IDR interval");

	if(ctx->qmax>0 ||ctx->qmin >0){
		ctx->enc->setQpRange(ctx->qmin, ctx->qmax, ctx->qmin,ctx->qmax, ctx->qmin, ctx->qmax);	
	}
	ret = ctx->enc->setIFrameInterval(ctx->iframe_interval);
	ENC_CHECK(ret < 0, "Could not set encoder I-Frame interval");
	
    if(ctx->max_perf)
    {
        /* Enable maximum performance mode by disabling internal DFS logic.
           NOTE: This enables encoder to run at max clocks */
		ret = ctx->enc->setMaxPerfMode(ctx->max_perf);
		ENC_CHECK(ret < 0, "Error while setting encoder to max perf");
	}
	
	//SPS/PPS insertion is an H.264/HEVC concept, do not apply it to other codecs
	if(ctx->insert_sps_pps_at_idr &&
	   (param->codingType==NV_VIDEO_CodingH264 || param->codingType==NV_VIDEO_CodingHEVC)){
		ret = ctx->enc->setInsertSpsPpsAtIdrEnabled(true);
		ENC_CHECK(ret < 0, "Could not set insertSPSPPSAtIDR");
	}

#ifdef V4L2_CID_MPEG_VIDEOENC_AV1_ERR_RESILIENT_MODE
	if(param->codingType==NV_VIDEO_CodingAV1)
	{
		//the only AV1 control 01_video_encode always sets (default true)
		ret = ctx->enc->setAV1ErrResilienceMode(true);
		ENC_CHECK(ret < 0, "Could not set AV1 error resilience mode");
	}
#endif

	ret = ctx->enc->setFrameRate(ctx->fps_n, ctx->fps_d);
	ENC_CHECK(ret < 0, "Could not set framerate");
	
	//ret = ctx->enc->output_plane.setupPlane(V4L2_MEMORY_USERPTR, ctx->packets_num, false, true);
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_MMAP)
	ret = ctx->enc->output_plane.setupPlane(V4L2_MEMORY_MMAP, ctx->packets_num, true, false);
#else
	ret = setup_output_dmabuf(ctx,ctx->packets_num); //V4L2_MEMORY_DMABUF
#endif
	ENC_CHECK(ret < 0, "Could not setup output plane");

	ret = ctx->enc->capture_plane.setupPlane(V4L2_MEMORY_MMAP, ctx->packets_num, true, false);
	ENC_CHECK(ret < 0, "Could not setup capture plane");

	ret = ctx->enc->subscribeEvent(V4L2_EVENT_EOS,0,0);
	ENC_CHECK(ret < 0, "Could not subscribe EOS event");

	ret = ctx->enc->output_plane.setStreamStatus(true);
	ENC_CHECK(ret < 0, "Error in output plane streamon");

	ret = ctx->enc->capture_plane.setStreamStatus(true);
	ENC_CHECK(ret < 0, "Error in capture plane streamon");

	if(ctx->blocking_mode)
	{
		ctx->enc->capture_plane.setDQThreadCallback(encoder_capture_plane_dq_callback);
		ctx->dqThreadRunning = true;
		ctx->enc->capture_plane.startDQThread(ctx);
	}
    else
    {
		/*
        sem_init(&ctx->pollthread_sema, 0, 0);
        sem_init(&ctx->encoderthread_sema, 0, 0);
        // Set encoder poll thread for non-blocking io mode
        pthread_create(&ctx->enc_pollthread, NULL, encoder_pollthread_fcn, ctx);
        pthread_setname_np(ctx->enc_pollthread, "EncPollThread");
        cerr << "Created the PollThread and Encoder Thread \n";
        */
    }

	// Enqueue all the empty capture plane buffers
	for (uint32_t i = 0; i < ctx->enc->capture_plane.getNumBuffers(); i++){
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];
		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;

		ret = ctx->enc->capture_plane.qBuffer(v4l2_buf, NULL);
		ENC_CHECK(ret < 0, "Error while queueing buffer at capture plane");

	}

	return ctx;
}

int copyFrameToNvBuf(nvFrame* frame, NvBuffer& buffer)
{
	uint32_t i, j;
	char *dataDst;
	char *dataSrc;
	for (i = 0; i < buffer.n_planes; i++)
	{
		NvBuffer::NvBufferPlane &plane = buffer.planes[i];
		unsigned int &frameLineSize = frame->linesize[i];
		size_t copySz = plane.fmt.bytesperpixel * plane.fmt.width;
		dataDst = (char *) plane.data;
		dataSrc = (char *) frame->payload[i];
		plane.bytesused = 0;
		for (j = 0; j < plane.fmt.height; j++)
		{
			memcpy(dataDst, dataSrc, copySz);
			dataDst += plane.fmt.stride;
			dataSrc += frameLineSize;
		}
		plane.bytesused = plane.fmt.stride * plane.fmt.height;
	}
	return 0;
}

int nvmpi_encoder_put_frame(nvmpictx* ctx,nvFrame* frame)
{
	if(ctx->flushing) return -2;
	
	int ret;
	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[MAX_PLANES];
	NvBuffer *nvBuffer;

	memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	memset(planes, 0, sizeof(planes));

	v4l2_buf.m.planes = planes;

	if(ctx->enc->isInError())
		return -1;

	if(ctx->index < ctx->enc->output_plane.getNumBuffers())
	{
		nvBuffer=ctx->enc->output_plane.getNthBuffer(ctx->index);
		v4l2_buf.index = ctx->index;
		ctx->index++;
		
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)
		v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		v4l2_buf.memory = V4L2_MEMORY_DMABUF;
		// Map output plane buffer for memory type DMABUF.
		ret = ctx->enc->output_plane.mapOutputBuffers(v4l2_buf, ctx->output_plane_fd[v4l2_buf.index]);
		if (ret < 0)
		{
			cerr << "Error while mapping buffer at output plane" << endl;
		}
#endif
	}
	else
	{
		/*TODO move it to another thread or make this call non-blocking.
		 * DQBuffer could take considerable time. e.g. when encoder performance is 20fps and all output_plane is busy
		 * it could take up to 1000/20=50ms... latency
		 */
		ret = ctx->enc->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
		if (ret < 0)
		{
			cerr << "Error DQing buffer at output plane" << std::endl;
			return -1;
		}
	}
	
	if(frame)
	{
		copyFrameToNvBuf(frame, *nvBuffer);
		v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
		v4l2_buf.timestamp.tv_usec = frame->timestamp % 1000000;
		v4l2_buf.timestamp.tv_sec = frame->timestamp / 1000000;
	}
	else
	{
		//send EOS and flush
		ctx->flushing = true;
		v4l2_buf.m.planes[0].m.userptr = 0;
		v4l2_buf.m.planes[0].bytesused = v4l2_buf.m.planes[1].bytesused = v4l2_buf.m.planes[2].bytesused = 0;
	}

	//needed for V4L2_MEMORY_MMAP and V4L2_MEMORY_DMABUF
	for (uint32_t j = 0 ; j < nvBuffer->n_planes; j++)
	{
#ifdef WITH_NVUTILS
		NvBufSurface *nvbuf_surf = 0;
		ret = NvBufSurfaceFromFd (nvBuffer->planes[j].fd, (void**)(&nvbuf_surf));
		if (ret < 0)
		{
			cerr << "Error while NvBufSurfaceFromFd" << endl;
		}
		ret = NvBufSurfaceSyncForDevice (nvbuf_surf, 0, j);
		if (ret < 0)
		{
			cerr << "Error while NvBufSurfaceSyncForDevice at output plane for V4L2_MEMORY_DMABUF" << endl;
		}
#else
		ret = NvBufferMemSyncForDevice (nvBuffer->planes[j].fd, j, (void **)&nvBuffer->planes[j].data);
		if (ret < 0)
		{
			cerr << "Error while NvBufferMemSyncForDevice at output plane for V4L2_MEMORY_DMABUF" << endl;
		}
#endif
	}
	
	//for V4L2_MEMORY_DMABUF only
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)
	for (uint32_t j = 0; j < nvBuffer->n_planes; j++)
	{
		v4l2_buf.m.planes[j].bytesused = nvBuffer->planes[j].bytesused;
	}
#endif

	ret = ctx->enc->output_plane.qBuffer(v4l2_buf, NULL);
	TEST_ERROR(ret < 0, "Error while queueing buffer at output plane", ret);

	return 0;
}

int nvmpi_encoder_dqEmptyPacket(nvmpictx* ctx,nvPacket** packet)
{
	nvPacket* pkt = ctx->pktPool->dqEmptyBuf();
	if(!pkt) return -1;
	*packet = pkt;
	return 0;
}

void nvmpi_encoder_qEmptyPacket(nvmpictx* ctx,nvPacket* packet)
{
	ctx->pktPool->qEmptyBuf(packet);
	return;
}

int nvmpi_encoder_get_packet(nvmpictx* ctx,nvPacket** packet)
{
	nvPacket* pkt = ctx->pktPool->dqFilledBuf();
	
	if(!pkt)
	{
		if(!ctx->flushing) return -1;
		bool wait = true;
		while(wait)
		{
			pkt = ctx->pktPool->dqFilledBuf();
			//also stop waiting if the DQ thread died without delivering EOS
			//(error paths), otherwise this would spin forever
			if(pkt || ctx->capPlaneGotEOS || !ctx->dqThreadRunning) wait = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if(!pkt) return -2; //if got eos
	}
	
	*packet = pkt;
	return 0;
}

int nvmpi_encoder_close(nvmpictx* ctx)
{
	if(!ctx) return -1;
	if(ctx->blocking_mode)
	{
		//release a DQ callback blocked in the backpressure wait; stopDQThread
		//itself is a no-op in blocking mode (the thread only exits when the
		//callback returns false)
		ctx->enc_shutdown = true;
		ctx->enc->capture_plane.stopDQThread();
		if(ctx->enc->capture_plane.waitForDQThread(1000) < 0)
			cerr << "[libnvmpi][W]: encoder DQ thread did not stop within 1s" << endl;
	}
	else
	{
		//sem_destroy(&ctx.pollthread_sema);
		//sem_destroy(&ctx.encoderthread_sema);
	}
	
#if (OUTPLANE_MEMTYPE == OUTPLANE_MEMTYPE_DMA)	
	int ret;
    if(ctx->enc)
    {
        for (uint32_t i = 0; i < ctx->enc->output_plane.getNumBuffers(); i++)
        {
            // Unmap output plane buffer for memory type DMABUF.
            ret = ctx->enc->output_plane.unmapOutputBuffers(i, ctx->output_plane_fd[i]);
            if (ret < 0)
            {
                cerr << "Error while unmapping buffer at output plane" << endl;
            }

            ret = NvBufSurf::NvDestroy(ctx->output_plane_fd[i]);
            ctx->output_plane_fd[i] = -1;
            if(ret < 0)
            {
                cerr << "Failed to Destroy NvBuffer\n" << endl;
                return ret;
            }
        }
    }
    delete[] ctx->output_plane_fd;
    #endif
	
	delete ctx->enc;
	delete ctx->pktPool;
	delete ctx;
	return 0;
}


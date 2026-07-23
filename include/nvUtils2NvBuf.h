#pragma once
#if defined(WITH_NVUTILS)
#include "nvbufsurface.h"
#include "nvbufsurftransform.h"
#include "NvBufSurface.h"
#define MAX_NUM_PLANES NVBUF_MAX_PLANES
#define NvBufferDestroy NvBufSurf::NvDestroy
#define NvBufferCreateParams NvBufSurf::NvCommonAllocateParams
#define NvBufferColorFormat_NV12 NVBUF_COLOR_FORMAT_NV12
#define NvBufferColorFormat_NV12_ER NVBUF_COLOR_FORMAT_NV12_ER
#define NvBufferColorFormat_NV12_709 NVBUF_COLOR_FORMAT_NV12_709
#define NvBufferColorFormat_NV12_709_ER NVBUF_COLOR_FORMAT_NV12_709_ER
#define NvBufferColorFormat_NV12_2020 NVBUF_COLOR_FORMAT_NV12_2020
#define NvBufferColorFormat_YUV420 NVBUF_COLOR_FORMAT_YUV420
#define NvBufferColorFormat_NV12_10LE NVBUF_COLOR_FORMAT_NV12_10LE
//#define NvBufferColorFormat_ABGR32 NVBUF_COLOR_FORMAT_BGRA
//NVBUF_COLOR_FORMAT_BGR - BGR-8-8-8 single plane. /nvbufsurface API only/
//NVBUF_COLOR_FORMAT_B8_G8_R8 -  BGR- unsigned 8-bit multiplanar plane. /nvbufsurface API only/
#define NvBufferLayout_Pitch NVBUF_LAYOUT_PITCH
#define NvBufferLayout_BlockLinear NVBUF_LAYOUT_BLOCK_LINEAR
#define NvBufferTransformParams NvBufSurfTransformParams
#define NvBufferRect NvBufSurfTransformRect
#define NVBUFFER_TRANSFORM_FILTER NVBUFSURF_TRANSFORM_FILTER
#define NVBUFFER_TRANSFORM_CROP_SRC NVBUFSURF_TRANSFORM_CROP_SRC
#define NVBUFFER_TRANSFORM_FLIP NVBUFSURF_TRANSFORM_FLIP
#define NvBufferTransform_Flip NvBufSurfTransform_Flip
#define NvBufferTransform_None NvBufSurfTransform_None
#define NvBufferTransform_Rotate90 NvBufSurfTransform_Rotate90
#define NvBufferTransform_Rotate180 NvBufSurfTransform_Rotate180
#define NvBufferTransform_Rotate270 NvBufSurfTransform_Rotate270
#define NvBufferTransform_FlipX NvBufSurfTransform_FlipX
#define NvBufferTransform_FlipY NvBufSurfTransform_FlipY
#define NvBufferTransform_Filter_Smart NvBufSurfTransformInter_Algo3
#define NvBufferTransform_Filter_Nearest NvBufSurfTransformInter_Nearest
#define NvBufferParams NvBufSurfTransform
#define NvBufferColorFormat NvBufSurfaceColorFormat

//#define NvBuffer2Raw(dmabuf, plane, out_width, out_height, ptr)
#else
#include "nvbuf_utils.h"
#endif

#include "nvmpi.h"

//nvTransform -> VIC flip enum. nvbufsurftransform.h documents
//NvBufSurfTransform_Rotate* as clockwise but the VIC actually rotates
//counter-clockwise (verified on device, matching the 07_video_convert sample
//help), hence the 90/270 swap. Likewise FlipX mirrors left<->right despite
//the "X-axis" name. If a JetPack inverts either, swap the pairs back here.
static inline NvBufferTransform_Flip nvmpi_map_transform(unsigned int t)
{
	switch(t)
	{
		case NV_TRANSFORM_ROTATE90:  return NvBufferTransform_Rotate270;
		case NV_TRANSFORM_ROTATE180: return NvBufferTransform_Rotate180;
		case NV_TRANSFORM_ROTATE270: return NvBufferTransform_Rotate90;
		case NV_TRANSFORM_FLIP_H:    return NvBufferTransform_FlipX;
		case NV_TRANSFORM_FLIP_V:    return NvBufferTransform_FlipY;
		default:                     return NvBufferTransform_None;
	}
}

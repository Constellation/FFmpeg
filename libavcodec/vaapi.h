/*
 * Video Acceleration API (shared data between FFmpeg and the video player)
 * HW decode acceleration for MPEG-2, MPEG-4, H.264 and VC-1
 *
 * Copyright (C) 2008-2009 Splitted-Desktop Systems
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_VAAPI_H
#define AVCODEC_VAAPI_H

/**
 * @file
 * @ingroup lavc_codec_hwaccel_vaapi
 * Public libavcodec VA API header.
 */

#include <stdint.h>

#include <va/va.h>

#include "avcodec.h"

/**
 * @defgroup lavc_codec_hwaccel_vaapi VA API Decoding
 * @ingroup lavc_codec_hwaccel
 * @{
 */

#if FF_API_VAAPI_CONTEXT
/**
 * @deprecated use AVVAAPIContext instead
 */
struct vaapi_context {
    /**
     * Window system dependent data
     *
     * - encoding: unused
     * - decoding: Set by user
     */
    void *display;

    /**
     * Configuration ID
     *
     * - encoding: unused
     * - decoding: Set by user
     */
    uint32_t config_id;

    /**
     * Context ID (video decode pipeline)
     *
     * - encoding: unused
     * - decoding: Set by user
     */
    uint32_t context_id;

    /**
     * VAPictureParameterBuffer ID
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    uint32_t pic_param_buf_id;

    /**
     * VAIQMatrixBuffer ID
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    uint32_t iq_matrix_buf_id;

    /**
     * VABitPlaneBuffer ID (for VC-1 decoding)
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    uint32_t bitplane_buf_id;

    /**
     * Slice parameter/data buffer IDs
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    uint32_t *slice_buf_ids;

    /**
     * Number of effective slice buffer IDs to send to the HW
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    unsigned int n_slice_buf_ids;

    /**
     * Size of pre-allocated slice_buf_ids
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    unsigned int slice_buf_ids_alloc;

    /**
     * Pointer to VASliceParameterBuffers
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    void *slice_params;

    /**
     * Size of a VASliceParameterBuffer element
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    unsigned int slice_param_size;

    /**
     * Size of pre-allocated slice_params
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    unsigned int slice_params_alloc;

    /**
     * Number of slices currently filled in
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    unsigned int slice_count;

    /**
     * Pointer to slice data buffer base
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    const uint8_t *slice_data;

    /**
     * Current size of slice data
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    uint32_t slice_data_size;
} attribute_deprecated;
#endif

/**
 * This structure is used to share data between the FFmpeg library and
 * the client video application.
 * This shall be zero-allocated and available as
 * AVCodecContext.hwaccel_context. All user members can be set once
 * during initialization or through each AVCodecContext.get_buffer()
 * function call. In any case, they must be valid prior to calling
 * decoding functions.
 *
 * The size of this structure is not a part of the public ABI and must not
 * be used outside of libavcodec. Use av_vaapi_alloc_context() to allocate an
 * AVVAAPIContext.
 */
typedef struct AVVAAPIContext {
    /**
     * Window system dependent data
     *
     * - encoding: unused
     * - decoding: Set by user
     */
    void *display;

    /**
     * Configuration ID
     *
     * - encoding: unused
     * - decoding: Set by user
     */
    uint32_t config_id;

    /**
     * Context ID (video decode pipeline)
     *
     * - encoding: unused
     * - decoding: Set by user
     */
    uint32_t context_id;

    /**
     * VAPictureParameterBuffer ID
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    uint32_t pic_param_buf_id;

    /**
     * VAIQMatrixBuffer ID
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    uint32_t iq_matrix_buf_id;

    /**
     * VABitPlaneBuffer ID (for VC-1 decoding)
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    uint32_t bitplane_buf_id;

    /**
     * Slice parameter/data buffer IDs
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    uint32_t *slice_buf_ids;

    /**
     * Number of effective slice buffer IDs to send to the HW
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    unsigned int n_slice_buf_ids;

    /**
     * Size of pre-allocated slice_buf_ids
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    unsigned int slice_buf_ids_alloc;

    /**
     * Pointer to VASliceParameterBuffers
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    void *slice_params;

    /**
     * Size of a VASliceParameterBuffer element
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    unsigned int slice_param_size;

    /**
     * Size of pre-allocated slice_params
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    unsigned int slice_params_alloc;

    /**
     * Number of slices currently filled in
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    unsigned int slice_count;

    /**
     * Pointer to slice data buffer base
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    const uint8_t *slice_data;

    /**
     * Current size of slice data
     *
     * - encoding: unused
     * - decoding: Set by libavcodec
     */
    uint32_t slice_data_size;
} AVVAAPIContext;

/**
 * Allocate an AVVAAPIContext.
 *
 * @return Newly-allocated AVVAAPIContext or NULL on failure.
 */
AVVAAPIContext *av_vaapi_alloc_context(void);

/**
 * Get a decoder profile that should be used for initializing a VAAPI config.
 * Should be called from the AVCodecContext.get_format() callback.
 *
 * @param avctx the codec context being used for decoding the stream
 * @param profile a pointer into which the result will be written on success.
 *                The contents of profile are undefined if this function returns
 *                an error.
 *
 * @return 0 on success (non-negative), a negative AVERROR on failure.
 */
int av_vaapi_get_profile(AVCodecContext *avctx, VAProfile *profile);

/* @} */

#endif /* AVCODEC_VAAPI_H */

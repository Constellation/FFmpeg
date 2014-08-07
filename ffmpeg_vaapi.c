/*
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"

#include <stdint.h>

#include <va/va.h>

#if HAVE_VAAPI_DRM
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <sys/stat.h>
#include <va/va_drm.h>
#endif

#if HAVE_VAAPI_X11
#include <X11/Xlib.h>
#include <va/va_x11.h>
#endif

#include "libavcodec/avcodec.h"
#include "libavcodec/vaapi.h"

#include "libavutil/avassert.h"
#include "libavutil/buffer.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"

#include "ffmpeg.h"

/* Stuff associated with one VAAPI config/decoder. It is refcounted, one ref is
 * held by each used frame + one by the VAAPIContext as long as this config is
 * being used for decoding */
typedef struct VAAPIConfig {
    AVBufferRef *ctx_buf;
    struct VAAPIContext *ctx;

    VAConfigID  config_id;
    VAContextID context_id;

    VASurfaceID *surfaces;
    uint8_t *surface_used;
    int nb_surfaces;
} VAAPIConfig;

/* Global stuff associated with a VAAPI display device + some temp variables. It
 * is refcounted, one reference is held by each config, one by each VAAPIImage +
 * one by itself (released on uninit). */
typedef struct VAAPIContext {
    AVBufferRef *self;
#if HAVE_VAAPI_DRM
    int drm_fd;
#endif
#if HAVE_VAAPI_X11
    Display *dpy;
#endif

    VADisplay display;

    VAAPIConfig *cur_config;
    AVBufferRef *cur_config_buf;

    VAImageFormat      img_fmt;
    enum AVPixelFormat pix_fmt;
    AVFrame *tmp_frame;
} VAAPIContext;

/* A wrapper around a VAAPI surface */
typedef struct VAAPIFrame {
    VADisplay display;
    AVBufferRef *config;
    uint8_t *used;
} VAAPIFrame;

/* A wrapper around a VAImage for retrieving the data */
typedef struct VAAPIImage {
    VAAPIContext *ctx;
    AVBufferRef  *ctx_buf;

    VAImage image;
} VAAPIImage;

static const int vaapi_formats[][2] = {
    { VA_FOURCC_YV12, AV_PIX_FMT_YUV420P },
    { VA_FOURCC_NV12, AV_PIX_FMT_NV12    },
};

static void vaapi_free_image(void *opaque, uint8_t *data)
{
    VAAPIImage *img = opaque;

    if (img->image.buf != VA_INVALID_ID)
        vaUnmapBuffer(img->ctx->display, img->image.buf);
    if (img->image.image_id != VA_INVALID_ID)
        vaDestroyImage(img->ctx->display, img->image.image_id);
    av_buffer_unref(&img->ctx_buf);
    av_freep(&img);
}

static int vaapi_retrieve_data(AVCodecContext *s, AVFrame *frame)
{
    VASurfaceID surface = (VASurfaceID)(uintptr_t)frame->data[3];
    InputStream    *ist = s->opaque;
    VAAPIContext   *ctx = ist->hwaccel_ctx;
    VAAPIImage *img;
    VAStatus err;
    uint8_t *data;
    int i, ret;

    img = av_mallocz(sizeof(*img));
    if (!img)
        return AVERROR(ENOMEM);

    img->image.buf      = VA_INVALID_ID;
    img->image.image_id = VA_INVALID_ID;

    img->ctx_buf = av_buffer_ref(ctx->self);
    if (!img->ctx_buf) {
        av_freep(&img);
        return AVERROR(ENOMEM);
    }
    img->ctx = ctx;

    err = vaCreateImage(ctx->display, &ctx->img_fmt,
                        frame->width, frame->height, &img->image);
    if (err != VA_STATUS_SUCCESS) {
        vaapi_free_image(img, NULL);
        av_log(NULL, AV_LOG_ERROR, "Error creating an image: %s\n",
               vaErrorStr(err));
        return AVERROR_UNKNOWN;
    }

    /* We do not use vaDeriveImage, because even when it is implemented, the
     * access to the image data is usually very slow */
    err = vaGetImage(ctx->display, surface, 0, 0, frame->width, frame->height,
                     img->image.image_id);
    if (err != VA_STATUS_SUCCESS) {
        vaapi_free_image(img, NULL);
        av_log(NULL, AV_LOG_ERROR, "Error getting an image: %s\n",
               vaErrorStr(err));
        return AVERROR_UNKNOWN;
    }

    err = vaMapBuffer(ctx->display, img->image.buf, (void**)&data);
    if (err != VA_STATUS_SUCCESS) {
        vaapi_free_image(img, NULL);
        av_log(NULL, AV_LOG_ERROR, "Error mapping the image buffer: %s\n",
               vaErrorStr(err));
        return AVERROR_UNKNOWN;
    }

    ctx->tmp_frame->buf[0] = av_buffer_create(data, img->image.data_size,
                                              vaapi_free_image, img, 0);
    if (!ctx->tmp_frame->buf[0]) {
        vaapi_free_image(img, NULL);
        return AVERROR(ENOMEM);
    }

    for (i = 0; i < img->image.num_planes; i++) {
        ctx->tmp_frame->data[i]     = data + img->image.offsets[i];
        ctx->tmp_frame->linesize[i] = img->image.pitches[i];
    }
    ctx->tmp_frame->format = ctx->pix_fmt;
    ctx->tmp_frame->width  = frame->width;
    ctx->tmp_frame->height = frame->height;

    ret = av_frame_copy_props(ctx->tmp_frame, frame);
    if (ret < 0) {
        av_frame_unref(ctx->tmp_frame);
        return ret;
    }

    av_frame_unref(frame);
    av_frame_move_ref(frame, ctx->tmp_frame);

    /* YV12 and YUV420P are essentially the same, but U and V are reversed.
     * To convert YV12 to YUV420P, swap U and V data */
    if (frame->format == AV_PIX_FMT_YUV420P)
        FFSWAP(uint8_t*, frame->data[1], frame->data[2]);

    return ret;
}

static void vaapi_release_buffer(void *opaque, uint8_t *data)
{
    VAAPIFrame *priv = opaque;

    *priv->used = 0;
    av_buffer_unref(&priv->config);
    av_freep(&priv);
}

static int vaapi_get_buffer(AVCodecContext *s, AVFrame *frame, int flags)
{
    InputStream     *ist = s->opaque;
    VAAPIContext    *ctx = ist->hwaccel_ctx;
    VAAPIConfig  *config = ctx->cur_config;
    VAAPIFrame     *priv = NULL;
    int i;

    av_assert0(frame->format == AV_PIX_FMT_VAAPI_VLD);

    for (i = 0; i < config->nb_surfaces; i++)
        if (!config->surface_used[i])
            break;
    if (i == config->nb_surfaces) {
        av_log(NULL, AV_LOG_ERROR, "No free surfaces left.\n");
        return AVERROR(ENOMEM);
    }

    priv = av_mallocz(sizeof(*priv));
    if (!priv)
        return AVERROR(ENOMEM);

    priv->used           = &config->surface_used[i];
    priv->display        = ctx->display;
    priv->config         = av_buffer_ref(ctx->cur_config_buf);
    if (!priv->config)
        goto fail;

    frame->buf[0] = av_buffer_create((uint8_t*)&config->surfaces[i],
                                     sizeof(*config->surfaces),
                                     vaapi_release_buffer, priv,
                                     AV_BUFFER_FLAG_READONLY);
    if (!frame->buf[0])
        return AVERROR(ENOMEM);

    frame->opaque  = priv;
    frame->data[3] = (uint8_t*)(uintptr_t)config->surfaces[i];
    config->surface_used[i] = 1;

    return 0;
fail:
    if (priv)
        av_buffer_unref(&priv->config);
    av_freep(&priv);
    return AVERROR(ENOMEM);
}

static void vaapi_free_config(void *opaque, uint8_t *data)
{
    VAAPIConfig *config = opaque;

    vaDestroySurfaces(config->ctx->display, config->surfaces, config->nb_surfaces);
    av_freep(&config->surfaces);
    av_freep(&config->surface_used);

    if (config->context_id != VA_INVALID_ID)
        vaDestroyContext(config->ctx->display, config->context_id);

    if (config->config_id != VA_INVALID_ID)
        vaDestroyConfig(config->ctx->display, config->config_id);

    av_buffer_unref(&config->ctx_buf);
    av_freep(&config);
}

static int vaapi_create_config(AVCodecContext *s)
{
    InputStream  *ist = s->opaque;
    VAAPIContext *ctx = ist->hwaccel_ctx;
    int loglevel = (ist->hwaccel_id == HWACCEL_AUTO) ? AV_LOG_VERBOSE : AV_LOG_ERROR;
    VAAPIConfig *config;
    VAProfile profile;
    VAStatus err;
    int ret;

    ret = av_vaapi_get_profile(s, &profile);
    if (ret < 0) {
        av_log(NULL, loglevel, "No known VAAPI decoder profile for input "
               "stream #%d:%d.\n", ist->file_index, ist->st->index);
        return AVERROR(EINVAL);
    }

    /* setup a refcounted buffer for the current config */
    config = av_mallocz(sizeof(*config));
    if (!config)
        return AVERROR(ENOMEM);
    ctx->cur_config_buf = av_buffer_create((uint8_t*)config,
                                           sizeof(*config),
                                           vaapi_free_config, config,
                                           AV_BUFFER_FLAG_READONLY);
    if (!ctx->cur_config_buf) {
        av_freep(&config);
        return AVERROR(ENOMEM);
    }

    config->ctx_buf    = av_buffer_ref(ctx->self);
    if (!config->ctx_buf)
        goto fail;
    config->ctx        = ctx;
    config->config_id  = VA_INVALID_ID;
    config->context_id = VA_INVALID_ID;

    /* create the surfaces */
    config->nb_surfaces = 16;
    if (s->active_thread_type == FF_THREAD_FRAME)
        config->nb_surfaces += s->thread_count;
    config->surfaces     = av_malloc_array (config->nb_surfaces, sizeof(*config->surfaces));
    config->surface_used = av_mallocz_array(config->nb_surfaces, sizeof(*config->surface_used));
    if (!config->surfaces || !config->surface_used) {
        av_freep(&config->surfaces);
        av_freep(&config->surface_used);
        goto fail;
    }

    err = vaCreateSurfaces(ctx->display, VA_RT_FORMAT_YUV420,
                           s->coded_width, s->coded_height,
                           config->surfaces, config->nb_surfaces, NULL, 0);
    if (err != VA_STATUS_SUCCESS) {
        config->surfaces[0] = VA_INVALID_ID;
        av_log(NULL, loglevel, "Error creating surfaces: %s\n",
               vaErrorStr(err));
        goto fail;
    }

    /* create the decoder configuration */
    err = vaCreateConfig(ctx->display, profile, VAEntrypointVLD,
                         NULL, 0, &config->config_id);
    if (err != VA_STATUS_SUCCESS) {
        av_log(NULL, loglevel, "Error creating configuration: %s\n",
               vaErrorStr(err));
        goto fail;
    }

    /* create the decoder context */
    err = vaCreateContext(ctx->display, config->config_id,
                          s->coded_width, s->coded_height, 0,
                          config->surfaces, config->nb_surfaces,
                          &config->context_id);
    if (err != VA_STATUS_SUCCESS) {
        av_log(NULL, loglevel, "Error creating the decoding context: %s\n",
               vaErrorStr(err));
        goto fail;
    }

    ctx->cur_config = config;

    return 0;
fail:
    av_buffer_unref(&ctx->cur_config_buf);
    return AVERROR_UNKNOWN;
}

static void vaapi_uninit(AVCodecContext *s)
{
    InputStream  *ist = s->opaque;
    VAAPIContext *ctx = ist->hwaccel_ctx;

    av_buffer_unref(&ctx->cur_config_buf);
    ctx->cur_config = NULL;

    av_freep(&s->hwaccel_context);
    ist->hwaccel_ctx = NULL;

    av_buffer_unref(&ctx->self);
}

static int pick_format(InputStream *ist, VAAPIContext *ctx)
{
    int loglevel = (ist->hwaccel_id == HWACCEL_AUTO) ? AV_LOG_VERBOSE : AV_LOG_ERROR;
    VAImageFormat *formats = NULL;
    VAStatus err;
    int nb_formats, i, j;

    nb_formats = vaMaxNumImageFormats(ctx->display);
    if (!nb_formats) {
        av_log(NULL, loglevel, "No image formats supported.\n");
        return AVERROR(EINVAL);
    }

    formats = av_mallocz_array(nb_formats, sizeof(*formats));
    if (!formats)
        return AVERROR(ENOMEM);

    err = vaQueryImageFormats(ctx->display, formats, &nb_formats);
    if (err != VA_STATUS_SUCCESS) {
        av_log(NULL, loglevel, "Error querying image formats: %s\n",
               vaErrorStr(err));
        goto fail;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(vaapi_formats); i++)
        for (j = 0; j < nb_formats; j++)
            if (vaapi_formats[i][0] == formats[j].fourcc) {
                ctx->img_fmt = formats[j];
                ctx->pix_fmt = vaapi_formats[i][1];
                av_freep(&formats);
                return 0;
            }

fail:
    av_freep(&formats);
    return AVERROR(EINVAL);
}

#if HAVE_VAAPI_DRM
static void vaapi_open_drm(InputStream *ist, VAAPIContext *ctx)
{
    if (!ist->hwaccel_device)
        return;

    ctx->drm_fd = drmOpen(ist->hwaccel_device, NULL);
    if (ctx->drm_fd == -1)
        ctx->drm_fd = open(ist->hwaccel_device, O_RDONLY);

    if (ctx->drm_fd == -1) {
        av_log(NULL, AV_LOG_VERBOSE, "Cannot open DRM device %s: %s.\n",
               ist->hwaccel_device, strerror(errno));
        return;
    }

    ctx->display = vaGetDisplayDRM(ctx->drm_fd);
    if (!ctx->display) {
        av_log(NULL, AV_LOG_VERBOSE, "Error getting a DRM VAAPI display.\n");
        drmClose(ctx->drm_fd);
        return;
    }

    av_log(NULL, AV_LOG_VERBOSE, "Successfully opened a VAAPI display on DRM "
           "device %s.\n", ist->hwaccel_device);
}
#endif

#if HAVE_VAAPI_X11
static void vaapi_open_x11(InputStream *ist, VAAPIContext *ctx)
{
    const char *display;

    ctx->dpy = XOpenDisplay(ist->hwaccel_device);
    if (!ctx->dpy) {
        av_log(NULL, AV_LOG_VERBOSE, "Cannot open the X11 display %s.\n",
               XDisplayName(ist->hwaccel_device));
        return;
    }
    display = XDisplayString(ctx->dpy);

    ctx->display = vaGetDisplay(ctx->dpy);
    if (!ctx->display) {
        av_log(NULL, AV_LOG_VERBOSE, "Error getting an X11 VAAPI display.\n");
        XCloseDisplay(ctx->dpy);
        return;
    }

    av_log(NULL, AV_LOG_VERBOSE, "Successfully opened a VAAPI display on X11 "
           "display %s.\n", display);
}
#endif

static void vaapi_free_context(void *opaque, uint8_t *data)
{
    VAAPIContext *ctx = opaque;

#if HAVE_VAAPI_DRM
    close(ctx->drm_fd);
#endif

#if HAVE_VAAPI_X11
    if (ctx->dpy)
        XCloseDisplay(ctx->dpy);
#endif

    if (ctx->display)
        vaTerminate(ctx->display);


    av_frame_free(&ctx->tmp_frame);
    av_freep(&ctx);
}

static int vaapi_create_context(AVCodecContext *s)
{
    InputStream  *ist = s->opaque;
    int loglevel = (ist->hwaccel_id == HWACCEL_AUTO) ? AV_LOG_VERBOSE : AV_LOG_ERROR;
    AVVAAPIContext *vaapi_ctx;
    VAAPIContext *ctx;
    VAStatus err;
    int ret, ver_minor, ver_major;

    ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return AVERROR(ENOMEM);

    ctx->self = av_buffer_create((uint8_t*)ctx, sizeof(*ctx),
                                 vaapi_free_context, ctx,
                                 AV_BUFFER_FLAG_READONLY);
    if (!ctx->self) {
        av_freep(&ctx);
        return AVERROR(ENOMEM);
    }

    ctx->tmp_frame = av_frame_alloc();
    if (!ctx->tmp_frame)
        goto fail;

#if HAVE_VAAPI_DRM
    vaapi_open_drm(ist, ctx);
#endif
#if HAVE_VAAPI_X11
    if (!ctx->display)
        vaapi_open_x11(ist, ctx);
#endif

    if (!ctx->display) {
        av_log(NULL, loglevel, "Could not open a VAAPI display.\n");
        goto fail;
    }

    err = vaInitialize(ctx->display, &ver_major, &ver_minor);
    if (err != VA_STATUS_SUCCESS) {
        av_log(NULL, loglevel, "Error initializing VAAPI: %s\n",
               vaErrorStr(err));
        goto fail;
    }

    ret = pick_format(ist, ctx);
    if (ret < 0) {
        av_log(NULL, loglevel, "No supported image format found.\n");
        goto fail;
    }

    vaapi_ctx = av_vaapi_alloc_context();
    if (!vaapi_ctx)
        goto fail;
    vaapi_ctx->display = ctx->display;

    s->hwaccel_context = vaapi_ctx;

    ist->hwaccel_ctx           = ctx;
    ist->hwaccel_uninit        = vaapi_uninit;

    av_log(NULL, AV_LOG_VERBOSE, "Using VAAPI version %d.%d -- %s -- "
           "to decode input stream #%d:%d.\n", ver_major, ver_minor,
           vaQueryVendorString(ctx->display), ist->file_index, ist->st->index);

    return 0;

fail:
    av_log(NULL, loglevel, "VAAPI init failed for stream #%d:%d.\n",
           ist->file_index, ist->st->index);
    vaapi_uninit(s);
    return AVERROR(EINVAL);
}

int vaapi_init(AVCodecContext *s)
{
    InputStream *ist = s->opaque;
    int loglevel = (ist->hwaccel_id == HWACCEL_AUTO) ? AV_LOG_VERBOSE : AV_LOG_ERROR;
    AVVAAPIContext *vaapi_ctx;
    VAAPIContext *ctx;
    int ret;

    if (!ist->hwaccel_ctx) {
        ret = vaapi_create_context(s);
        if (ret < 0)
            return ret;
    }
    ctx       = ist->hwaccel_ctx;
    vaapi_ctx = s->hwaccel_context;

    av_buffer_unref(&ctx->cur_config_buf);
    ctx->cur_config = NULL;

    ret = vaapi_create_config(s);
    if (ret < 0) {
        av_log(NULL, loglevel, "Error initializing a VAAPI configuration.\n");
        return AVERROR_UNKNOWN;
    }

    vaapi_ctx->config_id  = ctx->cur_config->config_id;
    vaapi_ctx->context_id = ctx->cur_config->context_id;

    ist->hwaccel_get_buffer    = vaapi_get_buffer;
    ist->hwaccel_retrieve_data = vaapi_retrieve_data;

    return 0;
}

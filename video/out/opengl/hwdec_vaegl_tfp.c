/*
 * This file is part of mpv.
 *
 * Parts based on the MPlayer VA-API patch (see vo_vaapi.c).
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <string.h>
#include <assert.h>

#include <EGL/egl.h>

#include "video/out/x11_common.h"
#include "hwdec.h"
#include "video/vaapi.h"

struct priv {
    struct mp_log *log;
    struct mp_vaapi_ctx *ctx;
    VADisplay *display;
    Display *xdisplay;
    GLuint gl_texture;
    Pixmap pixmap;
    EGLImageKHR images[4];
};

static void destroy_texture(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;

    for (int n = 0; n < 4; n++) {
        if (p->images[n])
            hw->gl->DestroyImageKHR(eglGetCurrentDisplay(), p->images[n]);
        p->images[n] = 0;
    }

    if (p->pixmap)
        XFreePixmap(p->xdisplay, p->pixmap);
    p->pixmap = 0;

    gl->DeleteTextures(1, &p->gl_texture);
    p->gl_texture = 0;
}

static void destroy(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;
    destroy_texture(hw);
    va_destroy(p->ctx);
}

static int create(struct gl_hwdec *hw)
{
    GL *gl = hw->gl;
    if (hw->hwctx)
        return -1;
    if (!eglGetCurrentDisplay()) {
            printf("eglGetCurrentDisplay error\n");
        return -1;
    }


    Display *x11disp =
        hw->gl->MPGetNativeDisplay ? hw->gl->MPGetNativeDisplay("x11") : NULL;
    if (!x11disp)
        return -1;
    if (!gl->CreateImageKHR || !gl->EGLImageTargetTexture2DOES)
        return -1;

    struct priv *p = talloc_zero(hw, struct priv);
    hw->priv = p;
    p->log = hw->log;
    p->xdisplay = x11disp;
    p->display = vaGetDisplay(x11disp);
    if (!p->display)
        return -1;
    p->ctx = va_initialize(p->display, p->log, true);
    if (!p->ctx) {
        vaTerminate(p->display);
        return -1;
    }
    if (hw->reject_emulated && va_guess_if_emulated(p->ctx)) {
        destroy(hw);
        return -1;
    }

    MP_VERBOSE(p, "using VAAPI EGL TFP interop\n");

    hw->hwctx = &p->ctx->hwctx;
    hw->converted_imgfmt = IMGFMT_RGB0;
    return 0;
}

static int reinit(struct gl_hwdec *hw, struct mp_image_params *params)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;

    destroy_texture(hw);

    assert(params->imgfmt == hw->driver->imgfmt);

    gl->GenTextures(1, &p->gl_texture);
    gl->BindTexture(GL_TEXTURE_2D, p->gl_texture);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->BindTexture(GL_TEXTURE_2D, 0);

    p->pixmap = XCreatePixmap(p->xdisplay,
                        RootWindow(p->xdisplay, DefaultScreen(p->xdisplay)),
                        params->w, params->h, 24);
    if (!p->pixmap) {
        MP_FATAL(hw, "could not create pixmap\n");
        return -1;
    }

    p->images[0] = hw->gl->CreateImageKHR(eglGetCurrentDisplay(),
            EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer)p->pixmap, NULL);
    if (!p->images[0]) {
        MP_FATAL(hw, "could not create egl image (%p)\n", eglGetError());
        return -1;
    }
    gl->BindTexture(GL_TEXTURE_2D, p->gl_texture);
    gl->EGLImageTargetTexture2DOES(GL_TEXTURE_2D, p->images[0]);
    gl->BindTexture(GL_TEXTURE_2D, 0);

    return 0;
}

static int map_image(struct gl_hwdec *hw, struct mp_image *hw_image,
                     GLuint *out_textures)
{
    struct priv *p = hw->priv;
    VAStatus status;

    if (!p->pixmap)
        return -1;

    va_lock(p->ctx);
    status = vaPutSurface(p->display, va_surface_id(hw_image), p->pixmap,
                          0, 0, hw_image->w, hw_image->h,
                          0, 0, hw_image->w, hw_image->h,
                          NULL, 0,
                          va_get_colorspace_flag(hw_image->params.colorspace));
    CHECK_VA_STATUS(p, "vaPutSurface()");
    va_unlock(p->ctx);

    out_textures[0] = p->gl_texture;
    return 0;
}

const struct gl_hwdec_driver gl_hwdec_vaegl_tfp = {
    .api_name = "vaapi",
    .imgfmt = IMGFMT_VAAPI,
    .create = create,
    .reinit = reinit,
    .map_image = map_image,
    .destroy = destroy,
};

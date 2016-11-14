/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <string.h>
#include <assert.h>

#include <bcm_host.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/mmal_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_brcm.h>

#include "hwdec.h"
#include "common.h"

struct priv {
    struct mp_hwdec_ctx hwctx;
    struct mp_log *log;
    GLuint texture;
    EGLImageKHR image;
};

static void unmap_frame(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;
    if (p->image != EGL_NO_IMAGE_KHR)
        eglDestroyImageKHR(eglGetCurrentDisplay(), p->image);
    p->image = EGL_NO_IMAGE_KHR;
}

static void destroy_textures(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;

    gl->DeleteTextures(1, &p->texture);
    p->texture = 0;
}

static void destroy(struct gl_hwdec *hw)
{
    struct priv *p = hw->priv;
    unmap_frame(hw);
    destroy_textures(hw);
    hwdec_devices_remove(hw->devs, &p->hwctx);
}

static int create(struct gl_hwdec *hw)
{
    struct priv *p = talloc_zero(hw, struct priv);
    hw->priv = p;
    p->log = hw->log;

    bcm_host_init();
    if (!eglGetCurrentContext())
        return -1;

    p->hwctx.driver_name = hw->driver->name;
    hwdec_devices_add(hw->devs, &p->hwctx);
    return 0;
}

static int reinit(struct gl_hwdec *hw, struct mp_image_params *params)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;

    destroy_textures(hw);

    gl->GenTextures(1, &p->texture);
    params->imgfmt = IMGFMT_RGB0;
    params->hw_subfmt = 0;

    return 0;
}

static int map_frame(struct gl_hwdec *hw, struct mp_image *hw_image,
                     struct gl_hwdec_frame *out_frame)
{
    struct priv *p = hw->priv;
    GL *gl = hw->gl;
    MMAL_BUFFER_HEADER_T *buf = (void *)hw_image->planes[3];
    struct mp_image layout = {0};
    mp_image_set_params(&layout, &hw_image->params);
    mp_image_setfmt(&layout, IMGFMT_RGB0);
    unmap_frame(hw);

    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, p->texture);
    p->image = eglCreateImageKHR(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_IMAGE_BRCM_MULTIMEDIA, (EGLClientBuffer)buf->data, NULL);
    if (p->image == EGL_NO_IMAGE_KHR)
        goto err;

    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, p->image);
    out_frame->planes[0] = (struct gl_hwdec_plane){
        .gl_texture = p->texture,
        .gl_target = GL_TEXTURE_EXTERNAL_OES,
        .tex_w = mp_image_plane_w(&layout, 0),
        .tex_h = mp_image_plane_h(&layout, 0),
    };
    gl->BindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    return 0;
err:
    MP_FATAL(p, "mapping MMAL EGL image failed\n");
    unmap_frame(hw);
    return -1;
}

const struct gl_hwdec_driver gl_hwdec_rpi_egl = {
    .name = "rpi-egl",
    .api = HWDEC_RPI,
    .imgfmt = IMGFMT_MMAL,
    .create = create,
    .reinit = reinit,
    .map_frame = map_frame,
    .unmap = unmap_frame,
    .destroy = destroy,
};

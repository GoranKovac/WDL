#include "xwayland-bridge-gl.h"
#include <gdk/gdkwayland.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <xcb/dri3.h>
#include <xcb/sync.h>
#include <drm_fourcc.h>
#include <unistd.h>
#include <limits.h>
#include <vector>

static void invalidate_gl_window(GdkWindow *window, cairo_region_t *region)
{
    cairo_rectangle_int_t rect = { 0, 0, gdk_window_get_width(window), gdk_window_get_height(window) };
    cairo_region_union_rectangle(region, &rect);
}

struct CaptureDmaBuf {
    xcb_dri3_buffers_from_pixmap_reply_t *reply = nullptr;
    int *fds = nullptr;

    ~CaptureDmaBuf()
    {
        if (fds) for (int i = 0; i < reply->nfd; ++i) close(fds[i]);
        free(reply);
    }

    bool get(xcb_connection_t *connection, xcb_pixmap_t pixmap, xcb_sync_fence_t &fence)
    {
        if (!connection || xcb_connection_has_error(connection)) return false;
        xcb_generic_error_t *error = nullptr;
        if (!fence) {
            const xcb_query_extension_reply_t *extension = xcb_get_extension_data(connection, &xcb_dri3_id);
            if (!extension || !extension->present) return false;
            xcb_dri3_query_version_reply_t *version = xcb_dri3_query_version_reply(connection,
                xcb_dri3_query_version(connection, 1, 2), &error);
            bool supported = !error && version &&
                (version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 2));
            free(version);
            free(error);
            if (!supported) return false;
            xcb_sync_fence_t id = xcb_generate_id(connection);
            error = xcb_request_check(connection, xcb_sync_create_fence_checked(connection, pixmap, id, true));
            supported = !error && !xcb_connection_has_error(connection);
            free(error);
            if (!supported) return false;
            fence = id;
        }
        xcb_sync_reset_fence(connection, fence);
        xcb_sync_trigger_fence(connection, fence);
        error = nullptr;
        reply = xcb_dri3_buffers_from_pixmap_reply(connection,
            xcb_dri3_buffers_from_pixmap(connection, pixmap), &error);
        if (reply) fds = xcb_dri3_buffers_from_pixmap_reply_fds(connection, reply);
        bool supported = !error && reply && reply->nfd > 0 && reply->nfd <= 4;
        free(error);
        return supported;
    }

    EGLImageKHR import(EGLDisplay display)
    {
        if (!epoxy_has_egl_extension(display, "EGL_EXT_image_dma_buf_import")) return EGL_NO_IMAGE_KHR;
        const bool modifiers = epoxy_has_egl_extension(display, "EGL_EXT_image_dma_buf_import_modifiers");
        if (!modifiers && reply->modifier != DRM_FORMAT_MOD_INVALID &&
            reply->modifier != DRM_FORMAT_MOD_LINEAR) return EGL_NO_IMAGE_KHR;
        const uint32_t *strides = xcb_dri3_buffers_from_pixmap_strides(reply);
        const uint32_t *offsets = xcb_dri3_buffers_from_pixmap_offsets(reply);
        static const EGLint planes[][5] = {
            { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
              EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
            { EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
              EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
            { EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
              EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
            { EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
              EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT }
        };
        std::vector<EGLint> attrs = {
            EGL_WIDTH, reply->width, EGL_HEIGHT, reply->height,
            EGL_LINUX_DRM_FOURCC_EXT, (EGLint)(reply->depth == 32 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888)
        };
        for (int i = 0; i < reply->nfd; ++i) {
            if (strides[i] > INT_MAX || offsets[i] > INT_MAX) return EGL_NO_IMAGE_KHR;
            attrs.insert(attrs.end(), { planes[i][0], fds[i], planes[i][1], (EGLint)offsets[i],
                                        planes[i][2], (EGLint)strides[i] });
            if (modifiers && reply->modifier != DRM_FORMAT_MOD_INVALID)
                attrs.insert(attrs.end(), { planes[i][3], (EGLint)reply->modifier,
                                            planes[i][4], (EGLint)(reply->modifier >> 32) });
        }
        attrs.push_back(EGL_NONE);
        return eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
    }
};

bool CaptureGL::make_current()
{
    gdk_gl_context_make_current(context);
    return gdk_gl_context_get_current() == context && eglGetCurrentDisplay() != EGL_NO_DISPLAY;
}

void CaptureGL::reset()
{
    if (fence) {
        xcb_sync_destroy_fence(connection, fence);
        xcb_flush(connection);
        fence = 0;
    }
    connection = nullptr;
    if (context) {
        GdkGLContext *previous = gdk_gl_context_get_current();
        if (previous) g_object_ref(previous);
        if (make_current()) {
            if (pending_read) glDeleteSync((GLsync)pending_read);
            glDeleteFramebuffers(2, framebuffers);
            glDeleteTextures(2, textures);
        }
        pending_read = nullptr;
        if (previous && previous != context) gdk_gl_context_make_current(previous);
        else gdk_gl_context_clear_current();
        if (previous) g_object_unref(previous);
        g_object_unref(context);
        context = nullptr;
    }
    textures[0] = textures[1] = framebuffers[0] = framebuffers[1] = 0;
    width = height = depth = 0;
}

bool CaptureGL::read(const CapturePixmap &pixmap, GdkWindow *window)
{
    if (pixmap.image_format == CAIRO_FORMAT_INVALID) return false;
    const int w = pixmap.width, h = pixmap.height;
    CaptureDmaBuf buffer;
    connection = pixmap.connection;
    if (!buffer.get(connection, pixmap.pixmap, fence) || buffer.reply->width != w || buffer.reply->height != h ||
        buffer.reply->bpp != 32 || buffer.reply->depth != pixmap.format.depth) return false;

    if (!context) {
        GError *error = nullptr;
        context = gdk_window_create_gl_context(window, &error);
        if (error) g_error_free(error);
        if (!context) return false;
        gdk_window_set_invalidate_handler(window, invalidate_gl_window);
        gdk_window_invalidate_rect(window, nullptr, FALSE);
        gdk_gl_context_set_required_version(context, 3, 2);
        error = nullptr;
        bool realized = gdk_gl_context_realize(context, &error);
        if (error) g_error_free(error);
        if (!realized) { g_object_unref(context); context = nullptr; return false; }
        g_signal_connect_object(gdk_window_get_frame_clock(window), "after-paint",
            G_CALLBACK(+[](GdkFrameClock *, gpointer) { gdk_gl_context_clear_current(); }),
            context, G_CONNECT_AFTER);
    }
    if (!make_current() ||
        epoxy_gl_version() < (epoxy_is_desktop_gl() ? 32 : 30) ||
        !epoxy_has_gl_extension("GL_OES_EGL_image")) return false;
    if (pending_read) {
        glWaitSync((GLsync)pending_read, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync((GLsync)pending_read);
        pending_read = nullptr;
    }
    EGLDisplay egl_display = eglGetCurrentDisplay();
    EGLImageKHR image = buffer.import(egl_display);
    if (image == EGL_NO_IMAGE_KHR) return false;
    if (!textures[0]) {
        glGenTextures(2, textures);
        glGenFramebuffers(2, framebuffers);
    }
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    eglDestroyImageKHR(egl_display, image);
    if (glGetError() != GL_NO_ERROR) return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffers[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[0], 0);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;

    glBindTexture(GL_TEXTURE_2D, textures[1]);
    if (width != w || height != h || depth != pixmap.format.depth) {
        glTexImage2D(GL_TEXTURE_2D, 0, pixmap.format.direct.alpha_mask ? GL_RGBA8 : GL_RGB8,
                     w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        width = w; height = h; depth = pixmap.format.depth;
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffers[1]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[1], 0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFlush();
    return glGetError() == GL_NO_ERROR;
}

bool CaptureGL::draw(GtkWidget *widget, cairo_t *cr, const CapturePixmap &pixmap, bool refresh)
{
    if (disabled) return false;
    const int w = pixmap.width, h = pixmap.height;
    GdkWindow *window = gtk_widget_get_window(widget);
    if (!window || !GDK_IS_WAYLAND_WINDOW(window)) return false;
    if (context && gdk_gl_context_get_window(context) != window) reset();
    GdkGLContext *previous = gdk_gl_context_get_current();
    if (previous) g_object_ref(previous);
    bool success = true;
    if (!context || refresh || width != w || height != h)
        success = read(pixmap, window);
    if (success) success = make_current();
    if (success) {
        cairo_save(cr);
        gdk_cairo_draw_from_gl(cr, window, textures[1], GL_TEXTURE,
                              gtk_widget_get_scale_factor(widget), 0, 0, w, h);
        cairo_restore(cr);
        if (pending_read) glDeleteSync((GLsync)pending_read);
        pending_read = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
    } else {
        reset();
        disabled = true;
    }
    if (previous) { gdk_gl_context_make_current(previous); g_object_unref(previous); }
    else gdk_gl_context_clear_current();
    return success;
}

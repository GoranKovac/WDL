#include "xwayland-bridge-image.h"
#include <cairo-xcb.h>
#include <xcb/composite.h>
#include <xcb/xcb_renderutil.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>

void CapturePixmap::reset()
{
    if (pixmap) xcb_free_pixmap(connection, pixmap);
    pixmap = 0;
    width = height = 0;
    connection = nullptr;
    screen = nullptr;
    format = {};
    image_format = CAIRO_FORMAT_INVALID;
}

// (Re-)name the composite pixmap of the plugin window. Automatic redirection
// means the X server keeps drawing the window off-screen; we sample it via
// CompositeNameWindowPixmap and render with cairo. The named
// pixmap is only a reference to whatever backs the window at naming time, so
// it goes stale on every resize and on every Present flip (GLX plugin UIs swap
// the window pixmap for the client's buffer) -- callers re-name on both.
bool CapturePixmap::update(xcb_connection_t *conn, xcb_window_t window)
{
    if (!conn || xcb_connection_has_error(conn)) return false;
    if (connection != conn) reset();
    connection = conn;
    const xcb_pixmap_t next = xcb_generate_id(conn);
    xcb_composite_name_window_pixmap(conn, window, next);
    xcb_get_geometry_cookie_t geometry_cookie = xcb_get_geometry(conn, next);
    xcb_get_window_attributes_cookie_t attributes_cookie = {};
    if (!format.id) attributes_cookie = xcb_get_window_attributes(conn, window);
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(conn, geometry_cookie, nullptr);
    if (!format.id) {
        xcb_get_window_attributes_reply_t *attributes =
            xcb_get_window_attributes_reply(conn, attributes_cookie, nullptr);
        const xcb_render_query_pict_formats_reply_t *formats = xcb_render_util_query_formats(conn);
        xcb_render_pictvisual_t *visual = attributes && formats ?
            xcb_render_util_find_visual_format(formats, attributes->visual) : nullptr;
        if (visual) {
            xcb_render_pictforminfo_t match = {};
            match.id = visual->format;
            xcb_render_pictforminfo_t *info = xcb_render_util_find_format(formats, XCB_PICT_FORMAT_ID, &match, 0);
            if (info) format = *info;
        }
        free(attributes);
        if (format.type == XCB_RENDER_PICT_TYPE_DIRECT &&
            (format.depth == 24 || format.depth == 32) &&
            format.direct.red_shift == 16 && format.direct.red_mask == 0xff &&
            format.direct.green_shift == 8 && format.direct.green_mask == 0xff &&
            format.direct.blue_shift == 0 && format.direct.blue_mask == 0xff &&
            (!format.direct.alpha_mask ||
             (format.direct.alpha_shift == 24 && format.direct.alpha_mask == 0xff)))
            image_format = format.direct.alpha_mask ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24;
    }
    if (!geometry || !format.id || geometry->depth != format.depth) {
        free(geometry);
        xcb_free_pixmap(conn, next);
        return false;
    }
    for (xcb_screen_iterator_t i = xcb_setup_roots_iterator(xcb_get_setup(conn)); i.rem; xcb_screen_next(&i))
        if (i.data->root == geometry->root) { screen = i.data; break; }
    if (pixmap) xcb_free_pixmap(conn, pixmap);
    pixmap = next;
    width = geometry->width;
    height = geometry->height;
    free(geometry);
    return screen != nullptr;
}

cairo_surface_t *CapturePixmap::surface()
{
    return cairo_xcb_surface_create_with_xrender_format(connection, screen, pixmap, &format, width, height);
}

bool CaptureImage::create(const CapturePixmap &pixmap)
{
    connection = pixmap.connection;
    if (pixmap.image_format == CAIRO_FORMAT_INVALID || pixmap.width <= 0 || pixmap.height <= 0) return false;
    xcb_shm_query_version_reply_t *version = xcb_shm_query_version_reply(connection,
        xcb_shm_query_version(connection), nullptr);
    const bool supported = version &&
        (version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 2));
    free(version);
    if (!supported) return false;
    image = xcb_image_create_native(connection, pixmap.width, pixmap.height,
        XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap.format.depth, nullptr, ~0U, nullptr);
    const unsigned int endian = 1;
    if (!image || image->bpp != 32 || !image->stride || image->stride > INT_MAX ||
        (size_t)image->height > SIZE_MAX / image->stride ||
        image->size != (size_t)image->height * image->stride ||
        image->byte_order != (*(const char*)&endian ? XCB_IMAGE_ORDER_LSB_FIRST : XCB_IMAGE_ORDER_MSB_FIRST))
        return false;
    int fd = memfd_create("swell-plugin", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return false;
    if (ftruncate(fd, image->size) < 0 ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0) {
        close(fd);
        return false;
    }
    void *mapping = mmap(nullptr, image->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) { close(fd); return false; }
    image->data = (uint8_t*)mapping;
    shm.shmaddr = image->data;
    shm.shmseg = xcb_generate_id(connection);
    xcb_generic_error_t *error = xcb_request_check(connection,
        xcb_shm_attach_fd_checked(connection, shm.shmseg, fd, false));
    attached = !error && !xcb_connection_has_error(connection);
    free(error);
    if (!attached) return false;
    surface = cairo_image_surface_create_for_data(image->data, pixmap.image_format,
        image->width, image->height, image->stride);
    return cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
}

void CaptureImage::reset()
{
    if (surface) cairo_surface_destroy(surface);
    surface = nullptr;
    if (attached) xcb_shm_detach(connection, shm.shmseg);
    attached = false;
    if (image) {
        if (image->data) munmap(image->data, image->size);
        xcb_image_destroy(image);
    }
    image = nullptr;
    shm = {};
    connection = nullptr;
}

cairo_surface_t *CaptureImage::read(const CapturePixmap &pixmap, bool refresh)
{
    if (disabled) return nullptr;
    if (image && (connection != pixmap.connection || image->width != pixmap.width || image->height != pixmap.height))
        reset();
    if (image && !refresh) return cairo_surface_reference(surface);
    if (!image && !create(pixmap)) {
        reset();
        disabled = true;
        return nullptr;
    }
    cairo_surface_flush(surface);
    if (!xcb_image_shm_get(connection, pixmap.pixmap, image, shm, 0, 0, ~0U)) {
        reset();
        disabled = true;
        return nullptr;
    }
    cairo_surface_mark_dirty(surface);
    return cairo_surface_reference(surface);
}

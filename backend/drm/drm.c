#include "backend/drm/backend.h"
#include "backend/drm/drm.h"
#include "backend/drm/event.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <GLES3/gl3.h>

static const char *conn_name[] = {
	[DRM_MODE_CONNECTOR_Unknown]     = "Unknown",
	[DRM_MODE_CONNECTOR_VGA]         = "VGA",
	[DRM_MODE_CONNECTOR_DVII]        = "DVI-I",
	[DRM_MODE_CONNECTOR_DVID]        = "DVI-D",
	[DRM_MODE_CONNECTOR_DVIA]        = "DVI-A",
	[DRM_MODE_CONNECTOR_Composite]   = "Composite",
	[DRM_MODE_CONNECTOR_SVIDEO]      = "SVIDEO",
	[DRM_MODE_CONNECTOR_LVDS]        = "LVDS",
	[DRM_MODE_CONNECTOR_Component]   = "Component",
	[DRM_MODE_CONNECTOR_9PinDIN]     = "DIN",
	[DRM_MODE_CONNECTOR_DisplayPort] = "DP",
	[DRM_MODE_CONNECTOR_HDMIA]       = "HDMI-A",
	[DRM_MODE_CONNECTOR_HDMIB]       = "HDMI-B",
	[DRM_MODE_CONNECTOR_TV]          = "TV",
	[DRM_MODE_CONNECTOR_eDP]         = "eDP",
	[DRM_MODE_CONNECTOR_VIRTUAL]     = "Virtual",
	[DRM_MODE_CONNECTOR_DSI]         = "DSI",
};

// EGL extensions
PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC create_platform_window_surface;

static bool egl_exts()
{
	get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");

	create_platform_window_surface = (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)
		eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");

	return get_platform_display && create_platform_window_surface;
}

static bool egl_get_config(EGLDisplay disp, EGLConfig *out)
{
	EGLint count = 0, matched = 0, ret;

	ret = eglGetConfigs(disp, NULL, 0, &count);
	if (ret == EGL_FALSE || count == 0) {
		return false;
	}

	EGLConfig configs[count];

	ret = eglChooseConfig(disp, NULL, configs, count, &matched);
	if (ret == EGL_FALSE) {
		return false;
	}

	for (int i = 0; i < matched; ++i) {
		EGLint gbm_format;

		if (!eglGetConfigAttrib(disp,
					configs[i],
					EGL_NATIVE_VISUAL_ID,
					&gbm_format))
			continue;

		if (gbm_format == GBM_FORMAT_XRGB8888) {
			*out = configs[i];
			return true;
		}
	}

	return false;
}

bool wlr_drm_renderer_init(struct wlr_drm_renderer *renderer,
		struct wlr_drm_backend *backend, int fd)
{
	if (!egl_exts()) {
		fprintf(stderr, "Could not get EGL extensions\n");
		return false;
	}

	renderer->gbm = gbm_create_device(fd);
	if (!renderer->gbm) {
		fprintf(stderr, "Could not create gbm device: %s\n", strerror(errno));
		return false;
	}

	if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
		fprintf(stderr, "Could not bind GLES3 API\n");
		goto error_gbm;
	}

	renderer->egl.disp = get_platform_display(EGL_PLATFORM_GBM_MESA, renderer->gbm, NULL);
	if (renderer->egl.disp == EGL_NO_DISPLAY) {
		fprintf(stderr, "Could not create EGL display\n");
		goto error_gbm;
	}

	if (eglInitialize(renderer->egl.disp, NULL, NULL) == EGL_FALSE) {
		fprintf(stderr, "Could not initalise EGL\n");
		goto error_egl;
	}

	if (!egl_get_config(renderer->egl.disp, &renderer->egl.conf)) {
		fprintf(stderr, "Could not get EGL config\n");
		goto error_egl;
	}

	static const EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};

	renderer->egl.context = eglCreateContext(renderer->egl.disp,
			renderer->egl.conf, EGL_NO_CONTEXT, attribs);

	if (renderer->egl.context == EGL_NO_CONTEXT) {
		fprintf(stderr, "Could not create EGL context\n");
		goto error_egl;
	}

	renderer->fd = fd;
	renderer->backend = backend;

	return true;

error_egl:
	eglTerminate(renderer->egl.disp);
	eglReleaseThread();
	eglMakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

error_gbm:
	gbm_device_destroy(renderer->gbm);

	return false;
}

void wlr_drm_renderer_free(struct wlr_drm_renderer *renderer)
{
	if (!renderer)
		return;

	eglDestroyContext(renderer->egl.disp, renderer->egl.context);
	eglTerminate(renderer->egl.disp);
	eglReleaseThread();
	eglMakeCurrent(EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

	gbm_device_destroy(renderer->gbm);
}

void wlr_drm_scan_connectors(struct wlr_drm_backend *backend)
{
	drmModeRes *res = drmModeGetResources(backend->fd);
	if (!res)
		return;

	// I don't know if this needs to be allocated with realloc like this,
	// as it may not even be possible for the number of connectors to change.
	// I'll just have to see how DisplayPort MST works with DRM.
	if ((size_t)res->count_connectors > backend->display_len) {
		struct wlr_drm_display *new = realloc(backend->displays, sizeof *new * res->count_connectors);
		if (!new)
			goto error;

		for (int i = backend->display_len; i < res->count_connectors; ++i) {
			new[i] = (struct wlr_drm_display) {
				.state = DRM_DISP_INVALID,
				.renderer = &backend->renderer,
			};
		}

		backend->display_len = res->count_connectors;
		backend->displays = new;
	}

	for (int i = 0; i < res->count_connectors; ++i) {
		struct wlr_drm_display *disp = &backend->displays[i];
		drmModeConnector *conn = drmModeGetConnector(backend->fd, res->connectors[i]);
		if (!conn)
			continue;

		if (backend->displays[i].state == DRM_DISP_INVALID) {
			disp->state = DRM_DISP_DISCONNECTED;
			disp->connector = res->connectors[i];
			snprintf(disp->name, sizeof disp->name, "%s-%"PRIu32,
				 conn_name[conn->connector_type],
				 conn->connector_type_id);
		}

		if (disp->state == DRM_DISP_DISCONNECTED &&
		    conn->connection == DRM_MODE_CONNECTED) {
			disp->state = DRM_DISP_NEEDS_MODESET;
			wlr_drm_add_event(backend, disp, DRM_EV_DISPLAY_ADD);

		} else if (disp->state == DRM_DISP_CONNECTED &&
		    conn->connection != DRM_MODE_CONNECTED) {
			disp->state = DRM_DISP_DISCONNECTED;
			wlr_drm_add_event(backend, disp, DRM_EV_DISPLAY_REM);
		}

		drmModeFreeConnector(conn);
	}

error:
	drmModeFreeResources(res);
}

static void free_fb(struct gbm_bo *bo, void *data)
{
	uint32_t *id = data;

	if (id && *id)
		drmModeRmFB(gbm_bo_get_fd(bo), *id);

	free(id);
}

static uint32_t get_fb_for_bo(int fd, struct gbm_bo *bo)
{
	uint32_t *id = gbm_bo_get_user_data(bo);

	if (id)
		return *id;

	id = calloc(1, sizeof *id);
	if (!id)
		return 0;

	drmModeAddFB(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), 24, 32,
		     gbm_bo_get_stride(bo), gbm_bo_get_handle(bo).u32, id);

	gbm_bo_set_user_data(bo, id, free_fb);

	return *id;
}

static bool display_init_renderer(struct wlr_drm_renderer *renderer,
		struct wlr_drm_display *disp)
{
	disp->renderer = renderer;

	disp->gbm = gbm_surface_create(renderer->gbm,
				       disp->width, disp->height,
				       GBM_FORMAT_XRGB8888,
				       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!disp->gbm)
		return false;

	disp->egl = create_platform_window_surface(renderer->egl.disp, renderer->egl.conf,
						   disp->gbm, NULL);
	if (disp->egl == EGL_NO_SURFACE)
		return false;

	// Render black frame

	eglMakeCurrent(renderer->egl.disp, disp->egl, disp->egl, renderer->egl.context);

	glViewport(0, 0, disp->width, disp->height);
	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	eglSwapBuffers(renderer->egl.disp, disp->egl);

	struct gbm_bo *bo = gbm_surface_lock_front_buffer(disp->gbm);
	uint32_t fb_id = get_fb_for_bo(renderer->fd, bo);

	drmModeSetCrtc(renderer->fd, disp->crtc, fb_id, 0, 0,
		       &disp->connector, 1, disp->active_mode);
	drmModePageFlip(renderer->fd, disp->crtc, fb_id, DRM_MODE_PAGE_FLIP_EVENT, disp);

	gbm_surface_release_buffer(disp->gbm, bo);

	return true;
}

static drmModeModeInfo *select_mode(size_t num_modes,
		drmModeModeInfo modes[static num_modes],
		drmModeCrtc *old_crtc,
		const char *str)
{
	if (strcmp(str, "preferred") == 0)
		return &modes[0];

	if (strcmp(str, "current") == 0) {
		if (!old_crtc) {
			fprintf(stderr, "Display does not have currently configured mode\n");
			return NULL;
		}

		for (size_t i = 0; i < num_modes; ++i) {
			if (memcmp(&modes[i], &old_crtc->mode, sizeof modes[0]) == 0)
				return &modes[i];
		}

		// We should never get here
		assert(0);
	}

	unsigned width = 0;
	unsigned height = 0;
	unsigned rate = 0;
	int ret;

	if ((ret = sscanf(str, "%ux%u@%u", &width, &height, &rate)) != 2 && ret != 3) {
		fprintf(stderr, "Invalid modesetting string\n");
		return NULL;
	}

	for (size_t i = 0; i < num_modes; ++i) {
		if (modes[i].hdisplay == width &&
		    modes[i].vdisplay == height &&
		    (!rate || modes[i].vrefresh == rate))
			return &modes[i];
	}

	return NULL;
}

bool wlr_drm_display_modeset(struct wlr_drm_backend *backend,
		struct wlr_drm_display *disp, const char *str)
{
	drmModeConnector *conn = drmModeGetConnector(backend->fd, disp->connector);
	if (!conn || conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0)
		goto error;

	disp->num_modes = conn->count_modes;
	disp->modes = malloc(sizeof *disp->modes * disp->num_modes);
	if (!disp->modes)
		goto error;
	memcpy(disp->modes, conn->modes, sizeof *disp->modes * disp->num_modes);

	drmModeEncoder *curr_enc = drmModeGetEncoder(backend->fd, conn->encoder_id);
	if (curr_enc) {
		disp->old_crtc = drmModeGetCrtc(backend->fd, curr_enc->crtc_id);
		free(curr_enc);
	}

	disp->active_mode = select_mode(disp->num_modes, disp->modes, disp->old_crtc, str);
	if (!disp->active_mode) {
		fprintf(stderr, "Could not find mode '%s' for %s\n", str, disp->name);
		goto error;
	}

	fprintf(stderr, "Configuring %s with mode %ux%u@%u\n",
		disp->name, disp->active_mode->hdisplay, disp->active_mode->vdisplay,
		disp->active_mode->vrefresh);

	drmModeRes *res = drmModeGetResources(backend->fd);
	if (!res)
		goto error;

	bool success = false;
	for (int i = 0; !success && i < conn->count_encoders; ++i) {
		drmModeEncoder *enc = drmModeGetEncoder(backend->fd, conn->encoders[i]);
		if (!enc)
			continue;

		for (int j = 0; j < res->count_crtcs; ++j) {
			if ((enc->possible_crtcs & (1 << j)) == 0)
				continue;

			if ((backend->taken_crtcs & (1 << j)) == 0) {
				backend->taken_crtcs |= 1 << j;
				disp->crtc = res->crtcs[j];

				success = true;
				break;
			}
		}

		drmModeFreeEncoder(enc);
	}

	drmModeFreeResources(res);

	if (!success)
		goto error;

	disp->state = DRM_DISP_CONNECTED;
	drmModeFreeConnector(conn);

	disp->width = disp->active_mode->hdisplay;
	disp->height = disp->active_mode->vdisplay;

	display_init_renderer(&backend->renderer, disp);

	return true;
error:
	disp->state = DRM_DISP_DISCONNECTED;
	drmModeFreeConnector(conn);

	wlr_drm_add_event(backend, disp, DRM_EV_DISPLAY_REM);

	return false;
}

static void page_flip_handler(int fd,
		unsigned seq,
		unsigned tv_sec,
		unsigned tv_usec,
		void *user)
{
	struct wlr_drm_display *disp = user;

	disp->pageflip_pending = true;
	if (!disp->cleanup)
		wlr_drm_add_event(disp->renderer->backend, disp, DRM_EV_RENDER);
}

void wlr_drm_display_free(struct wlr_drm_display *disp)
{
	if (!disp || disp->state != DRM_DISP_CONNECTED)
		return;

	struct wlr_drm_renderer *renderer = disp->renderer;

	drmModeCrtc *crtc = disp->old_crtc;
	if (crtc) {
		// Wait for exising page flips to finish

		drmEventContext event = {
			.version = DRM_EVENT_CONTEXT_VERSION,
			.page_flip_handler = page_flip_handler,
		};

		disp->cleanup = true;
		while (disp->pageflip_pending)
			drmHandleEvent(renderer->fd, &event);

		drmModeSetCrtc(renderer->fd, crtc->crtc_id, crtc->buffer_id,
			       crtc->x, crtc->y, &disp->connector,
			       1, &crtc->mode);
		drmModeFreeCrtc(crtc);
	}

	eglDestroySurface(renderer->egl.disp, disp->egl);
	gbm_surface_destroy(disp->gbm);

	free(disp->modes);
}

void wlr_drm_event(int fd)
{
	drmEventContext event = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = page_flip_handler,
	};

	drmHandleEvent(fd, &event);
}

void wlr_drm_display_begin(struct wlr_drm_display *disp)
{
	struct wlr_drm_renderer *renderer = disp->renderer;
	eglMakeCurrent(renderer->egl.disp, disp->egl, disp->egl, renderer->egl.context);
}

void wlr_drm_display_end(struct wlr_drm_display *disp)
{
	struct wlr_drm_renderer *renderer = disp->renderer;
	eglSwapBuffers(renderer->egl.disp, disp->egl);

	struct gbm_bo *bo = gbm_surface_lock_front_buffer(disp->gbm);
	uint32_t fb_id = get_fb_for_bo(renderer->fd, bo);

	drmModePageFlip(renderer->fd, disp->crtc, fb_id, DRM_MODE_PAGE_FLIP_EVENT, disp);

	gbm_surface_release_buffer(disp->gbm, bo);

	disp->pageflip_pending = false;
}

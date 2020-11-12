#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "util/signal.h"
#include "util/uuid.h"
#include "xdg-foreign-unstable-v1-protocol.h"

#define FOREIGN_V1_VERSION 1

static const struct zxdg_exported_v1_interface xdg_exported_impl;
static const struct zxdg_imported_v1_interface xdg_imported_impl;
static const struct zxdg_exporter_v1_interface xdg_exporter_impl;
static const struct zxdg_importer_v1_interface xdg_importer_impl;

static struct wlr_xdg_imported_v1 *xdg_imported_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zxdg_imported_v1_interface,
		&xdg_imported_impl));
	return wl_resource_get_user_data(resource);
}

static void xdg_imported_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static bool verify_is_toplevel(struct wl_resource *client_resource,
		struct wlr_surface *surface) {
	if (wlr_surface_is_xdg_surface(surface)) {
		struct wlr_xdg_surface *xdg_surface =
			wlr_xdg_surface_from_wlr_surface(surface);
		if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
			wl_resource_post_error(client_resource, -1,
					"surface must be an xdg_toplevel");
			return false;
		}
	} else {
		wl_resource_post_error(client_resource, -1,
				"surface must be an xdg_surface");
		return false;
	}
	return true;
}

static void destroy_imported_child(struct wlr_xdg_imported_child_v1 *child) {
	wl_list_remove(&child->xdg_toplevel_set_parent.link);
	wl_list_remove(&child->xdg_surface_unmap.link);
	wl_list_remove(&child->link);
	free(child);
}

static void handle_child_xdg_surface_unmap(
		struct wl_listener *listener, void *data) {
	struct wlr_xdg_imported_child_v1 *child =
		wl_container_of(listener, child, xdg_surface_unmap);
	destroy_imported_child(child);
}

static void handle_xdg_toplevel_set_parent(
		struct wl_listener *listener, void *data) {
	struct wlr_xdg_imported_child_v1 *child =
		wl_container_of(listener, child, xdg_toplevel_set_parent);
	destroy_imported_child(child);
}

static void xdg_imported_handle_set_parent_of(struct wl_client *client,
		struct wl_resource *resource,
		struct wl_resource *child_resource) {
	struct wlr_xdg_imported_v1 *imported =
		xdg_imported_from_resource(resource);
	if (imported->exported == NULL) {
		return;
	}
	struct wlr_surface *wlr_surface = imported->exported->surface;
	struct wlr_surface *wlr_surface_child =
		wlr_surface_from_resource(child_resource);

	if (!verify_is_toplevel(resource, wlr_surface_child)) {
		return;
	}
	if (wlr_surface_is_xdg_surface(wlr_surface)
			!= wlr_surface_is_xdg_surface(wlr_surface_child)) {
		wl_resource_post_error(resource, -1,
				"surfaces must have the same role");
		return;
	}
	struct wlr_xdg_imported_child_v1 *child;
	wl_list_for_each(child, &imported->children, link) {
		if (child->surface == wlr_surface_child) {
			return;
		}
	}

	child = calloc(1, sizeof(struct wlr_xdg_imported_child_v1));
	if (child == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	child->surface = wlr_surface_child;
	child->xdg_surface_unmap.notify = handle_child_xdg_surface_unmap;
	child->xdg_toplevel_set_parent.notify = handle_xdg_toplevel_set_parent;

	if (wlr_surface_is_xdg_surface(wlr_surface)) {
		struct wlr_xdg_surface *surface =
			wlr_xdg_surface_from_wlr_surface(wlr_surface);
		struct wlr_xdg_surface *surface_child =
			wlr_xdg_surface_from_wlr_surface(wlr_surface_child);

		wlr_xdg_toplevel_set_parent(surface_child, surface);
		wl_signal_add(&surface_child->events.unmap,
				&child->xdg_surface_unmap);
		wl_signal_add(&surface_child->toplevel->events.set_parent,
				&child->xdg_toplevel_set_parent);
	}

	wl_list_insert(&imported->children, &child->link);
}

static const struct zxdg_imported_v1_interface xdg_imported_impl = {
	.destroy = xdg_imported_handle_destroy,
	.set_parent_of = xdg_imported_handle_set_parent_of
};

static struct wlr_xdg_exported_v1 *xdg_exported_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zxdg_exported_v1_interface,
		&xdg_exported_impl));
	return wl_resource_get_user_data(resource);
}

static void xdg_exported_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zxdg_exported_v1_interface xdg_exported_impl = {
	.destroy = xdg_exported_handle_destroy
};

static void xdg_exporter_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static struct wlr_xdg_exporter_v1 *xdg_exporter_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zxdg_exporter_v1_interface,
		&xdg_exporter_impl));
	return wl_resource_get_user_data(resource);
}

static void disconnect_imported(struct wlr_xdg_imported_v1 *imported) {
	if (imported->exported != NULL) {
		imported->exported = NULL;
		zxdg_imported_v1_send_destroyed(imported->resource);
		wl_list_remove(&imported->export_link);
	}
}

static void xdg_exported_handle_resource_destroy(
		struct wl_resource *resource) {
	struct wlr_xdg_exported_v1 *exported =
		xdg_exported_from_resource(resource);
	struct wlr_xdg_imported_v1 *imported, *imported_tmp;
	wl_list_for_each_safe(imported, imported_tmp,
			&exported->imports, export_link) {
		disconnect_imported(imported);
	}
	wl_list_remove(&exported->xdg_surface_unmap.link);
	wl_list_remove(&exported->link);
	free(exported);
}

static void handle_xdg_surface_unmap(
		struct wl_listener *listener, void *data) {
	struct wlr_xdg_exported_v1 *exported =
		wl_container_of(listener, exported, xdg_surface_unmap);
	wl_resource_destroy(exported->resource);
}

static struct wlr_xdg_exported_v1 *find_exported(
		struct wlr_xdg_foreign_v1 *foreign, const char *handle) {
	if (handle == NULL) {
		return NULL;
	}
	struct wlr_xdg_exporter_v1 *exporter;
	wl_list_for_each(exporter, &foreign->exporter.clients, link) {
		struct wlr_xdg_exported_v1 *exported;
		wl_list_for_each(exported, &exporter->exports, link) {
			if (strcmp(handle, exported->handle) == 0) {
				return exported;
			}
		}
	}
	return NULL;
}

static void xdg_exporter_handle_export_toplevel(struct wl_client *wl_client,
		struct wl_resource *client_resource,
		uint32_t id,
		struct wl_resource *surface_resource) {
	struct wlr_xdg_exporter_v1 *exporter =
		xdg_exporter_from_resource(client_resource);
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);

	if (!verify_is_toplevel(client_resource, surface)) {
		return;
	}

	struct wlr_xdg_exported_v1 *exported =
		calloc(1, sizeof(struct wlr_xdg_exported_v1));
	if (exported == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	exported->surface = surface;
	do {
		if (generate_uuid(exported->handle)) {
			wl_client_post_no_memory(wl_client);
			free(exported);
			return;
		}
	} while (find_exported(exporter->foreign, exported->handle) != NULL);
	exported->resource = wl_resource_create(wl_client,
		&zxdg_exported_v1_interface,
		wl_resource_get_version(exporter->resource),
		id);

	wl_resource_set_implementation(exported->resource, &xdg_exported_impl,
			exported, xdg_exported_handle_resource_destroy);

	wl_list_insert(&exporter->exports, &exported->link);
	wl_list_init(&exported->imports);

	zxdg_exported_v1_send_handle(exported->resource, exported->handle);

	exported->xdg_surface_unmap.notify = handle_xdg_surface_unmap;
	if (wlr_surface_is_xdg_surface(surface)) {
		struct wlr_xdg_surface *xdg_surface =
			wlr_xdg_surface_from_wlr_surface(surface);
		wl_signal_add(&xdg_surface->events.unmap, &exported->xdg_surface_unmap);
	}
}

static const struct zxdg_exporter_v1_interface xdg_exporter_impl = {
	.destroy = xdg_exporter_handle_destroy,
	.export = xdg_exporter_handle_export_toplevel
};

static void xdg_exporter_handle_resource_destroy(
		struct wl_resource *resource) {
	struct wlr_xdg_exporter_v1 *exporter = xdg_exporter_from_resource(resource);
	struct wlr_xdg_exported_v1 *exported, *exported_tmp;
	wl_list_for_each_safe(exported, exported_tmp, &exporter->exports, link) {
		wl_resource_destroy(exported->resource);
	}
	wl_list_remove(&exporter->link);
	free(exporter);
}

static void xdg_exporter_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_xdg_foreign_v1 *foreign = data;

	struct wlr_xdg_exporter_v1 *exporter =
		calloc(1, sizeof(struct wlr_xdg_exporter_v1));
	if (exporter == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	exporter->foreign = foreign;
	exporter->resource =
		wl_resource_create(wl_client, &zxdg_exporter_v1_interface, version, id);
	if (exporter->resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_list_init(&exporter->exports);

	wl_resource_set_implementation(exporter->resource, &xdg_exporter_impl,
			exporter, xdg_exporter_handle_resource_destroy);

	wl_list_insert(&foreign->exporter.clients, &exporter->link);
}

static struct wlr_xdg_importer_v1 *xdg_importer_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zxdg_importer_v1_interface,
		&xdg_importer_impl));
	return wl_resource_get_user_data(resource);
}

static void xdg_importer_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void xdg_imported_handle_resource_destroy(
		struct wl_resource *resource) {
	struct wlr_xdg_imported_v1 *imported = xdg_imported_from_resource(resource);

	struct wlr_xdg_imported_child_v1 *child, *child_tmp;
	wl_list_for_each_safe(child, child_tmp, &imported->children, link) {
		struct wlr_xdg_surface *xdg_child =
			wlr_xdg_surface_from_wlr_surface(child->surface);
		wlr_xdg_toplevel_set_parent(xdg_child, NULL);
	}
	if (imported->export_link.prev != NULL) {
		wl_list_remove(&imported->export_link);
	}
	if (imported->link.prev != NULL) {
		wl_list_remove(&imported->link);
	}
	free(imported);
}

static void xdg_importer_handle_import_toplevel(struct wl_client *wl_client,
				struct wl_resource *client_resource,
				uint32_t id,
				const char *handle) {
	struct wlr_xdg_importer_v1 *importer =
		xdg_importer_from_resource(client_resource);

	struct wlr_xdg_imported_v1 *imported =
		calloc(1, sizeof(struct wlr_xdg_imported_v1));
	if (imported == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	imported->exported = find_exported(importer->foreign, handle);
	imported->resource = wl_resource_create(wl_client,
		&zxdg_imported_v1_interface,
		wl_resource_get_version(importer->resource),
		id);
	wl_resource_set_implementation(imported->resource, &xdg_imported_impl,
			imported, xdg_imported_handle_resource_destroy);

	wl_list_init(&imported->children);
	wl_list_insert(&importer->imports, &imported->link);

	if (imported->exported == NULL) {
		zxdg_imported_v1_send_destroyed(imported->resource);
	} else {
		wl_list_insert(&imported->exported->imports, &imported->export_link);
	}
}

static const struct zxdg_importer_v1_interface xdg_importer_impl = {
	.destroy = xdg_importer_handle_destroy,
	.import = xdg_importer_handle_import_toplevel
};

static void xdg_importer_handle_resource_destroy(
		struct wl_resource *resource) {
	struct wlr_xdg_importer_v1 *importer =
		xdg_importer_from_resource(resource);
	struct wlr_xdg_imported_v1 *imported, *imported_tmp;
	wl_list_for_each_safe(imported, imported_tmp, &importer->imports, link) {
		disconnect_imported(imported);
		wl_list_remove(&imported->link);
	}
	wl_list_remove(&importer->link);
	free(importer);
}

static void xdg_importer_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct wlr_xdg_foreign_v1 *foreign = data;

	struct wlr_xdg_importer_v1 *importer =
		calloc(1, sizeof(struct wlr_xdg_importer_v1));
	if (importer == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	importer->foreign = foreign;
	importer->resource =
		wl_resource_create(wl_client, &zxdg_importer_v1_interface, version, id);
	if (importer->resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_list_init(&importer->imports);

	wl_resource_set_implementation(importer->resource, &xdg_importer_impl,
			importer, xdg_importer_handle_resource_destroy);

	wl_list_insert(&foreign->importer.clients, &importer->link);
}

void wlr_xdg_foreign_v1_destroy(
		struct wlr_xdg_foreign_v1 *foreign) {
	if (!foreign) {
		return;
	}

	struct wl_resource *resource, *tmp_resource;
	wl_resource_for_each_safe(resource, tmp_resource,
			&foreign->importer.resources) {
		wl_resource_destroy(resource);
	}
	wl_resource_for_each_safe(resource, tmp_resource,
			&foreign->exporter.resources) {
		wl_resource_destroy(resource);
	}

	wlr_signal_emit_safe(&foreign->events.destroy, foreign);
	wl_list_remove(&foreign->display_destroy.link);

	wl_global_destroy(foreign->exporter.global);
	wl_global_destroy(foreign->importer.global);
	free(foreign);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_xdg_foreign_v1 *foreign =
		wl_container_of(listener, foreign, display_destroy);
	wlr_xdg_foreign_v1_destroy(foreign);
}

struct wlr_xdg_foreign_v1 *wlr_xdg_foreign_v1_create(
		struct wl_display *display) {
	struct wlr_xdg_foreign_v1 *foreign = calloc(1,
			sizeof(struct wlr_xdg_foreign_v1));
	if (!foreign) {
		return NULL;
	}

	foreign->exporter.global = wl_global_create(display,
			&zxdg_exporter_v1_interface,
			FOREIGN_V1_VERSION, foreign,
			xdg_exporter_bind);
	if (!foreign->exporter.global) {
		free(foreign);
		return NULL;
	}

	foreign->importer.global = wl_global_create(display,
			&zxdg_importer_v1_interface,
			FOREIGN_V1_VERSION, foreign,
			xdg_importer_bind);
	if (!foreign->importer.global) {
		wl_global_destroy(foreign->exporter.global);
		free(foreign);
		return NULL;
	}

	wl_signal_init(&foreign->events.destroy);
	wl_list_init(&foreign->exporter.resources);
	wl_list_init(&foreign->exporter.clients);
	wl_list_init(&foreign->importer.resources);
	wl_list_init(&foreign->importer.clients);

	foreign->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &foreign->display_destroy);

	return foreign;
}

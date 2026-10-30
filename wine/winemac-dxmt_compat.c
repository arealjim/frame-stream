/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 arealjim
 *
 * The macdrv_functions table and the win data layout are DXMT's (LGPL-2.1-or-later,
 * Copyright (c) 2023-2026 Feifan He for CodeWeavers), reproduced so its lookups resolve.
 *
 * dxmt_compat.c - the macdrv_functions table DXMT (github.com/3Shain/dxmt) looks up with dlsym(RTLD_DEFAULT),
 * which CrossOver's winemac exports and upstream's does not. DXMT reads a four-pointer win data
 * (hwnd, cocoa_window, cocoa_view, client_cocoa_view); upstream's struct differs, so get_win_data hands it a copy
 * in that layout and keeps the real one, locked, until release_win_data. It also creates the client view upstream
 * only makes for an OpenGL or Vulkan surface; without it DXMT fails with "Failed to create metal view".
 */
#if 0
#pragma makedep unix
#endif
#include "config.h"
#include <stdlib.h>
#include <dispatch/dispatch.h>
#include "macdrv.h"

extern void OnMainThread(dispatch_block_t block);

struct dxmt_win_data
{
    HWND hwnd;
    macdrv_window cocoa_window;
    macdrv_view cocoa_view;
    macdrv_view client_cocoa_view;
    struct macdrv_win_data *real;
};

static struct macdrv_win_data *dxmt_get_win_data(HWND hwnd)
{
    struct macdrv_win_data *data = get_win_data(hwnd);
    struct dxmt_win_data *copy;
    if (!data) return NULL;
    if (!(copy = calloc(1, sizeof(*copy)))) { release_win_data(data); return NULL; }
    if (!data->client_view && data->cocoa_window)
    {
        /* upstream makes a client view only for a GL or Vulkan surface; DXMT needs one to hang its layer on */
        RECT r = data->rects.client;
        OffsetRect(&r, -data->rects.visible.left, -data->rects.visible.top);
        data->client_view = macdrv_create_view(cgrect_from_rect(r));
        macdrv_set_view_superview(data->client_view, NULL, data->cocoa_window, NULL, NULL);
        macdrv_set_view_hidden(data->client_view, FALSE);
    }
    copy->hwnd = data->hwnd;
    copy->cocoa_window = data->cocoa_window;
    copy->cocoa_view = data->client_view;
    copy->client_cocoa_view = data->client_view;
    copy->real = data;
    return (struct macdrv_win_data *)copy;
}

static void dxmt_release_win_data(struct macdrv_win_data *data)
{
    struct dxmt_win_data *copy = (struct dxmt_win_data *)data;
    if (!copy) return;
    release_win_data(copy->real);
    free(copy);
}

struct macdrv_functions_t
{
    void (*macdrv_init_display_devices)(BOOL);
    struct macdrv_win_data *(*get_win_data)(HWND hwnd);
    void (*release_win_data)(struct macdrv_win_data *data);
    macdrv_window (*macdrv_get_cocoa_window)(HWND hwnd, BOOL require_on_screen);
    macdrv_metal_device (*macdrv_create_metal_device)(void);
    void (*macdrv_release_metal_device)(macdrv_metal_device d);
    macdrv_metal_view (*macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d);
    macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view v);
    void (*macdrv_view_release_metal_view)(macdrv_metal_view v);
    void (*on_main_thread)(dispatch_block_t b);
};

__attribute__((visibility("default"))) struct macdrv_functions_t macdrv_functions =
{
    NULL,
    dxmt_get_win_data,
    dxmt_release_win_data,
    macdrv_get_cocoa_window,
    macdrv_create_metal_device,
    macdrv_release_metal_device,
    macdrv_view_create_metal_view,
    macdrv_view_get_metal_layer,
    macdrv_view_release_metal_view,
    OnMainThread,
};

/*
 * Copyright © 2013 Samsung Electronics
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *   Bryce Harrington <b.harrington@samsung.com>
 *   Henry Song <henry.song@samsung.com>
 */

/* This test exercises gaussian blur rendering
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <cairo.h>
#include <cairo-gl.h>

#include "cairo-test.h"

static cairo_test_status_t
draw (cairo_t *cr, int width, int height)
{
    const cairo_test_context_t *ctx = cairo_test_get_context (cr);

    int rgba_attribs[] = {
	GLX_RGBA,
	GLX_RED_SIZE, 1,
	GLX_GREEN_SIZE, 1,
	GLX_BLUE_SIZE, 1,
	GLX_ALPHA_SIZE, 1,
	GLX_STENCIL_SIZE, 1,
	GLX_SAMPLES, 4,
	GLX_SAMPLE_BUFFERS, 1,
	GLX_DOUBLEBUFFER,
	None
    };
    int line_width = 20;
    int x = width / 2;
    int y = height / 2;
    int radius = width / 4;

    cairo_save (cr);
    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_paint (cr);
    cairo_restore (cr);

    cairo_save (cr);

    /* drop shadow */
    cairo_arc (cr, x, y, radius, 0, 2 * M_PI);
    cairo_set_line_width (cr, line_width);
    cairo_set_draw_shadow_only (cr, 1);
    cairo_set_shadow (cr, CAIRO_SHADOW_DROP);
    cairo_set_shadow_rgba (cr, 0, 0, 0, 0.8);
    cairo_set_shadow_blur (cr, 10, 10);
    cairo_set_shadow_offset (cr, -42, -7);
    cairo_stroke (cr);

    /* ring with inset shadow */
    cairo_arc (cr, x, y, radius, 0, 2 * M_PI);
    cairo_set_line_width (cr, line_width);
    cairo_set_source_rgb (cr, 0, 0.5, 0);
    cairo_set_draw_shadow_only (cr, 0);
    cairo_set_shadow (cr, CAIRO_SHADOW_INSET);
    cairo_set_shadow_rgba (cr, 0, 0, 0, 1);
    cairo_set_shadow_blur (cr, 5, 2);
    cairo_set_shadow_offset (cr, 6, 1);
    cairo_stroke (cr);

    /* draw spread */
    cairo_set_draw_shadow_only (cr, 1);
    cairo_set_shadow (cr, CAIRO_SHADOW_DROP);
    cairo_set_shadow_rgba (cr, 1, 1, 1, 1);
    cairo_set_shadow_blur (cr, 5, 2);
    cairo_set_line_width (cr, line_width/5);
    cairo_set_source_rgb (cr, 0, 0.5, 0);
    cairo_set_shadow_offset (cr, 6, 1);
    cairo_arc (cr, x, y, radius, 0, 2 * M_PI);
    cairo_stroke (cr);

    cairo_restore (cr);

    return CAIRO_TEST_SUCCESS;
}

CAIRO_TEST (blur,
	    "Tests gaussian blur of a drawn image",
	    "gl, blur, operator", /* keywords */
	    NULL, /* requirements */
	    256, 256,
	    NULL, draw)

/* -*- Mode: c; tab-width: 8; c-basic-offset: 4; indent-tabs-mode: t; -*- */
/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2003 University of Southern California
 * Copyright © 2009,2010,2011 Intel Corporation
 * Copyright © 2013 Samsung Research America, Silicon Valley
 *
 * This library is free software; you can redistribute it and/or
 * modify it either under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * (the "LGPL") or, at your option, under the terms of the Mozilla
 * Public License Version 1.1 (the "MPL"). If you do not alter this
 * notice, a recipient may use your version of this file under either
 * the MPL or the LGPL.
 *
 * You should have received a copy of the LGPL along with this library
 * in the file COPYING-LGPL-2.1; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA
 * You should have received a copy of the MPL along with this library
 * in the file COPYING-MPL-1.1
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY
 * OF ANY KIND, either express or implied. See the LGPL or the MPL for
 * the specific language governing rights and limitations.
 *
 * The Original Code is the cairo graphics library.
 *
 * The Initial Developer of the Original Code is University of Southern
 * California.
 *
 * Contributor(s):
 *	Henry Song <henry.song@samsung.com>
 */

/* The purpose of this file/surface is to simply translate a pattern
 * to a pixman_image_t and thence to feed it back to the general
 * compositor interface.
 */

#include "cairoint.h"

#include "cairo-image-surface-private.h"

#include "cairo-error-private.h"
#include "cairo-pattern-inline.h"
#include "cairo-filters-private.h"
#include "cairo-image-filters-private.h"

static pixman_fixed_t *
_pixman_image_create_convolution_params (double *params,
					 int col, int row,
					 cairo_bool_t x_pass)
{
    int i;
    pixman_fixed_t *pixman_params;
    double *coef;
    int length;

    if ( params == NULL)
	return NULL;

    if (x_pass) {
	pixman_params = _cairo_malloc_ab (col + 2, sizeof (double));
 	pixman_params[0] = pixman_int_to_fixed (col);
	pixman_params[1] = pixman_int_to_fixed (1);
	coef = _cairo_malloc_ab (col, sizeof (double));
	memset (coef, 0, sizeof (double) * col);
	compute_x_coef_to_double (params, row, col, coef);
	length = col;
    }
    else {
	pixman_params = _cairo_malloc_ab (row + 2, sizeof (double));
 	pixman_params[0] = pixman_int_to_fixed (1);
	pixman_params[1] = pixman_int_to_fixed (row);
	coef = _cairo_malloc_ab (row, sizeof (double));
	memset (coef, 0, sizeof (double) * row);
	compute_y_coef_to_double (params, row, col, coef);
	length = row;
    }

    for (i = 0; i < length; i++)
	pixman_params[i + 2] = pixman_double_to_fixed (coef[i]);

    free (coef);

    return pixman_params;
}

cairo_surface_t * 
_cairo_image_gaussian_filter (cairo_surface_t *src,  const cairo_pattern_t *pattern)
{
    int row, col;
    int width, height;
    int stride;
    pixman_fixed_t *pixman_params;
    pixman_transform_t pixman_transform;
    cairo_int_status_t status;
    cairo_matrix_t matrix;
    int ix = 0;
    int iy = 0;

    pixman_image_t *scratch_images[2];
    cairo_image_surface_t *src_image = (cairo_image_surface_t *)src;
    cairo_image_surface_t *clone_image;
    pixman_image_t *temp_image = NULL;

    int src_width = cairo_image_surface_get_width (src);
    int src_height = cairo_image_surface_get_height (src);
    int i;

    /* clone image, because we don't want to mess with original image
     * transformation
     */
    /* XXX: we need to first scale the image down */
    if (pattern->filter == CAIRO_FILTER_GAUSSIAN &&
        pattern->convolution_matrix) {
	for (i = 0; i < 2; i++)
	    scratch_images[i] = NULL;

	row = pattern->y_radius * 2 + 1;
	col = pattern->x_radius * 2 + 1;
 	width = src_width / pattern->shrink_factor_x;
 	height = src_height / pattern->shrink_factor_y;
	stride = width * (src_image->stride / src_width);
    
	clone_image = (cairo_image_surface_t *)
		cairo_image_surface_create (src_image->format,
					    src_width, src_height);

	if (unlikely (clone_image->base.status)) {
	    cairo_surface_destroy (&clone_image->base);
	    clone_image = (cairo_image_surface_t *)cairo_surface_reference (src);
	    goto DONE;
	}
					
	/* XXX: we must always create a clone because we need to modify
	 * it transformation, no copy data */
	temp_image = pixman_image_create_bits (src_image->pixman_format,
					       src_image->width,
					       src_image->height,
					       (uint32_t *)src_image->data,
					       src_image->stride);
	if (unlikely (temp_image == NULL)) {
	    cairo_surface_destroy (&clone_image->base);
	    clone_image = (cairo_image_surface_t *)cairo_surface_reference (src);
	    goto DONE;
	}
	
	/* create scratch images */
	for (i = 0; i < 2; i++) {
	    scratch_images[i] = pixman_image_create_bits (src_image->pixman_format,
							  width, height,
							  NULL, stride);
	    if (unlikely (scratch_images[i] == NULL)) {
		cairo_surface_destroy (&clone_image->base);
		clone_image = (cairo_image_surface_t *)cairo_surface_reference (src);
		goto DONE;
	    }
	}

	/* if scale, we need to shrink it to scratch 0 */
	/* paint temp to temp_surface */
	if (width != src_width || height != src_height) {
	    pixman_image_set_filter (temp_image, PIXMAN_FILTER_NEAREST, NULL, 0);
	    /* set up transform matrix */
 	    cairo_matrix_init_scale (&matrix, 
				     (double) src_width / (double) width,
				     (double) src_height / (double) height);
	    status = _cairo_matrix_to_pixman_matrix_offset (&matrix,
							    pattern->filter,
							    src_width/2,
							    src_height/2,
							    &pixman_transform,
							    &ix, &iy);
	    if (status == CAIRO_INT_STATUS_NOTHING_TO_DO) {
	    }
	    else if (unlikely (status != CAIRO_INT_STATUS_SUCCESS ||
		       ! pixman_image_set_transform (temp_image,
						     &pixman_transform))) {
		cairo_surface_destroy (&clone_image->base);
		clone_image = (cairo_image_surface_t *)cairo_surface_reference (src);
		goto DONE;
	    }
	    /* set repeat to none */
	    pixman_image_set_repeat (temp_image, PIXMAN_REPEAT_NONE);

	    if (pattern->has_component_alpha)
	        pixman_image_set_component_alpha (temp_image, TRUE);
	    pixman_image_set_filter (temp_image, PIXMAN_FILTER_NEAREST, NULL, 0);
            pixman_image_composite32 (PIXMAN_OP_SRC,
				      temp_image,
				      NULL,
				      scratch_images[0],
				      0, 0,
				      0, 0,
				      0, 0,
				      width, height);
	    pixman_image_unref (temp_image);
	    temp_image = pixman_image_ref (scratch_images[0]);
	}

	/* XXX: begin blur pass */
	/* set up convolution params for x-pass */
	pixman_params =
	    _pixman_image_create_convolution_params (pattern->convolution_matrix, col, row, TRUE);
	pixman_image_set_filter (temp_image, PIXMAN_FILTER_CONVOLUTION,
			   (const pixman_fixed_t *)pixman_params, col + 2);
	free (pixman_params);

	pixman_image_set_repeat (temp_image, PIXMAN_REPEAT_NONE);

	if (pattern->has_component_alpha)
	    pixman_image_set_component_alpha (temp_image, TRUE);

        pixman_image_composite32 (PIXMAN_OP_SRC,
				  temp_image,
				  NULL,
				  scratch_images[1],
				  0, 0,
				  0, 0,
				  0, 0,
				  width, height);

	/* y-pass */
	pixman_params =
	    _pixman_image_create_convolution_params (pattern->convolution_matrix, col, row, FALSE);
	pixman_image_set_filter (scratch_images[1], PIXMAN_FILTER_CONVOLUTION,
			   (const pixman_fixed_t *)pixman_params, row + 2);
	free (pixman_params);

	pixman_image_set_repeat (scratch_images[1], PIXMAN_REPEAT_NONE);

	if (pattern->has_component_alpha)
	    pixman_image_set_component_alpha (scratch_images[1], TRUE);

        pixman_image_composite32 (PIXMAN_OP_SRC,
				  scratch_images[1],
				  NULL,
				  scratch_images[0],
				  0, 0,
				  0, 0,
				  0, 0,
				  width, height);

	/* paint scratch_surfaces[0] to clone */
	/* set up transform matrix */
        cairo_matrix_init_scale (&matrix, 
				 (double) width / (double) src_width,
				 (double) height / (double) src_height);
        status = _cairo_matrix_to_pixman_matrix_offset (&matrix,
						        pattern->filter,
							width/2,
							height/2,
						        &pixman_transform,
							&ix, &iy);
	if (status == CAIRO_INT_STATUS_NOTHING_TO_DO) {
	    /* If the transform is an identity, we don't need to set it */
	}
	else if (unlikely (status != CAIRO_INT_STATUS_SUCCESS ||
		       ! pixman_image_set_transform (scratch_images[0],
						     &pixman_transform))) {
	    cairo_surface_destroy (&clone_image->base);
	    clone_image = (cairo_image_surface_t *)cairo_surface_reference (src);
	    goto DONE;
	}
	/* set repeat to none */
	pixman_image_set_repeat (scratch_images[0], PIXMAN_REPEAT_NONE);

	if (pattern->has_component_alpha)
	    pixman_image_set_component_alpha (scratch_images[0], TRUE);
	pixman_image_set_filter (scratch_images[0], PIXMAN_FILTER_NEAREST, NULL, 0);

        pixman_image_composite32 (PIXMAN_OP_SRC,
				  scratch_images[0],
				  NULL,
				  clone_image->pixman_image,
				  0, 0,
				  0, 0,
				  0, 0,
				  src_width, src_height);
DONE:
	/* free temp surfaces */
        if (temp_image)
	    pixman_image_unref (temp_image);
	for (i = 0; i < 2; i++) {
	    if (scratch_images[i])
		pixman_image_unref (scratch_images[i]);
	}
    }
    else
	clone_image = (cairo_image_surface_t *) cairo_surface_reference (src);

    return &clone_image->base;

}

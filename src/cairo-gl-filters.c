/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2009 Eric Anholt
 * Copyright © 2009 Chris Wilson
 * Copyright © 2005,2010 Red Hat, Inc
 * Copyright © 2011 Intel Corporation
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
 * The Initial Developer of the Original Code is Red Hat, Inc.
 *
 * Contributor(s):
 *	Benjamin Otte <otte@gnome.org>
 *	Carl Worth <cworth@cworth.org>
 *	Chris Wilson <chris@chris-wilson.co.uk>
 *	Eric Anholt <eric@anholt.net>
 */

#include "cairoint.h"

#include "cairo-gl-private.h"
#include "cairo-filters-private.h"

static cairo_int_status_t
_draw_rect (cairo_gl_context_t *ctx,
	    cairo_gl_composite_t *setup,
	    cairo_rectangle_int_t *rect)
{
    cairo_box_t box;
    cairo_point_t quad[4];

    _cairo_box_from_rectangle (&box, rect);
    quad[0].x = box.p1.x;
    quad[0].y = box.p1.y;
    quad[1].x = box.p1.x;
    quad[1].y = box.p2.y;
    quad[2].x = box.p2.x;
    quad[2].y = box.p2.y;
    quad[3].x = box.p2.x;
    quad[3].y = box.p1.y;

    return _cairo_gl_composite_emit_quad_as_tristrip (ctx, setup, quad);
}

/* stage 0 - shrink image */
static cairo_int_status_t
gaussian_filter_stage_0 (cairo_surface_pattern_t *pattern,
			 cairo_gl_surface_t *src,
			 cairo_gl_surface_t *dst,
			 int src_width, int src_height,
			 int dst_width, int dst_height)
{
    cairo_rectangle_int_t rect;
    cairo_clip_t *clip;
    cairo_int_status_t status;

    src->blur_stage = CAIRO_GL_BLUR_STAGE_0;
    _cairo_pattern_init_for_surface (pattern, &src->base);
    
    cairo_matrix_init_scale (&pattern->base.matrix,
			(double) src_width / (double) dst_width,
			(double) src_height / (double) dst_height);

    rect.x = 0;
    rect.y = 0;
    rect.width = dst_width + 1;
    rect.height = dst_height + 1;
    clip = _cairo_clip_intersect_rectangle (NULL, &rect);

    status = _cairo_surface_paint (&dst->base,
                                   CAIRO_OPERATOR_SOURCE,
                                   &pattern->base, clip);

    _cairo_clip_destroy (clip);
    status = _cairo_gl_surface_resolve_multisampling (dst);

    return status;
}

/* x-axis pass to scratches[1] */
static cairo_int_status_t
gaussian_filter_stage_1 (cairo_bool_t x_axis,
			 const cairo_surface_pattern_t *original_pattern,
			 cairo_surface_pattern_t *pattern,
			 cairo_gl_surface_t *src,
			 cairo_gl_surface_t *dst,
			 int dst_width, int dst_height,
			 cairo_gl_context_t **ctx)
{
    int row, col;
    float *coef;
    cairo_gl_composite_t setup;
    cairo_gl_context_t *ctx_out = NULL;
    cairo_rectangle_int_t rect;
    cairo_int_status_t status;

    src->image_content_scale_x = (double) dst_width / (double) src->width;
    src->image_content_scale_y = (double) dst_height / (double) src->height;
    row = original_pattern->base.y_radius * 2 + 1;
    col = original_pattern->base.x_radius * 2 + 1;

    src->blur_stage = CAIRO_GL_BLUR_STAGE_1;
    _cairo_pattern_init_for_surface (pattern, &src->base);
    pattern->base.filter = CAIRO_FILTER_NEAREST;

    if (x_axis) {
	src->operand.type = CAIRO_GL_OPERAND_GAUSSIAN;
	src->operand.pass = 1;

	coef = _cairo_malloc_ab (col, sizeof (float));
	memset (coef, 0, sizeof (float) * col);
	compute_x_coef_to_float (original_pattern->base.convolution_matrix, row, col, coef);

	if (src->operand.texture.coef)
	    free (src->operand.texture.coef);
	src->operand.texture.x_radius = original_pattern->base.x_radius;
	src->operand.texture.y_radius = 1;
	src->operand.texture.coef = coef;
    }
    else {
	src->operand.type = CAIRO_GL_OPERAND_GAUSSIAN;
	src->operand.pass = 2;

	coef = _cairo_malloc_ab (row, sizeof (float));
	memset (coef, 0, sizeof (float) * row);
	compute_y_coef_to_float (original_pattern->base.convolution_matrix, row, col, coef);

	if (src->operand.texture.coef)
	    free (src->operand.texture.coef);
	src->operand.texture.y_radius = original_pattern->base.y_radius;
	src->operand.texture.x_radius = 1;
	src->operand.texture.coef = coef;
    }

    *ctx = ctx_out;
    status = _cairo_gl_composite_init (&setup, CAIRO_OPERATOR_SOURCE,
				      dst, FALSE);
    if (unlikely (status))
	return status;

    pattern->base.extend = CAIRO_EXTEND_NONE;
    status = _cairo_gl_composite_set_source (&setup, &pattern->base,
					     NULL, NULL, FALSE, FALSE);
    if (unlikely (status)) {
	_cairo_gl_composite_fini (&setup);
	return status;
    }

    status = _cairo_gl_composite_begin (&setup, &ctx_out);
    if (unlikely (status)) {
	_cairo_gl_composite_fini (&setup);
	return status;
    }

    rect.x = 0;
    rect.y = 0;
    rect.width = dst_width + 1;
    rect.height = dst_height + 1;
    status = _draw_rect (ctx_out, &setup, &rect);
    _cairo_gl_composite_fini (&setup);
    if (unlikely (status)) {
	status = _cairo_gl_context_release (ctx_out, status);
	return status;
    }

    *ctx = ctx_out;
    _cairo_pattern_fini (&pattern->base);
    return status;
}

/* y-axis pass to scratches[1] */
static cairo_int_status_t
gaussian_filter_stage_2 (cairo_bool_t y_axis,
			 const cairo_surface_pattern_t *original_pattern,
			 cairo_gl_surface_t *stage_1_src,
			 cairo_gl_surface_t *stage_2_src,
			 int dst_width, int dst_height)
{
    cairo_int_status_t status;
    int row, col;
    float *coef;

    stage_2_src->image_content_scale_x = (double) dst_width / (double) stage_1_src->width;
    stage_2_src->image_content_scale_y = (double) dst_height / (double) stage_1_src->height;

    if (y_axis) {
	stage_2_src->operand.type = CAIRO_GL_OPERAND_GAUSSIAN;
	stage_2_src->operand.pass = 2;

	row = original_pattern->base.y_radius * 2 + 1;
	col = original_pattern->base.x_radius * 2 + 1;

	coef = _cairo_malloc_ab (row, sizeof (float));
	memset (coef, 0, sizeof (float) * row);
 	compute_y_coef_to_float (original_pattern->base.convolution_matrix, row, col, coef);
	if (stage_2_src->operand.texture.coef)
	    free (stage_2_src->operand.texture.coef);

	stage_2_src->operand.texture.y_radius = original_pattern->base.y_radius;
	stage_2_src->operand.texture.x_radius = 1;
	stage_2_src->operand.texture.coef = coef;
    }
    else
	stage_2_src->operand.type = CAIRO_GL_OPERAND_TEXTURE;

    stage_2_src->blur_stage = CAIRO_GL_BLUR_STAGE_2;

    status = _cairo_gl_surface_resolve_multisampling (stage_2_src);

    return CAIRO_INT_STATUS_SUCCESS;
}

/* out_extents should be populated by caller, and modified by the
 * function
 */
cairo_gl_surface_t *
_cairo_gl_gaussian_filter (cairo_gl_surface_t *dst,
			   const cairo_surface_pattern_t *pattern,
			   cairo_gl_surface_t *src,
			   cairo_rectangle_int_t *extents_out)
{
    cairo_gl_surface_t *scratches[2];
    int src_width, src_height;
    int width, height;
    int scratch_width, scratch_height, scratch_size;

    cairo_gl_context_t *ctx, *ctx_out;
    cairo_status_t status;
    cairo_bool_t skip_stage_0 = FALSE;
    cairo_gl_operand_type_t saved_type = src->operand.type;

    cairo_surface_pattern_t temp_pattern;
    cairo_bool_t is_source;

    int n;

    if (src->operand.type == CAIRO_GL_OPERAND_GAUSSIAN) {
	extents_out->x = extents_out->y = 0;
        extents_out->width = cairo_gl_surface_get_width (&src->base) * src->image_content_scale_x;
        extents_out->height = cairo_gl_surface_get_height (&src->base) * src->image_content_scale_y;
	return (cairo_gl_surface_t *)cairo_surface_reference (&src->base);
    }

    if (pattern->base.filter != CAIRO_FILTER_GAUSSIAN ||
	! pattern->base.convolution_matrix)
	return (cairo_gl_surface_t *)cairo_surface_reference (&src->base);

    if (! _cairo_gl_surface_is_texture (src))
	return (cairo_gl_surface_t *)cairo_surface_reference (&src->base);

    src_width = cairo_gl_surface_get_width (&src->base);
    src_height = cairo_gl_surface_get_height (&src->base);

    width = src_width / pattern->base.shrink_factor_x;
    height = src_height / pattern->base.shrink_factor_y;

    status = _cairo_gl_context_acquire (dst->base.device, &ctx);
    if (unlikely (status))
	return (cairo_gl_surface_t *)cairo_surface_reference (&src->base);

    for (n = 0; n < 2; n++) {
	if (ctx->source_scratch_in_use) {
	    scratches[n] = ctx->mask_scratch_surfaces[n];
	    is_source = FALSE;
	}
	else {
	    scratches[n] = ctx->source_scratch_surfaces[n];
	    is_source = TRUE;
	}
    
	if (scratches[n]) {
	    scratch_width = cairo_gl_surface_get_width (&scratches[n]->base);
	    scratch_height = cairo_gl_surface_get_height (&scratches[n]->base);

	    if ((scratch_width < width &&
		scratch_width < MAX_SCRATCH_SIZE) ||
		(scratch_height < height &&
		scratch_height < MAX_SCRATCH_SIZE)) {
		cairo_surface_destroy (&scratches[n]->base);
		scratches[n] = NULL;
	    }
	    else if (scratch_width > 4 * width ||
		     scratch_height > 4 * height) {
		cairo_surface_destroy (&scratches[n]->base);
		scratches[n] = NULL;
	    }
	}

	if (! scratches[n]) {
	    scratch_size = MIN_SCRATCH_SIZE;
	    while (scratch_size < width || scratch_size < height) {
		scratch_size *= 2;
		if (scratch_size == MAX_SCRATCH_SIZE)
		    break;
		else if (scratch_size > MAX_SCRATCH_SIZE) {
		    scratch_size *= 0.5;
		    break;
		}
	    }

	    scratches[n] = 
		(cairo_gl_surface_t *)_cairo_gl_surface_create_scratch (ctx,
							dst->base.content,
							scratch_size,
							scratch_size);
	}

	if (ctx->source_scratch_in_use)
	    ctx->mask_scratch_surfaces[n] = scratches[n];
	else
	    ctx->source_scratch_surfaces[n] = scratches[n];

        scratches[n]->needs_to_cache = FALSE;
	scratches[n]->force_no_cache = TRUE;
    }

    if (is_source)
	ctx->source_scratch_in_use = TRUE;

    /* we have created two scratch surfaces */
    /* shrink surface to scratches[0] */
    if ((scratches[0]->width >= src_width && scratches[0]->height >= src_height)) {
	skip_stage_0 = TRUE;
	width = src_width;
	height = src_height;
    }
    else if (width > scratches[0]->width ||
	     height > scratches[0]->height) {
	width = scratches[0]->width;
	height = scratches[0]->height;
    }

    if (! skip_stage_0) {
	status = gaussian_filter_stage_0 (&temp_pattern, src,
					  scratches[0],
					  src_width, src_height,
					  width, height);
	_cairo_pattern_fini (&temp_pattern.base);
	if (unlikely (status))
	    return (cairo_gl_surface_t *)cairo_surface_reference (&src->base);
    }

    /* x-axis pass to scratches[1] */
    if (! skip_stage_0)
	status = gaussian_filter_stage_1 (TRUE, pattern, &temp_pattern,
					  scratches[0], scratches[1],
					  width, height, &ctx_out);
    else {
	status = gaussian_filter_stage_1 (TRUE, pattern, &temp_pattern,
					  src, scratches[1],
					  width, height, &ctx_out);
	src->operand.type = saved_type;
    }
	
    if (ctx_out)
	status = _cairo_gl_context_release (ctx_out, status);
    if (unlikely (status))
	return (cairo_gl_surface_t *)cairo_surface_reference (&src->base);

    /* y-axis pass */
    status = gaussian_filter_stage_1 (FALSE, pattern, &temp_pattern,
				      scratches[1], scratches[0],
				      width, height, &ctx_out);
    if (ctx_out)
	status = _cairo_gl_context_release (ctx_out, status);
    if (unlikely (status))
	return (cairo_gl_surface_t *)cairo_surface_reference (&src->base);

    status = gaussian_filter_stage_2 (FALSE, pattern,
				      scratches[1], scratches[0],
				      width, height);

    extents_out->x = 0;
    extents_out->y = 0;
    extents_out->width = width;
    extents_out->height = height;

    status = _cairo_gl_context_release (ctx, status);

    return (cairo_gl_surface_t *) cairo_surface_reference (&scratches[0]->base);
}

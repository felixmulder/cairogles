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

#include "cairo-composite-rectangles-private.h"
#include "cairo-compositor-private.h"
#include "cairo-default-context-private.h"
#include "cairo-error-private.h"
#include "cairo-image-surface-private.h"
#include "cairo-surface-backend-private.h"
#include "cairo-surface-offset-private.h"
#include "cairo-surface-subsurface-inline.h"
#include "cairo-rtree-private.h"

static cairo_int_status_t
_cairo_gl_create_gradient_texture (cairo_gl_surface_t *dst,
				   const cairo_gradient_pattern_t *pattern,
                                   cairo_gl_gradient_t **gradient)
{
    cairo_gl_context_t *ctx;
    cairo_status_t status;

    status = _cairo_gl_context_acquire (dst->base.device, &ctx);
    if (unlikely (status))
	return status;

    status = _cairo_gl_gradient_create (ctx, pattern->n_stops, pattern->stops, gradient);

    return _cairo_gl_context_release (ctx, status);
}

static void
_cairo_gl_image_cache_lock (cairo_gl_context_t *ctx,
			    cairo_gl_image_t *image_node)
{
    if (ctx->image_cache && ctx->image_cache->surface)
	_cairo_rtree_pin (&ctx->image_cache->rtree, &image_node->node);
}

void
_cairo_gl_image_cache_unlock (cairo_gl_context_t *ctx)
{
    if (ctx->image_cache && ctx->image_cache->surface)
	_cairo_rtree_unpin (&(ctx->image_cache->rtree));
}

static cairo_int_status_t
_cairo_gl_copy_texture (cairo_gl_surface_t *surface,
			cairo_gl_surface_t *dst,
			cairo_gl_surface_t *image,
			int dst_x, int dst_y,
			int src_x, int src_y,
			int width, int height,
			cairo_bool_t replace,
			cairo_gl_context_t **ctx)
{
    cairo_int_status_t status;
    cairo_gl_context_t *ctx_out;
    cairo_gl_dispatch_t *dispatch;
    cairo_gl_surface_t *target;
    cairo_surface_pattern_t pattern;
    cairo_rectangle_int_t rect;
    cairo_clip_t *clip;

    if (! _cairo_gl_surface_is_texture (image))
	return CAIRO_INT_STATUS_UNSUPPORTED;

    status = _cairo_gl_context_acquire (surface->base.device, &ctx_out);
    if(unlikely (status))
	return status;

    if (replace)
	_cairo_gl_composite_flush (ctx_out);

    image->needs_to_cache = FALSE;
    dispatch = &ctx_out->dispatch;
    target = ctx_out->current_target;

    /* paint image to dst */
    _cairo_pattern_init_for_surface (&pattern, &image->base);
    cairo_matrix_init_translate (&pattern.base.matrix, 
				 -dst_x + src_x, -dst_y + src_y);

    rect.x = dst_x;
    rect.y = dst_y;
    rect.width = width;
    rect.height = height;
    clip = _cairo_clip_intersect_rectangle (NULL, &rect);

    status = _cairo_surface_paint (&dst->base,
                                   CAIRO_OPERATOR_SOURCE,
                                   &pattern.base, clip);

    _cairo_clip_destroy (clip);

    _cairo_gl_composite_flush (ctx_out);
    _cairo_pattern_fini (&pattern.base);
    image->needs_to_cache = TRUE;

    if (unlikely (status))
	goto finish;

    status = _cairo_gl_surface_resolve_multisampling (dst);

finish:
    /* restore ctx status */
    if (target)
	_cairo_gl_context_set_destination (ctx_out, target,
					   target->msaa_active);
    *ctx = ctx_out;

    if (unlikely (status))
        return _cairo_gl_context_release (ctx_out, status);
    return status;

}

static void
_cairo_gl_copy_image_cache (cairo_rtree_node_t *node, void *data)
{
    cairo_gl_image_cache_t *new_cache = (cairo_gl_image_cache_t *)data;
    cairo_gl_image_t *image_node = (cairo_gl_image_t *)node;
    cairo_gl_image_t *new_image_node;
    cairo_int_status_t status;
    cairo_rtree_node_t *new_node = NULL;
    int width, height;
    cairo_gl_surface_t *image = (cairo_gl_surface_t *)image_node->original_surface;
    cairo_gl_context_t *ctx = image_node->ctx;

    if (node->state != CAIRO_RTREE_NODE_OCCUPIED || !image)
        return;

    width = image->width;
    height = image->height;

    status = _cairo_rtree_insert (&new_cache->rtree, width,
				  height, &new_node);

    /* because new_cache has larger size, eviction will not happen */
    if (unlikely (status))
    {
        new_cache->copy_success = FALSE;
	return;
    }

    /* Paint image to cache. */
    status = _cairo_gl_copy_texture (new_cache->surface,
				     new_cache->surface,
				     ctx->image_cache->surface,
				     new_node->x, new_node->y,
				     node->x, node->y,
				     width, height,
				     FALSE, &ctx);
    if (unlikely (status)) {
        new_cache->copy_success = FALSE;
	return;
    }

    new_image_node = (cairo_gl_image_t *)new_node;
    new_image_node->ctx = ctx;
    new_image_node->original_surface = &image->base;
    /* Coordinate. */
    new_image_node->p1.x = new_node->x;
    new_image_node->p1.y = new_node->y;
    new_image_node->p2.x = new_node->x + image->width;
    new_image_node->p2.y = new_node->y + image->height;
    if (! _cairo_gl_device_requires_power_of_two_textures (&ctx->base)) {
	new_image_node->p1.x /= new_cache->surface->width;
	new_image_node->p2.x /= new_cache->surface->width;
	new_image_node->p1.y /= new_cache->surface->height;
	new_image_node->p2.y /= new_cache->surface->height;
    }
    image->content_changed = FALSE;

    image_node->original_surface = NULL;

    image->image_node = new_image_node;

    _cairo_gl_image_cache_lock (ctx, new_image_node);
    status = _cairo_gl_context_release (ctx, status);
}

static cairo_int_status_t
_cairo_gl_image_cache_replace_image (cairo_gl_image_t *image_node,
				     cairo_gl_surface_t *dst,
				     cairo_gl_surface_t *cache_surface,
				     cairo_gl_surface_t *image,
				     cairo_gl_context_t **ctx)
{
    cairo_int_status_t status;
    /* Paint image to cache. */
    status = _cairo_gl_copy_texture (dst, cache_surface, 
				     image, image_node->node.x,
				     image_node->node.y,
			  	     0, 0,
				     image->width, image->height,
				     TRUE,
				     ctx);
    image->content_changed = FALSE;
    return status;
}

static cairo_int_status_t
_cairo_gl_image_cache_add_image (cairo_gl_context_t *ctx,
				 cairo_gl_surface_t *dst,
				 cairo_gl_surface_t *image,
				 cairo_gl_image_t **image_node)
{
    cairo_int_status_t status;
    cairo_rtree_node_t *node = NULL;
    int width, height;
    cairo_bool_t replaced = FALSE;
    int image_cache_size;

    if (! image->base.device ||
	(image->width > IMAGE_CACHE_MAX_SIZE ||
	image->height > IMAGE_CACHE_MAX_SIZE))
	return CAIRO_INT_STATUS_UNSUPPORTED;
    else if (! _cairo_gl_surface_is_texture (image))
        return CAIRO_INT_STATUS_UNSUPPORTED;

    width = image->width;
    height = image->height;

    *image_node = image->image_node;

    if (*image_node) {
	if (image->content_changed) {
	    status = _cairo_gl_image_cache_replace_image (*image_node,
							  dst,
							  ctx->image_cache->surface,
							  image, &ctx);

	    if (unlikely (status))
		return status;

	    replaced = TRUE;
	}

	_cairo_gl_image_cache_lock (ctx, *image_node);

	image->content_changed = FALSE;
	if (replaced)
	    return _cairo_gl_context_release (ctx, status);
	return CAIRO_INT_STATUS_SUCCESS;
    }

    if (! ctx->image_cache) {
       status = _cairo_gl_image_cache_init (ctx,
					    MIN_IMAGE_CACHE_WIDTH,
					    MIN_IMAGE_CACHE_HEIGHT,
					    &ctx->image_cache);
       if (unlikely (status))
           return status;
    }

    status = _cairo_rtree_insert (&ctx->image_cache->rtree, width,
				  height, &node);
    /* Search for an unlocked slot. */
    if (status == CAIRO_INT_STATUS_UNSUPPORTED) {
        cairo_gl_image_cache_t *new_cache = NULL;

	_cairo_gl_composite_flush (ctx);

        image_cache_size = ((cairo_gl_surface_t *)(ctx->image_cache->surface))->width;
        if (image_cache_size < MAX_IMAGE_CACHE_WIDTH) {
            image_cache_size *= 2;
            status = _cairo_gl_image_cache_init (ctx,
						 image_cache_size,
                                                 image_cache_size,
						 &new_cache);
	    if (status == CAIRO_INT_STATUS_SUCCESS) {
                /* copy existing image cache to new image cache */
                _cairo_rtree_foreach (&ctx->image_cache->rtree,
                                      _cairo_gl_copy_image_cache,
                                      (void *)new_cache);
                if (new_cache->copy_success) {
		    _cairo_gl_image_cache_fini (ctx);
		    ctx->image_cache = new_cache;
                }
                else {
                    _cairo_rtree_fini (&new_cache->rtree);
                    cairo_surface_destroy (&new_cache->surface->base);
                    free (new_cache);
                    new_cache = NULL;
                    status = CAIRO_INT_STATUS_UNSUPPORTED;
                }
	    }
        }
        if (!new_cache)
	    status = _cairo_rtree_evict_random (&ctx->image_cache->rtree,
						width, height, &node);

	if (status == CAIRO_INT_STATUS_SUCCESS) {
	    if (! node)
	        status = _cairo_rtree_insert (&ctx->image_cache->rtree,
					      width, height, &node);
	    else
	        status = _cairo_rtree_node_insert (&ctx->image_cache->rtree,
					           node, width, height, &node);
	}
    }

    if (unlikely (status))
	return status;

    /* Paint image to cache. */
    status = _cairo_gl_copy_texture (dst, ctx->image_cache->surface,
				     image, node->x, node->y,
				     0, 0,
				     image->width, image->height, 
				     FALSE, &ctx);
    if (unlikely (status))
	return status;

    *image_node = (cairo_gl_image_t *)node;
    (*image_node)->ctx = ctx;
    (*image_node)->original_surface = &image->base;
    /* Coordinate. */
    (*image_node)->p1.x = node->x;
    (*image_node)->p1.y = node->y;
    (*image_node)->p2.x = node->x + image->width;
    (*image_node)->p2.y = node->y + image->height;
    if (! _cairo_gl_device_requires_power_of_two_textures (&ctx->base)) {
	(*image_node)->p1.x /= ctx->image_cache->surface->width;
	(*image_node)->p2.x /= ctx->image_cache->surface->width;
	(*image_node)->p1.y /= ctx->image_cache->surface->height;
	(*image_node)->p2.y /= ctx->image_cache->surface->height;
    }
    image->content_changed = FALSE;

    image->image_node = *image_node;

    _cairo_gl_image_cache_lock (ctx, *image_node);

    return _cairo_gl_context_release (ctx, status);
}

static cairo_status_t
_cairo_gl_subsurface_clone_operand_init (cairo_gl_operand_t *operand,
					 const cairo_pattern_t *_src,
					 cairo_gl_surface_t *dst,
					 const cairo_rectangle_int_t *sample,
					 const cairo_rectangle_int_t *extents,
					 cairo_bool_t use_texgen)
{
    const cairo_surface_pattern_t *src = (cairo_surface_pattern_t *)_src;
    cairo_surface_pattern_t local_pattern;
    cairo_surface_subsurface_t *sub;
    cairo_gl_surface_t *surface;
    cairo_gl_context_t *ctx;
    cairo_surface_attributes_t *attributes;
    cairo_status_t status;

    sub = (cairo_surface_subsurface_t *) src->surface;

    if (sub->snapshot &&
	sub->snapshot->type == CAIRO_SURFACE_TYPE_GL &&
	sub->snapshot->device == dst->base.device)
    {
	surface = (cairo_gl_surface_t *)
	    cairo_surface_reference (sub->snapshot);
    }
    else
    {
	status = _cairo_gl_context_acquire (dst->base.device, &ctx);
	if (unlikely (status))
	    return status;

	/* XXX Trim surface to the sample area within the subsurface? */
	surface = (cairo_gl_surface_t *)
	    _cairo_gl_surface_create_scratch (ctx,
					      sub->target->content,
					      sub->extents.width,
					      sub->extents.height);
	if (surface->base.status)
	    return _cairo_gl_context_release (ctx, surface->base.status);

	_cairo_pattern_init_for_surface (&local_pattern, sub->target);
	cairo_matrix_init_translate (&local_pattern.base.matrix,
				     sub->extents.x, sub->extents.y);
	local_pattern.base.filter = CAIRO_FILTER_NEAREST;
	status = _cairo_surface_paint (&surface->base,
				       CAIRO_OPERATOR_SOURCE,
				       &local_pattern.base,
				       NULL);
	_cairo_pattern_fini (&local_pattern.base);

	status = _cairo_gl_context_release (ctx, status);
	if (unlikely (status)) {
	    cairo_surface_destroy (&surface->base);
	    return status;
	}

	_cairo_surface_subsurface_set_snapshot (&sub->base, &surface->base);
    }

    status = _cairo_gl_surface_resolve_multisampling (surface);
    if (unlikely (status))
        return status;

    attributes = &operand->texture.attributes;

    operand->type = CAIRO_GL_OPERAND_TEXTURE;
    operand->texture.surface = surface;
    operand->texture.owns_surface = surface;
    operand->texture.tex = surface->tex;
    operand->texture.use_atlas = FALSE;

    if (_cairo_gl_device_requires_power_of_two_textures (dst->base.device)) {
	attributes->matrix = src->base.matrix;
    } else {
	cairo_matrix_t m;

	cairo_matrix_init_scale (&m,
				 1.0 / surface->width,
				 1.0 / surface->height);
	cairo_matrix_multiply (&attributes->matrix, &src->base.matrix, &m);
    }

    attributes->extend = src->base.extend;
    attributes->filter = src->base.filter;
    attributes->has_component_alpha = src->base.has_component_alpha;

    operand->texture.texgen = use_texgen;
    return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
_cairo_gl_subsurface_operand_init (cairo_gl_operand_t *operand,
				   const cairo_pattern_t *_src,
				   cairo_gl_surface_t *dst,
				   const cairo_rectangle_int_t *sample,
				   const cairo_rectangle_int_t *extents,
				   cairo_bool_t use_texgen)
{
    const cairo_surface_pattern_t *src = (cairo_surface_pattern_t *)_src;
    cairo_surface_subsurface_t *sub;
    cairo_gl_surface_t *surface, *blur_surface;
    cairo_surface_attributes_t *attributes;
    cairo_int_status_t status;
    cairo_gl_image_t *image_node = NULL;
    cairo_gl_context_t *ctx = (cairo_gl_context_t *)dst->base.device;
    cairo_bool_t ctx_acquired = FALSE;
    cairo_rectangle_int_t blur_extents;

    sub = (cairo_surface_subsurface_t *) src->surface;

    if (sample->x < 0 || sample->y < 0 ||
	sample->x + sample->width  > sub->extents.width ||
	sample->y + sample->height > sub->extents.height)
    {
	return _cairo_gl_subsurface_clone_operand_init (operand, _src,
							dst, sample, extents,
							use_texgen);
    }

    surface = (cairo_gl_surface_t *) sub->target;
    if (surface->base.device && (surface->base.device != dst->base.device ||
         (! _cairo_gl_surface_is_texture (surface) && ! surface->bounded_tex)))
	return CAIRO_INT_STATUS_UNSUPPORTED;

    status = _cairo_gl_surface_resolve_multisampling (surface);
    if (unlikely (status))
	return status;

    blur_extents.x = blur_extents.y = 0;
    blur_extents.width = cairo_gl_surface_get_height (&surface->base);
    blur_extents.height = cairo_gl_surface_get_height (&surface->base);

    blur_surface = _cairo_gl_gaussian_filter (dst, src, surface, &blur_extents);

    _cairo_gl_operand_copy(operand, &surface->operand);
    *operand = surface->operand;
    operand->texture.use_atlas = FALSE;

    attributes = &operand->texture.attributes;
    attributes->extend = src->base.extend;
    attributes->filter = src->base.filter;
    attributes->has_component_alpha = src->base.has_component_alpha;

    attributes->matrix = src->base.matrix;
    attributes->matrix.x0 += sub->extents.x;
    attributes->matrix.y0 += sub->extents.y;

    operand->texture.texgen = use_texgen;


    if (blur_surface == surface && 
	surface->needs_to_cache &&
	surface->base.device) {
        status = _cairo_gl_context_acquire (dst->base.device, &ctx);
        if (status == CAIRO_INT_STATUS_SUCCESS) {
	    ctx_acquired = TRUE;
	    status = _cairo_gl_image_cache_add_image (ctx, dst, surface,
						      &image_node);
        }
    }

    /* Translate the matrix from
     * (unnormalized src -> unnormalized src) to
     * (unnormalized dst -> unnormalized src)
     */

    if (unlikely (status) || ! image_node) {
	if (blur_surface == surface &&
	     blur_surface->operand.type != CAIRO_GL_OPERAND_GAUSSIAN) {
	cairo_matrix_multiply (&attributes->matrix,
			       &attributes->matrix,
			       &surface->operand.texture.attributes.matrix);
	}
	else {
	    cairo_matrix_t matrix = src->base.matrix;
	    operand->texture.use_atlas = TRUE;
	    attributes->extend = CAIRO_EXTEND_NONE;
	    operand->texture.extend = src->base.extend;

	    operand->texture.p1.x = 0;
	    operand->texture.p1.y = 0;
	    operand->texture.p2.x = (double) blur_extents.width / (double) blur_surface->width;
	    operand->texture.p2.y = (double) blur_extents.height / (double) blur_surface->height;
	    if (src->base.extend == CAIRO_EXTEND_PAD) {
		operand->texture.p1.x += 0.5 / blur_surface->width;
		operand->texture.p1.y += 0.5 / blur_surface->height;
		operand->texture.p2.x -= 0.5 / blur_surface->width;
		operand->texture.p2.y -= 0.5 / blur_surface->height;
	    }

	    operand->texture.surface = blur_surface;
	    operand->texture.owns_surface = NULL;
	    operand->texture.tex = blur_surface->tex;
	    if (blur_surface->blur_stage == CAIRO_GL_BLUR_STAGE_2)
		cairo_matrix_scale (&attributes->matrix,
			       (double)blur_extents.width/(double)surface->width,
			       (double)blur_extents.height/(double)surface->height);
	    cairo_matrix_multiply (&attributes->matrix,
			           &matrix,
				   &attributes->matrix);

	}
   }
   else {
	cairo_matrix_t matrix = src->base.matrix;
	operand->texture.surface = ctx->image_cache->surface;
	operand->texture.owns_surface = NULL;
	operand->texture.tex = ctx->image_cache->surface->tex;
	attributes->extend = CAIRO_EXTEND_NONE;
	operand->texture.extend = src->base.extend;
	attributes->matrix.x0 = image_node->node.x + sub->extents.x;
	attributes->matrix.y0 = image_node->node.y + sub->extents.y;
	operand->texture.use_atlas = TRUE;

	operand->texture.p1.x = image_node->p1.x;
	operand->texture.p1.y = image_node->p1.y;
	operand->texture.p2.x = image_node->p2.x;
	operand->texture.p2.y = image_node->p2.y;
	if (src->base.extend == CAIRO_EXTEND_PAD) {
	    operand->texture.p1.x += 0.5 / ctx->image_cache->surface->width;
	    operand->texture.p1.y += 0.5 / ctx->image_cache->surface->height;
	    operand->texture.p2.x -= 0.5 / ctx->image_cache->surface->width;
	    operand->texture.p2.y -= 0.5 / ctx->image_cache->surface->height;
	}

	cairo_matrix_multiply (&attributes->matrix,
			       &matrix,
			       &ctx->image_cache->surface->operand.texture.attributes.matrix);
    }
    cairo_surface_destroy (&blur_surface->base);

    if (ctx_acquired)
	return _cairo_gl_context_release (ctx, status);

    return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
_cairo_gl_surface_operand_init (cairo_gl_operand_t *operand,
				const cairo_pattern_t *_src,
				cairo_gl_surface_t *dst,
				const cairo_rectangle_int_t *sample,
				const cairo_rectangle_int_t *extents,
				cairo_bool_t use_texgen)
{
    const cairo_surface_pattern_t *src = (cairo_surface_pattern_t *)_src;
    cairo_gl_surface_t *surface, *blur_surface;
    cairo_surface_attributes_t *attributes;
    cairo_int_status_t status;
    cairo_gl_image_t *image_node = NULL;
    cairo_gl_context_t *ctx = (cairo_gl_context_t *)dst->base.device;
    cairo_bool_t ctx_acquired = FALSE;
    cairo_rectangle_int_t blur_extents;

    surface = (cairo_gl_surface_t *) src->surface;
    if (surface->base.type != CAIRO_SURFACE_TYPE_GL)
	return CAIRO_INT_STATUS_UNSUPPORTED;

    if (surface->base.backend->type != CAIRO_SURFACE_TYPE_GL) {
	if (_cairo_surface_is_subsurface (&surface->base))
	    return _cairo_gl_subsurface_operand_init (operand, _src, dst,
						      sample, extents,
						      use_texgen);

	return CAIRO_INT_STATUS_UNSUPPORTED;
    }

    if (surface->base.device && surface->base.device != dst->base.device)
	return CAIRO_INT_STATUS_UNSUPPORTED;

    if (surface->base.device && ! _cairo_gl_surface_is_texture (surface) &&
	! surface->bounded_tex)
	return CAIRO_INT_STATUS_UNSUPPORTED;

    status = _cairo_gl_surface_resolve_multisampling (surface);
    if (unlikely (status))
	return status;

    blur_extents.x = blur_extents.y = 0;
    blur_extents.width = cairo_gl_surface_get_height (&surface->base);
    blur_extents.height = cairo_gl_surface_get_height (&surface->base);

    blur_surface = _cairo_gl_gaussian_filter (dst, src, surface, &blur_extents);

    _cairo_gl_operand_copy(operand, &blur_surface->operand);
    operand->texture.use_atlas = FALSE;

    attributes = &operand->texture.attributes;
    attributes->extend = src->base.extend;
    attributes->filter = src->base.filter;
    attributes->has_component_alpha = src->base.has_component_alpha;

    operand->texture.texgen = use_texgen;

    if (blur_surface == surface && 
	surface->needs_to_cache &&
	surface->base.device) {
        status = _cairo_gl_context_acquire (dst->base.device, &ctx);
        if (status == CAIRO_INT_STATUS_SUCCESS) {
            ctx_acquired = TRUE;
	    status = _cairo_gl_image_cache_add_image (ctx, dst, surface,
						      &image_node);
	}
    }

    if (unlikely (status) || ! image_node) {
	if (blur_surface == surface &&
	     blur_surface->operand.type != CAIRO_GL_OPERAND_GAUSSIAN) {
	    cairo_matrix_multiply (&attributes->matrix,
				   &src->base.matrix,
				   &attributes->matrix);
	}
	else {
	    cairo_matrix_t matrix = src->base.matrix;
	    operand->texture.use_atlas = TRUE;
	    attributes->extend = CAIRO_EXTEND_NONE;
	    operand->texture.extend = src->base.extend;

	    operand->texture.p1.x = 0;
	    operand->texture.p1.y = 0;
	    operand->texture.p2.x = (double) blur_extents.width / (double) blur_surface->width;
	    operand->texture.p2.y = (double) blur_extents.height / (double) blur_surface->height;
	    if (src->base.extend == CAIRO_EXTEND_PAD) {
		operand->texture.p1.x += 0.5 / blur_surface->width;
		operand->texture.p1.y += 0.5 / blur_surface->height;
		operand->texture.p2.x -= 0.5 / blur_surface->width;
		operand->texture.p2.y -= 0.5 / blur_surface->height;
	    }

	    operand->texture.surface = blur_surface;
	    operand->texture.owns_surface = NULL;
	    operand->texture.tex = blur_surface->tex;
	    if (blur_surface->blur_stage == CAIRO_GL_BLUR_STAGE_2)
		cairo_matrix_scale (&attributes->matrix,
			       (double)blur_extents.width/(double)surface->width,
			       (double)blur_extents.height/(double)surface->height);
	    cairo_matrix_multiply (&attributes->matrix,
			           &matrix,
				   &attributes->matrix);

	}
    }
    else {
	cairo_matrix_t matrix = src->base.matrix;
	operand->texture.use_atlas = TRUE;
	attributes->extend = CAIRO_EXTEND_NONE;
	operand->texture.extend = src->base.extend;

	operand->texture.p1.x = image_node->p1.x;
	operand->texture.p1.y = image_node->p1.y;
	operand->texture.p2.x = image_node->p2.x;
	operand->texture.p2.y = image_node->p2.y;
	if (src->base.extend == CAIRO_EXTEND_PAD) {
	    operand->texture.p1.x += 0.5 / ctx->image_cache->surface->width;
	    operand->texture.p1.y += 0.5 / ctx->image_cache->surface->height;
	    operand->texture.p2.x -= 0.5 / ctx->image_cache->surface->width;
	    operand->texture.p2.y -= 0.5 / ctx->image_cache->surface->height;
	}

	operand->texture.surface = ctx->image_cache->surface;
	operand->texture.owns_surface = NULL;
	operand->texture.tex = ctx->image_cache->surface->tex;
	matrix.x0 += image_node->node.x;
	matrix.y0 += image_node->node.y;
	cairo_matrix_multiply (&attributes->matrix,
			       &matrix,
			       &ctx->image_cache->surface->operand.texture.attributes.matrix);
    }

    cairo_surface_destroy (&blur_surface->base);

    if (ctx_acquired)
	return _cairo_gl_context_release (ctx, status);

    return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
_cairo_gl_pattern_texture_setup (cairo_gl_operand_t *operand,
				 const cairo_pattern_t *_src,
				 cairo_gl_surface_t *dst,
				 const cairo_rectangle_int_t *extents)
{
    cairo_status_t status;
    cairo_gl_surface_t *surface;
    cairo_gl_context_t *ctx;
    cairo_image_surface_t *image;
    cairo_bool_t src_is_gl_surface = FALSE;
    cairo_rectangle_int_t map_extents;

    if (_src->type == CAIRO_PATTERN_TYPE_SURFACE) {
	cairo_surface_t* src_surface = ((cairo_surface_pattern_t *) _src)->surface;
	src_is_gl_surface = src_surface->type == CAIRO_SURFACE_TYPE_GL;
    }

    status = _cairo_gl_context_acquire (dst->base.device, &ctx);
    if (unlikely (status))
	return status;

    surface = (cairo_gl_surface_t *)
	_cairo_gl_surface_create_scratch (ctx,
					  CAIRO_CONTENT_COLOR_ALPHA,
					  extents->width, extents->height);
    map_extents = *extents;
    map_extents.x = map_extents.y = 0;
    image = _cairo_surface_map_to_image (&surface->base, &map_extents);

    /* If the pattern is a GL surface, it belongs to some other GL context,
       so we need to release this device while we paint it to the image. */
    if (src_is_gl_surface) {
	status = _cairo_gl_context_release (ctx, status);
	if (unlikely (status))
	    goto fail;
    }

    status = _cairo_surface_offset_paint (&image->base, extents->x, extents->y,
					  CAIRO_OPERATOR_SOURCE, _src, NULL);

    if (src_is_gl_surface) {
	status = _cairo_gl_context_acquire (dst->base.device, &ctx);
	if (unlikely (status))
	    goto fail;
    }

    status = _cairo_surface_unmap_image (&surface->base, image);
    status = _cairo_gl_context_release (ctx, status);
    if (unlikely (status))
	goto fail;

    *operand = surface->operand;
    operand->texture.owns_surface = surface;
    operand->texture.attributes.matrix.x0 -= extents->x * operand->texture.attributes.matrix.xx;
    operand->texture.attributes.matrix.y0 -= extents->y * operand->texture.attributes.matrix.yy;

    if (_cairo_gl_surface_is_texture (dst) && 
        dst->width <= IMAGE_CACHE_MAX_SIZE &&
        dst->height <= IMAGE_CACHE_MAX_SIZE &&
	! dst->force_no_cache)
        dst->needs_to_cache = TRUE;

    operand->texture.use_atlas = FALSE;

    return CAIRO_STATUS_SUCCESS;

fail:
    cairo_surface_destroy (&surface->base);
    return status;
}

void
_cairo_gl_solid_operand_init (cairo_gl_operand_t *operand,
			      const cairo_color_t *color)
{
    operand->type = CAIRO_GL_OPERAND_CONSTANT;
    operand->constant.color[0] = color->red   * color->alpha;
    operand->constant.color[1] = color->green * color->alpha;
    operand->constant.color[2] = color->blue  * color->alpha;
    operand->constant.color[3] = color->alpha;
}

void
_cairo_gl_operand_translate (cairo_gl_operand_t *operand,
			     double tx, double ty)
{
    switch (operand->type) {
    case CAIRO_GL_OPERAND_TEXTURE:
    case CAIRO_GL_OPERAND_GAUSSIAN:
	operand->texture.attributes.matrix.x0 -= tx * operand->texture.attributes.matrix.xx;
	operand->texture.attributes.matrix.y0 -= ty * operand->texture.attributes.matrix.yy;
	break;

    case CAIRO_GL_OPERAND_LINEAR_GRADIENT:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_A0:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_NONE:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_EXT:
	operand->gradient.m.x0 -= tx * operand->gradient.m.xx;
	operand->gradient.m.y0 -= ty * operand->gradient.m.yy;
	break;

    case CAIRO_GL_OPERAND_NONE:
    case CAIRO_GL_OPERAND_CONSTANT:
    case CAIRO_GL_OPERAND_COUNT:
    default:
	break;
    }
}

static cairo_status_t
_cairo_gl_gradient_operand_init (cairo_gl_operand_t *operand,
                                 const cairo_pattern_t *pattern,
				 cairo_gl_surface_t *dst,
				 cairo_bool_t use_texgen)
{
    const cairo_gradient_pattern_t *gradient = (const cairo_gradient_pattern_t *)pattern;
    cairo_status_t status;

    assert (gradient->base.type == CAIRO_PATTERN_TYPE_LINEAR ||
	    gradient->base.type == CAIRO_PATTERN_TYPE_RADIAL);

    if (! _cairo_gl_device_has_glsl (dst->base.device))
	return CAIRO_INT_STATUS_UNSUPPORTED;

    status = _cairo_gl_create_gradient_texture (dst,
						gradient,
						&operand->gradient.gradient);
    if (unlikely (status))
	return status;

    if (gradient->base.type == CAIRO_PATTERN_TYPE_LINEAR) {
	cairo_linear_pattern_t *linear = (cairo_linear_pattern_t *) gradient;
	double x0, y0, dx, dy, sf, offset;

	dx = linear->pd2.x - linear->pd1.x;
	dy = linear->pd2.y - linear->pd1.y;
	sf = 1.0 / (dx * dx + dy * dy);
	dx *= sf;
	dy *= sf;

	x0 = linear->pd1.x;
	y0 = linear->pd1.y;
	offset = dx * x0 + dy * y0;

	operand->type = CAIRO_GL_OPERAND_LINEAR_GRADIENT;

	cairo_matrix_init (&operand->gradient.m, dx, 0, dy, 1, -offset, 0);
	if (! _cairo_matrix_is_identity (&pattern->matrix)) {
	    cairo_matrix_multiply (&operand->gradient.m,
				   &pattern->matrix,
				   &operand->gradient.m);
	}
    } else {
	cairo_matrix_t m;
	cairo_circle_double_t circles[2];
	double x0, y0, r0, dx, dy, dr;
	double scale = 1.0;
	cairo_radial_pattern_t *radial_pattern = (cairo_radial_pattern_t *)gradient;

	/*
	 * Some fragment shader implementations use half-floats to
	 * represent numbers, so the maximum number they can represent
	 * is about 2^14. Some intermediate computations used in the
	 * radial gradient shaders can produce results of up to 2*k^4.
	 * Setting k=8 makes the maximum result about 8192 (assuming
	 * that the extreme circles are not much smaller than the
	 * destination image).
	 */
	_cairo_gradient_pattern_fit_to_range (gradient, 8.,
					      &operand->gradient.m, circles);

	/*
	 * Instead of using scaled data that might introducing rounding
	 * errors, we use original data directly
	 */
	if (circles[0].center.x)
		scale = radial_pattern->cd1.center.x / circles[0].center.x;
	else if (circles[0].center.y)
		scale = radial_pattern->cd1.center.y / circles[0].center.y;
	else if (circles[0].radius)
		scale = radial_pattern->cd1.radius / circles[0].radius;
	else if (circles[1].center.x)
		scale = radial_pattern->cd2.center.x / circles[1].center.x;
	else if (circles[1].center.y)
		scale = radial_pattern->cd2.center.y / circles[1].center.y;
	else if (circles[1].radius)
		scale = radial_pattern->cd2.radius / circles[1].radius;

	x0 = circles[0].center.x;
	y0 = circles[0].center.y;
	r0 = circles[0].radius;
	dx = radial_pattern->cd2.center.x - radial_pattern->cd1.center.x;
	dy = radial_pattern->cd2.center.y - radial_pattern->cd1.center.y;
	dr = radial_pattern->cd2.radius	  - radial_pattern->cd1.radius;

	operand->gradient.a = (dx * dx + dy * dy - dr * dr)/(scale * scale);
	operand->gradient.radius_0 = r0;
	operand->gradient.circle_d.center.x = dx / scale;
	operand->gradient.circle_d.center.y = dy / scale;
	operand->gradient.circle_d.radius	= dr / scale;

	if (operand->gradient.a == 0)
	    operand->type = CAIRO_GL_OPERAND_RADIAL_GRADIENT_A0;
	else if (pattern->extend == CAIRO_EXTEND_NONE)
	    operand->type = CAIRO_GL_OPERAND_RADIAL_GRADIENT_NONE;
	else
	    operand->type = CAIRO_GL_OPERAND_RADIAL_GRADIENT_EXT;

	cairo_matrix_init_translate (&m, -x0, -y0);
	cairo_matrix_multiply (&operand->gradient.m,
			       &operand->gradient.m,
			       &m);
    }

    operand->gradient.extend = pattern->extend;
    operand->gradient.texgen = use_texgen;

    return CAIRO_STATUS_SUCCESS;
}

void
_cairo_gl_operand_copy (cairo_gl_operand_t *dst,
			const cairo_gl_operand_t *src)
{
    *dst = *src;
    switch (dst->type) {
    case CAIRO_GL_OPERAND_CONSTANT:
	break;
    case CAIRO_GL_OPERAND_LINEAR_GRADIENT:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_A0:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_NONE:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_EXT:
	_cairo_gl_gradient_reference (dst->gradient.gradient);
	break;
    case CAIRO_GL_OPERAND_TEXTURE:
    case CAIRO_GL_OPERAND_GAUSSIAN:
	cairo_surface_reference (&dst->texture.owns_surface->base);
	break;
    default:
    case CAIRO_GL_OPERAND_COUNT:
        ASSERT_NOT_REACHED;
    case CAIRO_GL_OPERAND_NONE:
        break;
    }
}

void
_cairo_gl_operand_destroy (cairo_gl_operand_t *operand)
{
    switch (operand->type) {
    case CAIRO_GL_OPERAND_CONSTANT:
	break;
    case CAIRO_GL_OPERAND_LINEAR_GRADIENT:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_A0:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_NONE:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_EXT:
	_cairo_gl_gradient_destroy (operand->gradient.gradient);
	break;
    case CAIRO_GL_OPERAND_TEXTURE:
	cairo_surface_destroy (&operand->texture.owns_surface->base);
	break;
    case CAIRO_GL_OPERAND_GAUSSIAN:
	cairo_surface_destroy (&operand->texture.owns_surface->base);
	break;
    default:
    case CAIRO_GL_OPERAND_COUNT:
        ASSERT_NOT_REACHED;
    case CAIRO_GL_OPERAND_NONE:
        break;
    }

    operand->type = CAIRO_GL_OPERAND_NONE;
}

cairo_int_status_t
_cairo_gl_operand_init (cairo_gl_operand_t *operand,
		        const cairo_pattern_t *pattern,
		        cairo_gl_surface_t *dst,
			const cairo_rectangle_int_t *sample,
			const cairo_rectangle_int_t *extents,
			cairo_bool_t use_texgen,
			cairo_bool_t encode_color_as_attribute)
{
    cairo_int_status_t status;

    TRACE ((stderr, "%s: type=%d\n", __FUNCTION__, pattern->type));
    switch (pattern->type) {
    case CAIRO_PATTERN_TYPE_SOLID:
	_cairo_gl_solid_operand_init (operand,
				      &((cairo_solid_pattern_t *) pattern)->color);
	operand->constant.encode_as_attribute = encode_color_as_attribute;
	return CAIRO_STATUS_SUCCESS;
    case CAIRO_PATTERN_TYPE_SURFACE:
	status = _cairo_gl_surface_operand_init (operand, pattern, dst,
						 sample, extents, use_texgen);
	if (status == CAIRO_INT_STATUS_UNSUPPORTED)
	    break;

	return status;

    case CAIRO_PATTERN_TYPE_LINEAR:
    case CAIRO_PATTERN_TYPE_RADIAL:
	status = _cairo_gl_gradient_operand_init (operand, pattern, dst,
						  use_texgen);
	if (status == CAIRO_INT_STATUS_UNSUPPORTED)
	    break;

	return status;

    default:
    case CAIRO_PATTERN_TYPE_MESH:
    case CAIRO_PATTERN_TYPE_RASTER_SOURCE:
	break;
    }

    return _cairo_gl_pattern_texture_setup (operand, pattern, dst, extents);
}

cairo_filter_t
_cairo_gl_operand_get_filter (cairo_gl_operand_t *operand)
{
    cairo_filter_t filter;

    switch ((int) operand->type) {
    case CAIRO_GL_OPERAND_TEXTURE:
	filter = operand->texture.attributes.filter;
	break;
    case CAIRO_GL_OPERAND_LINEAR_GRADIENT:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_A0:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_NONE:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_EXT:
    case CAIRO_GL_OPERAND_GAUSSIAN:
	filter = CAIRO_FILTER_BILINEAR;
	break;
    default:
	filter = CAIRO_FILTER_DEFAULT;
	break;
    }

    return filter;
}

GLint
_cairo_gl_operand_get_gl_filter (cairo_gl_operand_t *operand)
{
    cairo_filter_t filter = _cairo_gl_operand_get_filter (operand);

    if (filter == CAIRO_FILTER_GAUSSIAN)
	return GL_LINEAR;

    return filter != CAIRO_FILTER_FAST && filter != CAIRO_FILTER_NEAREST ?
	   GL_LINEAR :
	   GL_NEAREST;
}

cairo_bool_t
_cairo_gl_operand_get_use_atlas (cairo_gl_operand_t *operand)
{
    if (operand->type != CAIRO_GL_OPERAND_TEXTURE && 
	operand->type != CAIRO_GL_OPERAND_GAUSSIAN)
	return FALSE;

    return operand->texture.use_atlas;
}

cairo_extend_t
_cairo_gl_operand_get_extend (cairo_gl_operand_t *operand)
{
    cairo_extend_t extend;

    switch ((int) operand->type) {
    case CAIRO_GL_OPERAND_TEXTURE:
    case CAIRO_GL_OPERAND_GAUSSIAN:
	if (! operand->texture.use_atlas)
	    extend = operand->texture.attributes.extend;
	else
	    extend = operand->texture.extend;
	break;
    case CAIRO_GL_OPERAND_LINEAR_GRADIENT:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_A0:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_NONE:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_EXT:
	extend = operand->gradient.extend;
	break;
    default:
	extend = CAIRO_EXTEND_NONE;
	break;
    }

    return extend;
}

cairo_extend_t
_cairo_gl_operand_get_atlas_extend (cairo_gl_operand_t *operand)
{
    cairo_extend_t extend;

    switch ((int) operand->type) {
    case CAIRO_GL_OPERAND_TEXTURE:
    case CAIRO_GL_OPERAND_GAUSSIAN:
	if (operand->texture.use_atlas)
	    extend = operand->texture.extend;
	else
	    extend = CAIRO_EXTEND_NONE;
	break;
    default:
	extend = CAIRO_EXTEND_NONE;
	break;
    }

    return extend;
}

void
_cairo_gl_operand_bind_to_shader (cairo_gl_context_t *ctx,
                                  cairo_gl_operand_t *operand,
                                  cairo_gl_tex_t      tex_unit)
{
    const cairo_matrix_t *texgen = NULL;

    switch (operand->type) {
    default:
    case CAIRO_GL_OPERAND_COUNT:
        ASSERT_NOT_REACHED;
    case CAIRO_GL_OPERAND_NONE:
	return;

    case CAIRO_GL_OPERAND_CONSTANT:
	if (operand->constant.encode_as_attribute)
	    return;

	_cairo_gl_shader_bind_vec4 (ctx,
				    _cairo_gl_shader_uniform_for_texunit (
					CAIRO_GL_UNIFORM_CONSTANT, tex_unit),
				    operand->constant.color[0],
				    operand->constant.color[1],
				    operand->constant.color[2],
				    operand->constant.color[3]);
        return;
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_NONE:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_EXT:
	_cairo_gl_shader_bind_float  (ctx,
				      _cairo_gl_shader_uniform_for_texunit (
					  CAIRO_GL_UNIFORM_A, tex_unit),
				      operand->gradient.a);
	/* fall through */
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_A0:
	_cairo_gl_shader_bind_vec3 (ctx,
				    _cairo_gl_shader_uniform_for_texunit (
					CAIRO_GL_UNIFORM_CIRCLE_D, tex_unit),
				      operand->gradient.circle_d.center.x,
				      operand->gradient.circle_d.center.y,
				      operand->gradient.circle_d.radius);
	_cairo_gl_shader_bind_float  (ctx,
				      _cairo_gl_shader_uniform_for_texunit (
					  CAIRO_GL_UNIFORM_RADIUS_0, tex_unit),
				      operand->gradient.radius_0);
        /* fall through */
    case CAIRO_GL_OPERAND_LINEAR_GRADIENT:
    case CAIRO_GL_OPERAND_TEXTURE:
    case CAIRO_GL_OPERAND_GAUSSIAN:
	/*
	 * For GLES2 we use shaders to implement GL_CLAMP_TO_BORDER (used
	 * with CAIRO_EXTEND_NONE). When bilinear filtering is enabled,
	 * these shaders need the texture dimensions for their calculations.
	 */
	if (ctx->gl_flavor == CAIRO_GL_FLAVOR_ES &&
	    _cairo_gl_operand_get_extend (operand) == CAIRO_EXTEND_NONE &&
	    _cairo_gl_operand_get_gl_filter (operand) == GL_LINEAR)
	{
	    float width, height;
	    if (operand->type == CAIRO_GL_OPERAND_TEXTURE ||
		operand->type == CAIRO_GL_OPERAND_GAUSSIAN) {
		width = operand->texture.surface->width;
		height = operand->texture.surface->height;
	    }
	    else {
		width = operand->gradient.gradient->cache_entry.size,
		height = 1;
	    }
	    if (operand->type != CAIRO_GL_OPERAND_GAUSSIAN)
		_cairo_gl_shader_bind_vec2 (ctx,
					    _cairo_gl_shader_uniform_for_texunit (
						CAIRO_GL_UNIFORM_TEXDIMS, tex_unit),
					    width, height);
	}

	break;
    }

    if (operand->type == CAIRO_GL_OPERAND_GAUSSIAN &&
	operand->pass == 1) {
	float x_axis = 1.0;
	float y_axis = 0.0;
	_cairo_gl_shader_bind_float (ctx,
				    _cairo_gl_shader_uniform_for_texunit (
					    CAIRO_GL_UNIFORM_BLUR_X_AXIS, tex_unit),
				    x_axis);
	_cairo_gl_shader_bind_float (ctx,
				    _cairo_gl_shader_uniform_for_texunit (
					    CAIRO_GL_UNIFORM_BLUR_Y_AXIS, tex_unit),
				    y_axis);
	_cairo_gl_shader_bind_int (ctx,
				    _cairo_gl_shader_uniform_for_texunit (
					    CAIRO_GL_UNIFORM_BLUR_RADIUS, tex_unit),
				    operand->texture.x_radius);

	_cairo_gl_shader_bind_float (ctx,
				    _cairo_gl_shader_uniform_for_texunit (
					    CAIRO_GL_UNIFORM_BLUR_STEP, tex_unit),
				    1.0 / cairo_gl_surface_get_width (&operand->texture.surface->base));

	_cairo_gl_shader_bind_float_array (ctx,
				    _cairo_gl_shader_uniform_for_texunit (
					    CAIRO_GL_UNIFORM_BLURS, tex_unit),
				    operand->texture.x_radius * 2 + 1,
				    operand->texture.coef);
    }
    else if (operand->type == CAIRO_GL_OPERAND_GAUSSIAN &&
	     operand->pass == 2) {
	float x_axis = 0.0;
	float y_axis = 1.0;
	_cairo_gl_shader_bind_float (ctx,
				    _cairo_gl_shader_uniform_for_texunit (
					    CAIRO_GL_UNIFORM_BLUR_X_AXIS, tex_unit),
				    x_axis);
	_cairo_gl_shader_bind_float (ctx,
				    _cairo_gl_shader_uniform_for_texunit (
					    CAIRO_GL_UNIFORM_BLUR_Y_AXIS, tex_unit),
				    y_axis);
	_cairo_gl_shader_bind_int (ctx,
				    _cairo_gl_shader_uniform_for_texunit (
					    CAIRO_GL_UNIFORM_BLUR_RADIUS, tex_unit),
				    operand->texture.y_radius);

	_cairo_gl_shader_bind_float (ctx,
				    _cairo_gl_shader_uniform_for_texunit (
					    CAIRO_GL_UNIFORM_BLUR_STEP, tex_unit),
				    1.0 / cairo_gl_surface_get_height (&operand->texture.surface->base));

	_cairo_gl_shader_bind_float_array (ctx,
				    _cairo_gl_shader_uniform_for_texunit (
					    CAIRO_GL_UNIFORM_BLURS, tex_unit),
				    operand->texture.y_radius * 2 + 1,
				    operand->texture.coef);
    }

    if (operand->type == CAIRO_GL_OPERAND_TEXTURE ||
        operand->type == CAIRO_GL_OPERAND_GAUSSIAN) {
	    if (operand->texture.texgen)
		    texgen = &operand->texture.attributes.matrix;
    } else {
	    if (operand->gradient.texgen)
		    texgen = &operand->gradient.m;
    }

    if (texgen) {
	_cairo_gl_shader_bind_matrix (ctx,
				      _cairo_gl_shader_uniform_for_texunit (
					  CAIRO_GL_UNIFORM_TEXGEN, tex_unit),
				      texgen);
    }
}

cairo_bool_t
_cairo_gl_operand_needs_setup (cairo_gl_operand_t *dest,
                               cairo_gl_operand_t *source,
                               unsigned int        vertex_offset)
{
    if (dest->type != source->type)
        return TRUE;
    if (dest->vertex_offset != vertex_offset)
        return TRUE;

    switch (source->type) {
    case CAIRO_GL_OPERAND_NONE:
        return FALSE;
    case CAIRO_GL_OPERAND_CONSTANT:
	if (dest->constant.encode_as_attribute &&
	    source->constant.encode_as_attribute)
	    return FALSE;
        if (dest->constant.encode_as_attribute !=
	    source->constant.encode_as_attribute)
	    return TRUE;
        return dest->constant.color[0] != source->constant.color[0] ||
               dest->constant.color[1] != source->constant.color[1] ||
               dest->constant.color[2] != source->constant.color[2] ||
               dest->constant.color[3] != source->constant.color[3];
    case CAIRO_GL_OPERAND_TEXTURE:
        return dest->texture.surface != source->texture.surface ||
               dest->texture.attributes.extend != source->texture.attributes.extend ||
               dest->texture.attributes.filter != source->texture.attributes.filter ||
               dest->texture.attributes.has_component_alpha != source->texture.attributes.has_component_alpha;
    case CAIRO_GL_OPERAND_LINEAR_GRADIENT:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_A0:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_NONE:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_EXT:
    case CAIRO_GL_OPERAND_GAUSSIAN:
        /* XXX: improve this */
        return TRUE;
    default:
    case CAIRO_GL_OPERAND_COUNT:
        ASSERT_NOT_REACHED;
        break;
    }
    return TRUE;
}

unsigned int
_cairo_gl_operand_get_vertex_size (const cairo_gl_operand_t *operand)
{
    switch (operand->type) {
    default:
    case CAIRO_GL_OPERAND_COUNT:
        ASSERT_NOT_REACHED;
    case CAIRO_GL_OPERAND_NONE:
    case CAIRO_GL_OPERAND_CONSTANT:
        return operand->constant.encode_as_attribute ? 4 * sizeof (GLfloat) : 0;
    case CAIRO_GL_OPERAND_TEXTURE:
    case CAIRO_GL_OPERAND_GAUSSIAN:
	if (operand->texture.texgen) {
	    if (operand->texture.use_atlas)
		return 4 * sizeof (GLfloat);
	    return 0;
	}
	else {
	    if (operand->texture.use_atlas)
		return 6 * sizeof (GLfloat);
	    return 2 * sizeof (GLfloat);
    }
    case CAIRO_GL_OPERAND_LINEAR_GRADIENT:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_A0:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_NONE:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_EXT:
        return operand->gradient.texgen ? 0 : 2 * sizeof (GLfloat);
    }
}

void
_cairo_gl_operand_emit (cairo_gl_operand_t *operand,
                        GLfloat ** vb,
                        GLfloat x,
                        GLfloat y)
{
    switch (operand->type) {
    default:
    case CAIRO_GL_OPERAND_COUNT:
        ASSERT_NOT_REACHED;
    case CAIRO_GL_OPERAND_NONE:
	break;
    case CAIRO_GL_OPERAND_CONSTANT: {
	if (operand->constant.encode_as_attribute) {
	    *(*vb)++ = operand->constant.color[0];
	    *(*vb)++ = operand->constant.color[1];
	    *(*vb)++ = operand->constant.color[2];
	    *(*vb)++ = operand->constant.color[3];
	}
	break;
    }
    case CAIRO_GL_OPERAND_LINEAR_GRADIENT:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_A0:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_NONE:
    case CAIRO_GL_OPERAND_RADIAL_GRADIENT_EXT:
	if (! operand->gradient.texgen) {
	    double s = x;
	    double t = y;

	    cairo_matrix_transform_point (&operand->gradient.m, &s, &t);

	    *(*vb)++ = s;
	    *(*vb)++ = t;
        }
	break;
    case CAIRO_GL_OPERAND_TEXTURE:
    case CAIRO_GL_OPERAND_GAUSSIAN:
	if (! operand->texture.texgen) {
            cairo_surface_attributes_t *src_attributes = &operand->texture.attributes;
            double s = x;
            double t = y;

            cairo_matrix_transform_point (&src_attributes->matrix, &s, &t);
            *(*vb)++ = s;
            *(*vb)++ = t;
	}

	if (operand->texture.use_atlas) {
	    *(*vb)++ = operand->texture.p1.x;
	    *(*vb)++ = operand->texture.p1.y;
	    *(*vb)++ = operand->texture.p2.x;
	    *(*vb)++ = operand->texture.p2.y;
        }
        break;
    }
}

static inline cairo_int_status_t
_cairo_gl_context_get_image_cache (cairo_gl_context_t 	   *ctx,
				   cairo_gl_image_cache_t  **cache_out)
{
    if (! ctx->image_cache)
	return CAIRO_INT_STATUS_UNSUPPORTED;

    *cache_out = ctx->image_cache;
    return CAIRO_INT_STATUS_SUCCESS;
}

/* Called from _cairo_rtree_node_remove. */
void
_cairo_gl_image_node_destroy (cairo_rtree_node_t *node)
{
    cairo_surface_t *surface;

    cairo_gl_image_t *image_node = cairo_container_of (node,
						       cairo_gl_image_t,
						       node);

    surface = image_node->original_surface;
   /* Remove from original surface. */
   if (image_node->original_surface)
	((cairo_gl_surface_t *)surface)->image_node = NULL;
}

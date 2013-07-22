/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2005 Red Hat, Inc
 * Copyright © 2007 Adrian Johnson
 * Copyright © 2009 Chris Wilson
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
 * The Initial Developer of the Original Code is Red Hat, Inc.
 *
 * Contributor(s):
 *	Henry Song <henry.song@samsung.com
 */

#include "cairoint.h"
#include "cairo-surface-private.h"
#include "cairo-clip-inline.h"
#include "cairo-error-private.h"
#include "cairo-pattern-private.h"
#include "cairo-surface-shadow-private.h"
#include "cairo-surface-scale-translate-private.h"
#include "cairo-path-fixed-private.h"
#include "cairo-list-inline.h"
#include "cairo-device-private.h"
#include "cairo-image-surface-private.h"

#define MAX_SHADOW_SIZE 256

static unsigned long
_cairo_stroke_style_hash (unsigned long hash, 
			  const cairo_stroke_style_t *style)
{
    hash = _cairo_hash_bytes (hash, style, sizeof (cairo_stroke_style_t));
    if (style->num_dashes)
	hash = _cairo_hash_bytes (hash, style->dash, sizeof (double) * style->num_dashes);
    return hash;
}

static unsigned long
_cairo_matrix_hash (unsigned long hash, const cairo_matrix_t *matrix)
{
    return _cairo_hash_bytes (hash, matrix, sizeof (cairo_matrix_t));
}

static unsigned long
_cairo_path_fixed_rel_hash (unsigned long hash, const cairo_path_fixed_t *path)
{
    const cairo_path_buf_t *buf;
    unsigned int count;
    cairo_path_fixed_t path_copy;
    cairo_status_t status;
    unsigned int i;
    cairo_fixed_t dx, dy;
    
    status = _cairo_path_fixed_init_copy (&path_copy, path);
    if (unlikely (status))
	return hash;

    dx = path_copy.buf.points[0].x;
    dy = path_copy.buf.points[0].y;

    cairo_path_foreach_buf_start (buf, &path_copy) {
	for (i = 0; i < buf->num_points; i++) {
	    buf->points[i].x -= dx;
	    buf->points[i].y -= dy;
	}
    } cairo_path_foreach_buf_end (buf, &path_copy);

    count = 0;
    cairo_path_foreach_buf_start (buf, &path_copy) {
	hash = _cairo_hash_bytes (hash, buf->op,
			          buf->num_ops * sizeof (buf->op[0]));
	count += buf->num_ops;
    } cairo_path_foreach_buf_end (buf, &path_copy);
    hash = _cairo_hash_bytes (hash, &count, sizeof (count));

    count = 0;
    cairo_path_foreach_buf_start (buf, &path_copy) {
	hash = _cairo_hash_bytes (hash, buf->points,
			          buf->num_points * sizeof (buf->points[0]));
	count += buf->num_points;
    } cairo_path_foreach_buf_end (buf, &path_copy);
    hash = _cairo_hash_bytes (hash, &count, sizeof (count));

    _cairo_path_fixed_fini (&path_copy);

    return hash;
}

static unsigned long
_cairo_shadow_hash (unsigned long hash, const cairo_shadow_t *shadow)
{
    return _cairo_hash_bytes (hash, shadow, sizeof (cairo_shadow_t));
}

static unsigned long
_cairo_shadow_hash_for_paint (const cairo_pattern_t *source,
			      const cairo_shadow_t *shadow)
{
    unsigned long hash = _CAIRO_HASH_INIT_VALUE;

    hash = _cairo_pattern_hash_with_hash (hash, source);
    return _cairo_shadow_hash (hash, shadow);
}

static unsigned long
_cairo_shadow_hash_for_mask (const cairo_pattern_t *source,
			     const cairo_pattern_t *mask,
			     const cairo_shadow_t *shadow)
{
    unsigned long hash = _CAIRO_HASH_INIT_VALUE;

    hash = _cairo_pattern_hash_with_hash (hash, source);
    hash = _cairo_pattern_hash_with_hash (hash, mask);
    return _cairo_shadow_hash (hash, shadow);
}

static unsigned long
_cairo_shadow_hash_for_fill (const cairo_pattern_t      *source,
			    const cairo_path_fixed_t	*path,
			    cairo_fill_rule_t		 fill_rule,
			    const cairo_shadow_t	*shadow)
{
    unsigned long hash = _CAIRO_HASH_INIT_VALUE;

    hash = _cairo_pattern_hash_with_hash (hash, source);
    hash = _cairo_path_fixed_rel_hash (hash, path);
    hash = _cairo_hash_bytes (hash, &fill_rule, sizeof (cairo_fill_rule_t));
    return _cairo_shadow_hash (hash, shadow);
}

static unsigned long
_cairo_shadow_hash_for_stroke (const cairo_pattern_t      *source,
			       const cairo_path_fixed_t	  *path,
			       const cairo_stroke_style_t*stroke_style,
			       const cairo_matrix_t	*ctm,
			       const cairo_shadow_t     *shadow)
{
    unsigned long hash = _CAIRO_HASH_INIT_VALUE;

    hash = _cairo_pattern_hash_with_hash (hash, source);
    hash = _cairo_path_fixed_rel_hash (hash, path);
    hash = _cairo_stroke_style_hash (hash, stroke_style);
    hash = _cairo_matrix_hash (hash, ctm);
    return _cairo_shadow_hash (hash, shadow);
}

/* A collection of routines to draw shadow*/

cairo_status_t
_cairo_surface_shadow_paint (cairo_surface_t		*target,
			     cairo_operator_t		 op,
			     const cairo_pattern_t	*source,
			     const cairo_clip_t		*clip,
			     const cairo_shadow_t 	*shadow)
{
    cairo_status_t	  status;
    cairo_pattern_union_t shadow_source;
    cairo_rectangle_int_t shadow_extents;
    cairo_pattern_t 	 *shadow_pattern = NULL;
    cairo_pattern_t	 *color_pattern = NULL;
    cairo_surface_t	 *shadow_surface = NULL;
    cairo_rectangle_int_t shadow_surface_extents;

    int shadow_width, shadow_height;
    int width_out, height_out;
    int x_blur, y_blur;
    cairo_shadow_t       shadow_copy = *shadow;

    cairo_matrix_t 	  m;
    double 		  scale;
    double		  x_scale = 1.0;
    double		  y_scale = 1.0;
    double		  x_offset = shadow->x_offset;
    double		  y_offset = shadow->y_offset;
    cairo_content_t       content;

    unsigned long         hash = 0;
    cairo_shadow_cache_t *shadow_cache;
    cairo_device_t       *device = target->device;
    cairo_bool_t          found = FALSE;
    unsigned long         size;
    cairo_surface_t	 *cache_surface = NULL;
    cairo_bool_t          bounded;
    cairo_bool_t          locked = FALSE;
    cairo_list_t	 *shadow_caches = NULL;
    unsigned long	 *shadow_caches_size = 0;

    if (shadow->type == CAIRO_SHADOW_NONE)
	return CAIRO_STATUS_SUCCESS;

    if (shadow->x_sigma <= 0.0 && shadow->y_sigma <= 0.0 &&
	shadow->x_offset == 0.0 && shadow->y_sigma == 0.0)
	return CAIRO_STATUS_SUCCESS;

    if (_cairo_clip_is_all_clipped (clip))
	return CAIRO_STATUS_SUCCESS;

    if (device != NULL) {
	hash = _cairo_shadow_hash_for_paint (source, shadow);
	cairo_list_foreach_entry (shadow_cache, cairo_shadow_cache_t, &device->shadow_caches, link) {
	    if (shadow_cache->hash == hash) {
		found = TRUE;
		break;
	    }
	}
	shadow_caches = &device->shadow_caches;
	shadow_caches_size = &device->shadow_caches_size;
    }
    else if (target->type == CAIRO_SURFACE_TYPE_IMAGE) {
	cairo_image_surface_t *image_target = (cairo_image_surface_t *)target;

	status = _cairo_image_surface_shadow_cache_acquire (image_target);
	locked = TRUE;

	if (status == CAIRO_STATUS_SUCCESS) {
	    shadow_caches = _cairo_image_surface_get_shadow_cache (image_target);
	    if (shadow_caches) {
		shadow_caches_size = _cairo_image_surface_get_shadow_cache_size (image_target);
		hash = _cairo_shadow_hash_for_paint (source, shadow);
		cairo_list_foreach_entry (shadow_cache,
					  cairo_shadow_cache_t, 
					  shadow_caches, link) {
		    if (shadow_cache->hash == hash) {
			found = TRUE;
			break;
		    }
		}
	    }
	}
    }

    if (found) {
	/* paint the shadow surface to target */
	x_blur = shadow_cache->x_blur;
	y_blur = shadow_cache->y_blur;

	color_pattern = cairo_pattern_create_rgba (shadow_copy.color.red,
					           shadow_copy.color.green,
					           shadow_copy.color.blue,
						   1.0);

	status = _cairo_surface_paint_get_offset_extents (target,
							  x_offset,
							  y_offset,
							  source,
							  clip,
							  &shadow_source.base,
							  &shadow_extents,
							  &bounded);
 	if (unlikely (status))
	    goto FINISH;

	if (shadow_extents.width == 0 || shadow_extents.height == 0)
	    goto FINISH;

 	x_offset = shadow_extents.x - x_blur;
	y_offset = shadow_extents.y - y_blur;

	shadow_width = shadow_extents.width + x_blur * 2;
	shadow_height = shadow_extents.height + y_blur * 2;

	scale = shadow_cache->scale;

	cairo_matrix_init_scale (&m, scale, scale);
	cairo_matrix_translate (&m, -x_offset, -y_offset);

	shadow_pattern = cairo_pattern_create_for_surface (shadow_cache->surface);
	cairo_pattern_set_matrix (shadow_pattern, &m);

	status = _cairo_surface_mask (target, op, color_pattern, 
				      shadow_pattern, clip);
	cairo_list_move (&shadow_cache->link, shadow_caches);
	goto FINISH;
    }

    ((cairo_pattern_t *)source)->shadow.type = CAIRO_SHADOW_NONE;

    x_blur = ceil (shadow_copy.x_sigma * 2);
    y_blur = ceil (shadow_copy.y_sigma * 2);

    color_pattern = cairo_pattern_create_rgba (shadow_copy.color.red,
					       shadow_copy.color.green,
					       shadow_copy.color.blue,
					       shadow_copy.color.alpha);

    status = _cairo_surface_paint_get_offset_extents (target,
						      x_offset, y_offset,
						      source,
						      clip,
						      &shadow_source.base,
						      &shadow_extents,
						      &bounded);
    if (unlikely (status))
	goto FINISH;

    if (shadow_extents.width == 0 && shadow_extents.height == 0)
	goto FINISH;

    x_offset = shadow_extents.x - x_blur;
    y_offset = shadow_extents.y - y_blur;

    shadow_width = shadow_extents.width + x_blur * 2;
    shadow_height = shadow_extents.height + y_blur * 2;

    if (target->backend->get_shadow_surface)
	shadow_surface = target->backend->get_shadow_surface (target,
						      shadow_width,
						      shadow_height,
						      &width_out,
						      &height_out);
    else {
	int scaled_width = MAX (shadow_width, MAX_SHADOW_SIZE);
	int scaled_height = MAX (shadow_height, MAX_SHADOW_SIZE);

	content = cairo_surface_get_content (target);
 	if (content == CAIRO_CONTENT_COLOR)
	    content = CAIRO_CONTENT_COLOR_ALPHA;
	shadow_surface = cairo_surface_create_similar (target,
						       content,
						       scaled_width,
						       scaled_height);
	width_out = scaled_width;
	height_out = scaled_height;
    }
    if (unlikely (shadow_surface->status))
	goto FINISH;

    shadow_surface_extents.x = shadow_surface_extents.y = 0;
    shadow_surface_extents.width = width_out;
    shadow_surface_extents.height = height_out;

    if ((device || locked) && shadow->enable_cache && bounded) {
	content = cairo_surface_get_content (target);
	if (content == CAIRO_CONTENT_COLOR)
	    content = CAIRO_CONTENT_COLOR_ALPHA;

	cache_surface = cairo_surface_create_similar (target, content,
						      shadow_surface_extents.width,
						      shadow_surface_extents.height);
	if (unlikely (cache_surface->status))
	    goto FINISH;

	if (device)
	    _cairo_surface_release_device_reference (cache_surface);
    }

    x_scale = (double) shadow_surface_extents.width / (double) shadow_width;
    y_scale = (double) shadow_surface_extents.height / (double) shadow_height;

    scale = MIN (x_scale, y_scale);
    if (scale > 1.0)
	scale = 1.0;

    cairo_matrix_init_scale (&m, scale, scale);
    cairo_matrix_translate (&m, -x_offset, -y_offset);

    /* paint with offset and scale */
    status = _cairo_surface_scale_translate_paint (shadow_surface,
						   &m,
						   CAIRO_OPERATOR_OVER,
						   &shadow_source.base,
						   NULL);

    if (unlikely (status))
	goto FINISH;

    shadow_pattern = cairo_pattern_create_for_surface (shadow_surface);
    cairo_pattern_set_filter (shadow_pattern, CAIRO_FILTER_GAUSSIAN);
    cairo_pattern_set_sigma (shadow_pattern, shadow_copy.x_sigma * scale,
			     shadow_copy.y_sigma * scale);

    status = _cairo_pattern_create_gaussian_matrix (shadow_pattern, 1024);
    if (unlikely (status))
	goto FINISH;

    if ((locked ||device) && shadow->enable_cache && bounded) {
	status = _cairo_surface_mask (cache_surface, CAIRO_OPERATOR_OVER,
				      color_pattern, shadow_pattern, NULL);
	if (unlikely (status))
	    goto FINISH;

	cairo_pattern_destroy (shadow_pattern);

	size = shadow_surface_extents.width * shadow_surface_extents.height;

	while (*shadow_caches_size + size > MAX_SHADOW_CACHE_SIZE) {
	    shadow_cache = cairo_list_last_entry (shadow_caches,
						  cairo_shadow_cache_t,
						  link);
	    cairo_list_del (&shadow_cache->link);
	    cairo_surface_destroy (shadow_cache->surface);
	    *shadow_caches_size -= shadow_cache->size;
	    free (shadow_cache);
	}

	shadow_cache = malloc (sizeof (cairo_shadow_cache_t));
	cairo_list_init (&shadow_cache->link);
	shadow_cache->surface = cairo_surface_reference (cache_surface);
	shadow_cache->size = size;
	shadow_cache->hash = hash;
	shadow_cache->x_blur = x_blur;
	shadow_cache->y_blur = y_blur;
	shadow_cache->scale = scale;

	cairo_list_add (&shadow_cache->link, shadow_caches);

	*shadow_caches_size += size;

	shadow_pattern = cairo_pattern_create_for_surface (cache_surface);
	cairo_pattern_set_matrix (shadow_pattern, &m);

	cairo_pattern_destroy (color_pattern);
	color_pattern = cairo_pattern_create_rgba (shadow_copy.color.red,
					           shadow_copy.color.green,
					           shadow_copy.color.blue,
					           1.0);

	status = _cairo_surface_mask (target, op, color_pattern, 
				      shadow_pattern, clip);
    }
    else {
	cairo_pattern_set_matrix (shadow_pattern, &m);
	status = _cairo_surface_mask (target, op,
				      color_pattern, shadow_pattern, clip);
    }

FINISH:
    cairo_pattern_destroy (color_pattern);

    if (shadow_pattern)
	cairo_pattern_destroy (shadow_pattern);

    cairo_surface_destroy (shadow_surface);
    cairo_surface_destroy (cache_surface);

    if (locked)
	_cairo_image_surface_shadow_cache_release ((cairo_image_surface_t *)target);

    return status;
}

cairo_status_t
_cairo_surface_shadow_mask (cairo_surface_t		*target,
			    cairo_operator_t		 op,
			    const cairo_pattern_t	*source,
			    const cairo_pattern_t	*mask,
			    const cairo_clip_t		*clip,
			    const cairo_shadow_t	*shadow)
{
    cairo_status_t	  status;
    cairo_pattern_union_t shadow_source;
    cairo_pattern_union_t shadow_mask;
    cairo_rectangle_int_t shadow_extents;
    cairo_pattern_t 	 *shadow_pattern = NULL;
    cairo_pattern_t	 *color_pattern = NULL;
    cairo_surface_t	 *shadow_surface = NULL;
    cairo_rectangle_int_t shadow_surface_extents;
    cairo_content_t       content;

    int shadow_width, shadow_height;
    int width_out, height_out;
    int x_blur, y_blur;
    cairo_shadow_t       shadow_copy = *shadow;

    cairo_matrix_t 	  m;
    double 		  scale;
    double		  x_scale = 1.0;
    double		  y_scale = 1.0;
    double		  x_offset = shadow->x_offset;
    double		  y_offset = shadow->y_offset;

    unsigned long         hash = 0;
    cairo_shadow_cache_t *shadow_cache;
    cairo_device_t       *device = target->device;
    cairo_bool_t          found = FALSE;
    unsigned long         size;
    cairo_surface_t	 *cache_surface = NULL;
    cairo_bool_t          bounded;
    cairo_bool_t          locked = FALSE;
    cairo_list_t	 *shadow_caches = NULL;
    unsigned long        *shadow_caches_size = 0;

    if (shadow->type == CAIRO_SHADOW_NONE)
	return CAIRO_STATUS_SUCCESS;

    if (shadow->x_sigma <= 0.0 && shadow->y_sigma <= 0.0 &&
	shadow->x_offset == 0.0 && shadow->y_sigma == 0.0)
	return CAIRO_STATUS_SUCCESS;

    if (_cairo_clip_is_all_clipped (clip))
	return CAIRO_STATUS_SUCCESS;

    if (device != NULL) {
	hash = _cairo_shadow_hash_for_mask (source, mask, shadow);
	shadow_caches = &device->shadow_caches;
	shadow_caches_size = &device->shadow_caches_size;
	cairo_list_foreach_entry (shadow_cache, cairo_shadow_cache_t, &device->shadow_caches, link) {
	    if (shadow_cache->hash == hash) {
		found = TRUE;
		break;
	    }
	}
    }
    else if (target->type == CAIRO_SURFACE_TYPE_IMAGE) {
	cairo_image_surface_t *image_target = (cairo_image_surface_t *)target;

	status = _cairo_image_surface_shadow_cache_acquire (image_target);
	locked = TRUE;

	if (status == CAIRO_STATUS_SUCCESS) {
	    shadow_caches = _cairo_image_surface_get_shadow_cache (image_target);
	    if (shadow_caches) {
		shadow_caches_size = _cairo_image_surface_get_shadow_cache_size (image_target);
		hash = _cairo_shadow_hash_for_mask (source, mask, shadow);
		cairo_list_foreach_entry (shadow_cache,
					  cairo_shadow_cache_t, 
					  shadow_caches, link) {
		    if (shadow_cache->hash == hash) {
			found = TRUE;
			break;
		    }
		}
	    }
	}
    }

    if (found) {
	/* paint the shadow surface to target */
	x_blur = shadow_cache->x_blur;
	y_blur = shadow_cache->y_blur;

	color_pattern = cairo_pattern_create_rgba (shadow_copy.color.red,
					           shadow_copy.color.green,
					           shadow_copy.color.blue,
						   1.0);

	status = _cairo_surface_mask_get_offset_extents (target,
							  x_offset,
							  y_offset,
							  source,
							  mask,
							  clip,
							  &shadow_source.base,
							  &shadow_mask.base,
							  &shadow_extents,
							  &bounded);
 	if (unlikely (status))
	    goto FINISH;

	if (shadow_extents.width == 0 || shadow_extents.height == 0)
	    goto FINISH;

 	x_offset = shadow_extents.x - x_blur;
	y_offset = shadow_extents.y - y_blur;

	shadow_width = shadow_extents.width + x_blur * 2;
	shadow_height = shadow_extents.height + y_blur * 2;

	scale = shadow_cache->scale;

	cairo_matrix_init_scale (&m, scale, scale);
	cairo_matrix_translate (&m, -x_offset, -y_offset);

	shadow_pattern = cairo_pattern_create_for_surface (shadow_cache->surface);
	cairo_pattern_set_matrix (shadow_pattern, &m);

	status = _cairo_surface_mask (target, op, color_pattern, 
				      shadow_pattern, clip);
	cairo_list_move (&shadow_cache->link, shadow_caches);
	goto FINISH;
    }

    ((cairo_pattern_t *)source)->shadow.type = CAIRO_SHADOW_NONE;

    x_blur = ceil (shadow_copy.x_sigma * 2);
    y_blur = ceil (shadow_copy.y_sigma * 2);

    color_pattern = cairo_pattern_create_rgba (shadow_copy.color.red,
					       shadow_copy.color.green,
					       shadow_copy.color.blue,
					       shadow_copy.color.alpha);

    status = _cairo_surface_mask_get_offset_extents (target,
						     x_offset, y_offset,
						     source,
						     mask,
						     clip,
						     &shadow_source.base,
						     &shadow_mask.base,
						     &shadow_extents,
						     &bounded);
    if (unlikely (status))
	goto FINISH;

    if (shadow_extents.width == 0 && shadow_extents.height == 0)
	goto FINISH;

    x_offset = shadow_extents.x - x_blur;
    y_offset = shadow_extents.y - y_blur;

    shadow_width = shadow_extents.width + x_blur * 2;
    shadow_height = shadow_extents.height + y_blur * 2;

    if (target->backend->get_shadow_surface)
	shadow_surface = target->backend->get_shadow_surface (target,
						      shadow_width,
						      shadow_height,
						      &width_out,
						      &height_out);
    else {
	int scaled_width = MAX (shadow_width, MAX_SHADOW_SIZE);
	int scaled_height = MAX (shadow_width, MAX_SHADOW_SIZE);

	content = cairo_surface_get_content (target);
	content = cairo_surface_get_content (target);
	if (content == CAIRO_CONTENT_COLOR)
	    content = CAIRO_CONTENT_COLOR_ALPHA;
	shadow_surface = cairo_surface_create_similar (target,
						       content,
						       scaled_width,
						       scaled_height);
	width_out = scaled_width;
	height_out = scaled_height;
    }
    if (unlikely (shadow_surface->status))
	goto FINISH;

    shadow_surface_extents.x = shadow_surface_extents.y = 0;
    shadow_surface_extents.width = width_out;
    shadow_surface_extents.height = height_out;

    if ((locked || device) && shadow->enable_cache && bounded) {
	content = cairo_surface_get_content (target);
	if (content == CAIRO_CONTENT_COLOR)
	    content = CAIRO_CONTENT_COLOR_ALPHA;

	cache_surface = cairo_surface_create_similar (target, content,
						      shadow_surface_extents.width,
						      shadow_surface_extents.height);
	if (unlikely (cache_surface->status))
	    goto FINISH;

	if (device)
	    _cairo_surface_release_device_reference (cache_surface);
    }

    x_scale = (double) shadow_surface_extents.width / (double) shadow_width;
    y_scale = (double) shadow_surface_extents.height / (double) shadow_height;

    scale = MIN (x_scale, y_scale);
    if (scale > 1.0)
	scale = 1.0;

    cairo_matrix_init_scale (&m, scale, scale);
    cairo_matrix_translate (&m, -x_offset, -y_offset);

    /* paint with offset and scale */
    status = _cairo_surface_scale_translate_mask (shadow_surface,
						   &m,
						   CAIRO_OPERATOR_OVER,
						   &shadow_source.base,
						   &shadow_mask.base,
						   NULL);

    if (unlikely (status))
	goto FINISH;

    shadow_pattern = cairo_pattern_create_for_surface (shadow_surface);
    cairo_pattern_set_filter (shadow_pattern, CAIRO_FILTER_GAUSSIAN);
    cairo_pattern_set_sigma (shadow_pattern, shadow_copy.x_sigma * scale,
			     shadow_copy.y_sigma * scale);

    status = _cairo_pattern_create_gaussian_matrix (shadow_pattern, 1024);
    if (unlikely (status))
	goto FINISH;

    if ((locked || device) && shadow->enable_cache && bounded) {
	status = _cairo_surface_mask (cache_surface, CAIRO_OPERATOR_OVER,
				      color_pattern, shadow_pattern, NULL);
	if (unlikely (status))
	    goto FINISH;

	cairo_pattern_destroy (shadow_pattern);

	size = shadow_surface_extents.width * shadow_surface_extents.height;

	while (*shadow_caches_size + size > MAX_SHADOW_CACHE_SIZE) {
	    shadow_cache = cairo_list_last_entry (shadow_caches,
						  cairo_shadow_cache_t,
						  link);
	    cairo_list_del (&shadow_cache->link);
	    cairo_surface_destroy (shadow_cache->surface);
	    *shadow_caches_size -= shadow_cache->size;
	    free (shadow_cache);
	}

	shadow_cache = malloc (sizeof (cairo_shadow_cache_t));
	cairo_list_init (&shadow_cache->link);
	shadow_cache->surface = cairo_surface_reference (cache_surface);
	shadow_cache->size = size;
	shadow_cache->hash = hash;
	shadow_cache->x_blur = x_blur;
	shadow_cache->y_blur = y_blur;
	shadow_cache->scale = scale;

	cairo_list_add (&shadow_cache->link, shadow_caches);
	*shadow_caches_size += size;

	shadow_pattern = cairo_pattern_create_for_surface (cache_surface);
	cairo_pattern_set_matrix (shadow_pattern, &m);

	cairo_pattern_destroy (color_pattern);
	color_pattern = cairo_pattern_create_rgba (shadow_copy.color.red,
					           shadow_copy.color.green,
					           shadow_copy.color.blue,
					           1.0);

	status = _cairo_surface_mask (target, op, color_pattern, 
				      shadow_pattern, clip);
    }
    else {
	cairo_pattern_set_matrix (shadow_pattern, &m);
	status = _cairo_surface_mask (target, op,
				      color_pattern, shadow_pattern, clip);
    }

FINISH:
    cairo_pattern_destroy (color_pattern);

    if (shadow_pattern)
	cairo_pattern_destroy (shadow_pattern);

    cairo_surface_destroy (shadow_surface);
    cairo_surface_destroy (cache_surface);

    if (locked)
	_cairo_image_surface_shadow_cache_release ((cairo_image_surface_t *)target);

    return status;
}

cairo_status_t
_cairo_surface_shadow_stroke (cairo_surface_t		*target,
			      cairo_operator_t		 op,
			      const cairo_pattern_t	*source,
			      const cairo_path_fixed_t	*path,
			      const cairo_stroke_style_t*stroke_style,
			      const cairo_matrix_t	*ctm,
			      const cairo_matrix_t	*ctm_inverse,
			      double			 tolerance,
			      cairo_antialias_t		 antialias,
			      const cairo_clip_t		*clip,
			      const cairo_shadow_t	*shadow)
{
    cairo_status_t	  status;
    cairo_pattern_union_t shadow_source;
    cairo_path_fixed_t    shadow_path;
    cairo_rectangle_int_t shadow_extents;
    cairo_pattern_t 	 *shadow_pattern = NULL;
    cairo_pattern_t	 *color_pattern = NULL;
    cairo_surface_t	 *shadow_surface = NULL;
    cairo_rectangle_int_t shadow_surface_extents;
    cairo_matrix_t        shadow_ctm, shadow_ctm_inverse;
    cairo_content_t       content;

    int shadow_width, shadow_height;
    int width_out, height_out;
    int x_blur, y_blur;
    cairo_shadow_t       shadow_copy = *shadow;

    cairo_matrix_t 	  m;
    double 		  scale;
    double		  x_scale = 1.0;
    double		  y_scale = 1.0;
    double		  x_offset = shadow->x_offset;
    double		  y_offset = shadow->y_offset;
    unsigned long         hash = 0;
    cairo_shadow_cache_t *shadow_cache;
    cairo_device_t       *device = target->device;
    cairo_bool_t          found = FALSE;
    unsigned long         size;
    cairo_surface_t	 *cache_surface = NULL;
    cairo_bool_t          locked = FALSE;
    cairo_list_t	 *shadow_caches = NULL;
    unsigned long        *shadow_caches_size = 0;

    if (shadow->type == CAIRO_SHADOW_NONE)
	return CAIRO_STATUS_SUCCESS;

    if (shadow->x_sigma <= 0.0 && shadow->y_sigma <= 0.0 &&
	shadow->x_offset == 0.0 && shadow->y_sigma == 0.0)
	return CAIRO_STATUS_SUCCESS;

    if (_cairo_clip_is_all_clipped (clip))
	return CAIRO_STATUS_SUCCESS;

    if (device != NULL) {
	hash = _cairo_shadow_hash_for_stroke (source, path, stroke_style, ctm, shadow);
	shadow_caches = &device->shadow_caches;
	shadow_caches_size = &device->shadow_caches_size;
	cairo_list_foreach_entry (shadow_cache, cairo_shadow_cache_t, shadow_caches, link) {
	    if (shadow_cache->hash == hash) {
		found = TRUE;
		break;
	    }
	}
    }
    else if (target->type == CAIRO_SURFACE_TYPE_IMAGE) {
	cairo_image_surface_t *image_target = (cairo_image_surface_t *)target;

	status = _cairo_image_surface_shadow_cache_acquire (image_target);
	locked = TRUE;

	if (status == CAIRO_STATUS_SUCCESS) {
	    shadow_caches = _cairo_image_surface_get_shadow_cache (image_target);
	    if (shadow_caches) {
		shadow_caches_size = _cairo_image_surface_get_shadow_cache_size (image_target);
		hash = _cairo_shadow_hash_for_stroke (source, path, stroke_style, ctm, shadow);
		cairo_list_foreach_entry (shadow_cache,
					  cairo_shadow_cache_t, 
					  shadow_caches, link) {
		    if (shadow_cache->hash == hash) {
			found = TRUE;
			break;
		    }
		}
	    }
	}
    }


    if (found) {
	/* paint the shadow surface to target */
	x_blur = shadow_cache->x_blur;
	y_blur = shadow_cache->y_blur;

	color_pattern = cairo_pattern_create_rgba (shadow_copy.color.red,
					           shadow_copy.color.green,
					           shadow_copy.color.blue,
						   1.0);

	status = _cairo_surface_stroke_get_offset_extents (target,
							   x_offset,
							   y_offset,
							   source,
							   path,
							   stroke_style,
							   ctm, ctm_inverse,
							   clip,
							   &shadow_source.base,
							   &shadow_path,
							   &shadow_ctm,
							   &shadow_ctm_inverse,
							   &shadow_extents);
 	if (unlikely (status))
	    goto FINISH;

	if (shadow_extents.width == 0 || shadow_extents.height == 0)
	    goto FINISH;

 	x_offset = shadow_extents.x - x_blur;
	y_offset = shadow_extents.y - y_blur;

	shadow_width = shadow_extents.width + x_blur * 2;
	shadow_height = shadow_extents.height + y_blur * 2;

	scale = shadow_cache->scale;

	cairo_matrix_init_scale (&m, scale, scale);
	cairo_matrix_translate (&m, -x_offset, -y_offset);

	shadow_pattern = cairo_pattern_create_for_surface (shadow_cache->surface);
	cairo_pattern_set_matrix (shadow_pattern, &m);

	status = _cairo_surface_mask (target, op, color_pattern, 
				      shadow_pattern, clip);
	cairo_list_move (&shadow_cache->link, shadow_caches);
	goto FINISH;
    }

    ((cairo_pattern_t *)source)->shadow.type = CAIRO_SHADOW_NONE;

    x_blur = ceil (shadow_copy.x_sigma * 2);
    y_blur = ceil (shadow_copy.y_sigma * 2);

    color_pattern = cairo_pattern_create_rgba (shadow_copy.color.red,
					       shadow_copy.color.green,
					       shadow_copy.color.blue,
					       shadow_copy.color.alpha);

    status = _cairo_surface_stroke_get_offset_extents (target,
						       x_offset, y_offset,
						       source,
						       path,
						       stroke_style,
						       ctm, ctm_inverse,
						       clip,
						       &shadow_source.base,
						       &shadow_path,
						       &shadow_ctm,
						       &shadow_ctm_inverse,
						       &shadow_extents);
    if (unlikely (status))
	goto FINISH;

    if (shadow_extents.width == 0 || shadow_extents.height == 0)
	goto FINISH;

    x_offset = shadow_extents.x - x_blur;
    y_offset = shadow_extents.y - y_blur;

    shadow_width = shadow_extents.width + x_blur * 2;
    shadow_height = shadow_extents.height + y_blur * 2;

    if (target->backend->get_shadow_surface)
	shadow_surface = target->backend->get_shadow_surface (target,
						      shadow_width,
						      shadow_height,
						      &width_out,
						      &height_out);
    else {
	int scaled_width = MAX (shadow_width, MAX_SHADOW_SIZE);
	int scaled_height = MAX (shadow_width, MAX_SHADOW_SIZE);

	content = cairo_surface_get_content (target);
	if (content == CAIRO_CONTENT_COLOR)
	    content = CAIRO_CONTENT_COLOR_ALPHA;
	shadow_surface = cairo_surface_create_similar (target,
						       content,
						       scaled_width,
						       scaled_height);
	width_out = scaled_width;
	height_out = scaled_height;
    }
    if (unlikely (shadow_surface->status))
	goto FINISH;

    shadow_surface_extents.x = shadow_surface_extents.y = 0;
    shadow_surface_extents.width = width_out;
    shadow_surface_extents.height = height_out;

    if ((locked || device) && shadow->enable_cache) {
	content = cairo_surface_get_content (target);
	if (content == CAIRO_CONTENT_COLOR)
	    content = CAIRO_CONTENT_COLOR_ALPHA;

	cache_surface = cairo_surface_create_similar (target, content,
						      shadow_surface_extents.width,
						      shadow_surface_extents.height);
	if (unlikely (cache_surface->status))
	    goto FINISH;

	if (device)
	    _cairo_surface_release_device_reference (cache_surface);
    }

    x_scale = (double) shadow_surface_extents.width / (double) shadow_width;
    y_scale = (double) shadow_surface_extents.height / (double) shadow_height;

    scale = MIN (x_scale, y_scale);
    if (scale > 1.0)
	scale = 1.0;

    cairo_matrix_init_scale (&m, scale, scale);
    cairo_matrix_translate (&m, -x_offset, -y_offset);

    /* paint with offset and scale */
    status = _cairo_surface_scale_translate_stroke (shadow_surface,
						    &m,
						    CAIRO_OPERATOR_OVER,
						    &shadow_source.base,
						    &shadow_path,
						    stroke_style,
						    &shadow_ctm,
						    &shadow_ctm_inverse,
						    tolerance,
						    antialias,
						    NULL);

    if (unlikely (status))
	goto FINISH;

    shadow_pattern = cairo_pattern_create_for_surface (shadow_surface);
    cairo_pattern_set_filter (shadow_pattern, CAIRO_FILTER_GAUSSIAN);
    cairo_pattern_set_sigma (shadow_pattern, shadow_copy.x_sigma * scale,
			     shadow_copy.y_sigma * scale);

    status = _cairo_pattern_create_gaussian_matrix (shadow_pattern,
						    stroke_style->line_width);
    if (unlikely (status))
	goto FINISH;

    if ((locked || device) && shadow->enable_cache) {
	status = _cairo_surface_mask (cache_surface, CAIRO_OPERATOR_OVER,
				      color_pattern, shadow_pattern, NULL);
	if (unlikely (status))
	    goto FINISH;

	cairo_pattern_destroy (shadow_pattern);

	size = shadow_surface_extents.width * shadow_surface_extents.height;

	while (*shadow_caches_size + size > MAX_SHADOW_CACHE_SIZE) {
	    shadow_cache = cairo_list_last_entry (shadow_caches,
						  cairo_shadow_cache_t,
						  link);
	    cairo_list_del (&shadow_cache->link);
	    cairo_surface_destroy (shadow_cache->surface);
	    *shadow_caches_size -= shadow_cache->size;
	    free (shadow_cache);
	}

	shadow_cache = malloc (sizeof (cairo_shadow_cache_t));
	cairo_list_init (&shadow_cache->link);
	shadow_cache->surface = cairo_surface_reference (cache_surface);
	shadow_cache->size = size;
	shadow_cache->hash = hash;
	shadow_cache->x_blur = x_blur;
	shadow_cache->y_blur = y_blur;
	shadow_cache->scale = scale;

	cairo_list_add (&shadow_cache->link, shadow_caches);
	*shadow_caches_size += size;

	shadow_pattern = cairo_pattern_create_for_surface (cache_surface);
	cairo_pattern_set_matrix (shadow_pattern, &m);

	cairo_pattern_destroy (color_pattern);
	color_pattern = cairo_pattern_create_rgba (shadow_copy.color.red,
					           shadow_copy.color.green,
					           shadow_copy.color.blue,
					           1.0);


	status = _cairo_surface_mask (target, op, color_pattern, 
				      shadow_pattern, clip);
    }
    else {
	cairo_pattern_set_matrix (shadow_pattern, &m);
	status = _cairo_surface_mask (target, op,
				      color_pattern, shadow_pattern, clip);
    }

FINISH:
    _cairo_path_fixed_fini (&shadow_path);
    cairo_pattern_destroy (color_pattern);

    if (shadow_pattern)
	cairo_pattern_destroy (shadow_pattern);

    cairo_surface_destroy (shadow_surface);
    cairo_surface_destroy (cache_surface);

    if (locked)
	_cairo_image_surface_shadow_cache_release ((cairo_image_surface_t *)target);

    return status;
}

cairo_status_t
_cairo_surface_shadow_fill (cairo_surface_t	*target,
			    cairo_operator_t	 op,
			    const cairo_pattern_t*source,
			    const cairo_path_fixed_t	*path,
			    cairo_fill_rule_t	 fill_rule,
			    double		 tolerance,
			    cairo_antialias_t	 antialias,
			    const cairo_clip_t	*clip,
			    const cairo_shadow_t *shadow)
{
    cairo_status_t	  status;
    cairo_pattern_union_t shadow_source;
    cairo_path_fixed_t    shadow_path;
    cairo_rectangle_int_t shadow_extents;
    cairo_pattern_t 	 *shadow_pattern = NULL;
    cairo_pattern_t	 *color_pattern = NULL;
    cairo_surface_t	 *shadow_surface = NULL;
    cairo_rectangle_int_t shadow_surface_extents;
    cairo_content_t       content;

    int shadow_width, shadow_height;
    int width_out, height_out;
    int x_blur, y_blur;
    cairo_shadow_t       shadow_copy = *shadow;

    cairo_matrix_t 	  m;
    double 		  scale;
    double		  x_scale = 1.0;
    double		  y_scale = 1.0;
    double		  x_offset = shadow->x_offset;
    double		  y_offset = shadow->y_offset;
    unsigned long         hash = 0;
    cairo_shadow_cache_t *shadow_cache;
    cairo_device_t       *device = target->device;
    cairo_bool_t          found = FALSE;
    unsigned long         size;
    cairo_surface_t	 *cache_surface = NULL;
    cairo_bool_t          locked = FALSE;
    cairo_list_t	 *shadow_caches = NULL;
    unsigned long        *shadow_caches_size = 0;

    if (shadow->type == CAIRO_SHADOW_NONE)
	return CAIRO_STATUS_SUCCESS;

    if (shadow->x_sigma <= 0.0 && shadow->y_sigma <= 0.0 &&
	shadow->x_offset == 0.0 && shadow->y_sigma == 0.0)
	return CAIRO_STATUS_SUCCESS;

    if (_cairo_clip_is_all_clipped (clip))
	return CAIRO_STATUS_SUCCESS;


    if (device != NULL) {
	hash = _cairo_shadow_hash_for_fill (source, path, fill_rule, shadow);
	shadow_caches = &device->shadow_caches;
	shadow_caches_size = &device->shadow_caches_size;
	cairo_list_foreach_entry (shadow_cache, cairo_shadow_cache_t, &device->shadow_caches, link) {
	    if (shadow_cache->hash == hash) {
		found = TRUE;
		break;
	    }
	}
    }
    else if (target->type == CAIRO_SURFACE_TYPE_IMAGE) {
	cairo_image_surface_t *image_target = (cairo_image_surface_t *)target;

	status = _cairo_image_surface_shadow_cache_acquire (image_target);
	locked = TRUE;

	if (status == CAIRO_STATUS_SUCCESS) {
	    shadow_caches = _cairo_image_surface_get_shadow_cache (image_target);
	    if (shadow_caches) {
		shadow_caches_size = _cairo_image_surface_get_shadow_cache_size (image_target);
		hash = _cairo_shadow_hash_for_fill (source, path, fill_rule, shadow);
		cairo_list_foreach_entry (shadow_cache,
					  cairo_shadow_cache_t, 
					  shadow_caches, link) {
		    if (shadow_cache->hash == hash) {
			found = TRUE;
			break;
		    }
		}
	    }
	}
    }

    if (found) {
	/* paint the shadow surface to target */
 	color_pattern = cairo_pattern_create_rgba (shadow_copy.color.red,
					           shadow_copy.color.green,
					           shadow_copy.color.blue,
					           1.0);
	x_blur = shadow_cache->x_blur;
	y_blur = shadow_cache->y_blur;

	status = _cairo_surface_fill_get_offset_extents (target,
							 x_offset,
							 y_offset,
							 source,
							 path,
							 clip,
							 &shadow_source.base,
							 &shadow_path,
							 &shadow_extents);
 	if (unlikely (status))
	    goto FINISH;

	if (shadow_extents.width == 0 || shadow_extents.height == 0)
	    goto FINISH;

 	x_offset = shadow_extents.x - x_blur;
	y_offset = shadow_extents.y - y_blur;

	shadow_width = shadow_extents.width + x_blur * 2;
	shadow_height = shadow_extents.height + y_blur * 2;

	scale = shadow_cache->scale;

	cairo_matrix_init_scale (&m, scale, scale);
	cairo_matrix_translate (&m, -x_offset, -y_offset);

	shadow_pattern = cairo_pattern_create_for_surface (shadow_cache->surface);
	cairo_pattern_set_matrix (shadow_pattern, &m);

	status = _cairo_surface_mask (target, op, color_pattern, 
				      shadow_pattern, clip);
	cairo_list_move (&shadow_cache->link, shadow_caches);
	goto FINISH;
    }

    ((cairo_pattern_t *)source)->shadow.type = CAIRO_SHADOW_NONE;
 
    color_pattern = cairo_pattern_create_rgba (shadow_copy.color.red,
					       shadow_copy.color.green,
					       shadow_copy.color.blue,
					       shadow_copy.color.alpha);

    x_blur = ceil (shadow_copy.x_sigma * 2);
    y_blur = ceil (shadow_copy.y_sigma * 2);

    status = _cairo_surface_fill_get_offset_extents (target,
						     x_offset, y_offset,
						     source,
						     path,
						     clip,
						     &shadow_source.base,
						     &shadow_path,
						     &shadow_extents);
    if (unlikely (status))
	goto FINISH;

    if (shadow_extents.width == 0 && shadow_extents.height == 0)
	goto FINISH;

    x_offset = shadow_extents.x - x_blur;
    y_offset = shadow_extents.y - y_blur;

    shadow_width = shadow_extents.width + x_blur * 2;
    shadow_height = shadow_extents.height + y_blur * 2;

    if (target->backend->get_shadow_surface)
	shadow_surface = target->backend->get_shadow_surface (target,
						      shadow_width,
						      shadow_height,
						      &width_out,
						      &height_out);
    else {
	int scaled_width = MAX (shadow_width, MAX_SHADOW_SIZE);
	int scaled_height = MAX (shadow_width, MAX_SHADOW_SIZE);

	content = cairo_surface_get_content (target);
	if (content == CAIRO_CONTENT_COLOR)
	    content = CAIRO_CONTENT_COLOR_ALPHA;
	shadow_surface = cairo_surface_create_similar (target,
						       content,
						       scaled_width,
						       scaled_height);
	width_out = scaled_width;
	height_out = scaled_height;
    }
    if (unlikely (shadow_surface->status))
	goto FINISH;

    shadow_surface_extents.x = shadow_surface_extents.y = 0;
    shadow_surface_extents.width = width_out;
    shadow_surface_extents.height = height_out;

    if ((locked || device) && shadow->enable_cache) {
	content = cairo_surface_get_content (target);
	if (content == CAIRO_CONTENT_COLOR)
	    content = CAIRO_CONTENT_COLOR_ALPHA;

	cache_surface = cairo_surface_create_similar (target, content,
						      shadow_surface_extents.width,
						      shadow_surface_extents.height);
 	if (unlikely (cache_surface->status))
	    goto FINISH;

	if (device)
	    _cairo_surface_release_device_reference (cache_surface);
    }

    x_scale = (double) shadow_surface_extents.width / (double) shadow_width;
    y_scale = (double) shadow_surface_extents.height / (double) shadow_height;

    scale = MIN (x_scale, y_scale);
    if (scale > 1.0)
	scale = 1.0;

    cairo_matrix_init_scale (&m, scale, scale);
    cairo_matrix_translate (&m, -x_offset, -y_offset);

    /* paint with offset and scale */
    status = _cairo_surface_scale_translate_fill (shadow_surface,
						  &m,
						  CAIRO_OPERATOR_OVER,
						  &shadow_source.base,
						  &shadow_path,
						  fill_rule,
						  tolerance,
						  antialias,
						  NULL);

    if (unlikely (status))
	goto FINISH;

    shadow_pattern = cairo_pattern_create_for_surface (shadow_surface);
    cairo_pattern_set_filter (shadow_pattern, CAIRO_FILTER_GAUSSIAN);
    cairo_pattern_set_sigma (shadow_pattern, shadow_copy.x_sigma * scale,
			     shadow_copy.y_sigma * scale);

    status = _cairo_pattern_create_gaussian_matrix (shadow_pattern, 1024);
    if (unlikely (status))
	goto FINISH;

    if ((locked || device) && shadow->enable_cache) {
	status = _cairo_surface_mask (cache_surface, CAIRO_OPERATOR_OVER,
				      color_pattern, shadow_pattern, NULL);
	if (unlikely (status))
	    goto FINISH;

 	cairo_pattern_destroy (shadow_pattern);

	size = shadow_surface_extents.width * shadow_surface_extents.height;

	while (*shadow_caches_size + size > MAX_SHADOW_CACHE_SIZE) {
	    shadow_cache = cairo_list_last_entry (shadow_caches,
						  cairo_shadow_cache_t,
						  link);
	    cairo_list_del (&shadow_cache->link);
	    cairo_surface_destroy (shadow_cache->surface);
	    *shadow_caches_size -= shadow_cache->size;
	    free (shadow_cache);
	}

	shadow_cache = malloc (sizeof (cairo_shadow_cache_t));
	cairo_list_init (&shadow_cache->link);
	shadow_cache->surface = cairo_surface_reference (cache_surface);
	shadow_cache->size = size;
	shadow_cache->hash = hash;
	shadow_cache->x_blur = x_blur;
	shadow_cache->y_blur = y_blur;
	shadow_cache->scale = scale;

	cairo_list_add (&shadow_cache->link, shadow_caches);
	*shadow_caches_size += size;

	shadow_pattern = cairo_pattern_create_for_surface (cache_surface);
	cairo_pattern_set_matrix (shadow_pattern, &m);

	cairo_pattern_destroy (color_pattern);
	color_pattern = cairo_pattern_create_rgba (shadow_copy.color.red,
					           shadow_copy.color.green,
					           shadow_copy.color.blue,
					           1.0);


	status = _cairo_surface_mask (target, op, color_pattern,
				      shadow_pattern, clip);
    }
    else {
	cairo_pattern_set_matrix (shadow_pattern, &m);
	status = _cairo_surface_mask (target, op,
				      color_pattern, shadow_pattern, clip);
    }

FINISH:
    _cairo_path_fixed_fini (&shadow_path);
    cairo_pattern_destroy (color_pattern);

    if (shadow_pattern)
	cairo_pattern_destroy (shadow_pattern);

    cairo_surface_destroy (cache_surface);

    cairo_surface_destroy (shadow_surface);

    if (locked)
	_cairo_image_surface_shadow_cache_release ((cairo_image_surface_t *)target);

    return status;
}

cairo_status_t
_cairo_surface_shadow_glyphs (cairo_surface_t		*target,
			      cairo_operator_t		 op,
			      const cairo_pattern_t	*source,
			      cairo_scaled_font_t	*scaled_font,
			      cairo_glyph_t		*glyphs,
			      int			 num_glyphs,
			      const cairo_clip_t	*clip,
			      const cairo_shadow_t	*shadow)
{
    cairo_status_t	  status;
    cairo_pattern_union_t shadow_source;
    cairo_rectangle_int_t shadow_extents;
    cairo_pattern_t 	 *shadow_pattern = NULL;
    cairo_pattern_t	 *color_pattern;
    cairo_surface_t	 *shadow_surface = NULL;
    cairo_surface_t	 *blur_surface = NULL;
    cairo_rectangle_int_t shadow_surface_extents;
    cairo_glyph_t        *shadow_glyphs;
    cairo_content_t       content; 

    int shadow_width, shadow_height;
    int x_blur, y_blur;
    cairo_shadow_t       shadow_copy = *shadow;

    cairo_matrix_t 	  m;
    double		  x_offset = shadow->x_offset;
    double		  y_offset = shadow->y_offset;

    if (shadow->type == CAIRO_SHADOW_NONE)
	return CAIRO_STATUS_SUCCESS;

    if (shadow->x_sigma <= 0.0 && shadow->y_sigma <= 0.0 &&
	shadow->x_offset == 0.0 && shadow->y_sigma == 0.0)
	return CAIRO_STATUS_SUCCESS;

    if (_cairo_clip_is_all_clipped (clip))
	return CAIRO_STATUS_SUCCESS;

    ((cairo_pattern_t *)source)->shadow.type = CAIRO_SHADOW_NONE;

    x_blur = ceil (shadow_copy.x_sigma * 2);
    y_blur = ceil (shadow_copy.y_sigma * 2);

    color_pattern = cairo_pattern_create_rgba (shadow_copy.color.red,
					       shadow_copy.color.green,
					       shadow_copy.color.blue,
					       shadow_copy.color.alpha);

    shadow_glyphs = (cairo_glyph_t *)_cairo_malloc_ab (num_glyphs,
						       sizeof (cairo_glyph_t));

    status = _cairo_surface_glyphs_get_offset_extents (target,
						       x_offset, y_offset,
						       source,
						       scaled_font,
						       glyphs,
						       num_glyphs,
						       clip,
						       &shadow_source.base,
						       shadow_glyphs,
						       &shadow_extents);
    if (unlikely (status))
	goto FINISH;

    if (shadow_extents.width == 0 && shadow_extents.height == 0)
	goto FINISH;

    x_offset = shadow_extents.x - x_blur;
    y_offset = shadow_extents.y - y_blur;

    shadow_width = shadow_extents.width + x_blur * 2;
    shadow_height = shadow_extents.height + y_blur * 2;

    if (target->backend->get_glyph_shadow_surface)
	shadow_surface = target->backend->get_glyph_shadow_surface (target,
						 		    shadow_width,
								    shadow_height,
								    FALSE);
    else {
	content = cairo_surface_get_content (target);
	if (content == CAIRO_CONTENT_COLOR)
	    content = CAIRO_CONTENT_COLOR_ALPHA;
	shadow_surface = cairo_surface_create_similar (target,
						       content,
						       shadow_width,
						       shadow_height);
    }
    if (unlikely (shadow_surface->status))
	goto FINISH;

    if(! _cairo_surface_get_extents (shadow_surface, &shadow_surface_extents))
	goto FINISH;

    cairo_matrix_init_translate (&m, -x_offset, -y_offset);

    /* paint with offset and scale */
    status = _cairo_surface_translate_glyphs (shadow_surface,
					      &m,
					      CAIRO_OPERATOR_OVER,
					      &shadow_source.base,
					      scaled_font,
					      shadow_glyphs,
					      num_glyphs,
					      NULL);

    if (unlikely (status))
	goto FINISH;

    shadow_pattern = cairo_pattern_create_for_surface (shadow_surface);
    cairo_pattern_set_filter (shadow_pattern, CAIRO_FILTER_GAUSSIAN);
    cairo_pattern_set_sigma (shadow_pattern, shadow_copy.x_sigma,
			     shadow_copy.y_sigma);

    status = _cairo_pattern_create_gaussian_matrix (shadow_pattern, 1024);
    if (unlikely (status))
	goto FINISH;

    if (op != CAIRO_OPERATOR_SOURCE) {
	cairo_pattern_set_matrix (shadow_pattern, &m);

	status = _cairo_surface_mask (target, op, color_pattern,
				      shadow_pattern, NULL);

    }
    else {
	cairo_matrix_t identity_matrix;
	cairo_pattern_t *clear_pattern;
 	cairo_pattern_t *blur_pattern;

	cairo_matrix_init_identity (&identity_matrix);

	if (target->backend->get_glyph_shadow_surface)
	    blur_surface = target->backend->get_glyph_shadow_surface (target,
						 		    shadow_width,
								    shadow_height,
								    TRUE);
	else
	    blur_surface = cairo_surface_create_similar (target,
						       cairo_surface_get_content (target),
						       shadow_width,
						       shadow_height);
	if (unlikely (blur_surface->status))
	    goto FINISH;

	/* paint with blur */
	/* FIXME: not efficient, we should only clear what is needed */
	clear_pattern = cairo_pattern_create_rgba (0, 0, 0, 0);
	status = _cairo_surface_paint (blur_surface, CAIRO_OPERATOR_SOURCE,
				       clear_pattern, NULL);
        cairo_pattern_destroy (clear_pattern);

	cairo_pattern_set_matrix (shadow_pattern, &identity_matrix);
 	status = _cairo_surface_paint (blur_surface,
				       CAIRO_OPERATOR_OVER,
				       shadow_pattern,
				       NULL);

	if (unlikely (status))
	    goto FINISH;

	/* paint blur back to target */
        blur_pattern = cairo_pattern_create_for_surface (blur_surface);
	cairo_pattern_set_matrix (blur_pattern, &m);

	status = _cairo_surface_mask (target, op, color_pattern,
				      blur_pattern, clip);
	cairo_pattern_destroy (blur_pattern);
    }
FINISH:
    cairo_pattern_destroy (color_pattern);

    if (shadow_pattern)
	cairo_pattern_destroy (shadow_pattern);


    free (shadow_glyphs);

    cairo_surface_destroy (shadow_surface);
    cairo_surface_destroy (blur_surface);

    return status;
}

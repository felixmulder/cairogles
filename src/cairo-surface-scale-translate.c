/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2005 Red Hat, Inc
 * Copyright © 2007 Adrian Johnson
 * Copyright © 2009 Chris Wilson
 * Copyright © 2013 Chris Wilson
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
 *	Henry Song <henry.song@samsung.com>
 */

#include "cairoint.h"

#include "cairo-clip-inline.h"
#include "cairo-error-private.h"
#include "cairo-pattern-private.h"
#include "cairo-surface-scale-translate-private.h"

/* A collection of routines to facilitate drawing to an alternate surface. */

static void
_copy_transformed_pattern (cairo_pattern_t *pattern,
			   const cairo_pattern_t *original,
			   const cairo_matrix_t  *ctm_inverse)
{
    _cairo_pattern_init_static_copy (pattern, original);

    if (! _cairo_matrix_is_identity (ctm_inverse))
	_cairo_pattern_transform (pattern, ctm_inverse);
}

static void
_transformed_pattern (cairo_pattern_t *pattern,
		      const cairo_matrix_t  *ctm_inverse)
{
    if (! _cairo_matrix_is_identity (ctm_inverse))
	_cairo_pattern_transform (pattern, ctm_inverse);
}

cairo_status_t
_cairo_surface_scale_translate_paint (cairo_surface_t	    *target,
				      const cairo_bool_t     clear_bg,
				      const cairo_matrix_t  *matrix,
				      cairo_operator_t	     op,
				      cairo_pattern_t *source,
				      const cairo_clip_t     *clip)
{
    cairo_status_t status;
    cairo_clip_t *dev_clip = NULL;
    cairo_matrix_t m;
    cairo_pattern_t *clear_pattern;

    if (unlikely (target->status))
	return target->status;

    if (_cairo_clip_is_all_clipped (clip))
	return CAIRO_STATUS_SUCCESS;

    if (! _cairo_matrix_is_identity (matrix)) {
	if (clip) {
	    dev_clip = _cairo_clip_copy (clip);
	    dev_clip = _cairo_clip_transform (dev_clip, matrix);
	}

	m = *matrix;
	status = cairo_matrix_invert (&m);
	_transformed_pattern (source, &m);
    }

    if (clear_bg) {
	clear_pattern = cairo_pattern_create_rgba (0, 0, 0, 0);
	status = _cairo_surface_paint (target, CAIRO_OPERATOR_SOURCE,
				       clear_pattern, dev_clip);
	cairo_pattern_destroy (clear_pattern);
    }

    status = _cairo_surface_paint (target, op, source, dev_clip);

    if (dev_clip && dev_clip != clip)
	_cairo_clip_destroy (dev_clip);

    return status;
}

cairo_private cairo_status_t
_cairo_surface_paint_get_offset_extents (cairo_surface_t *target,
					 double x_offset, double y_offset,
					 const cairo_pattern_t *source,
					 const cairo_clip_t *clip,
					 cairo_pattern_t *source_out,
					 cairo_rectangle_t *extents,
					 cairo_bool_t *bounded)
{
    cairo_matrix_t m;
    cairo_rectangle_t rect, temp;
    cairo_rectangle_int_t int_rect;

    if (unlikely (target->status))
	return target->status;

    if (_cairo_clip_is_all_clipped (clip)) {
	extents->x = extents->y = 0;
	extents->width = extents->height = 0;
	return CAIRO_STATUS_SUCCESS;
    }

    cairo_matrix_init_translate (&m, -x_offset, -y_offset);
    _copy_transformed_pattern (source_out, source, &m);

    _cairo_surface_get_extents (target, &int_rect);
    rect.x = int_rect.x;
    rect.y = int_rect.y;
    rect.width = int_rect.width;
    rect.height = int_rect.height;

    _cairo_pattern_get_exact_extents (source_out, &temp);
    _cairo_rectangle_exact_intersect (&rect, &temp);

    *bounded = TRUE;

    if (rect.width == _cairo_unbounded_rectangle.width ||
	rect.height == _cairo_unbounded_rectangle.height) {
	const cairo_rectangle_int_t *clip_extent = _cairo_clip_get_extents (clip);
	*bounded = FALSE;
	temp.x = clip_extent->x;
	temp.y = clip_extent->y;
	temp.width = clip_extent->width;
	temp.height = clip_extent->height;
	_cairo_rectangle_exact_intersect (&rect, &temp);
	if (rect.width == _cairo_unbounded_rectangle.width ||
	    rect.height == _cairo_unbounded_rectangle.height)
	    rect.width = rect.height = 0;
    }

    *extents = rect;

    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t
_cairo_surface_scale_translate_mask (cairo_surface_t *target,
				     const cairo_bool_t clear_bg,
				     const cairo_matrix_t *matrix,
				     cairo_operator_t	 op,
				     cairo_pattern_t *source,
				     cairo_pattern_t *mask,
				     const cairo_clip_t	    *clip)
{
    cairo_status_t status;
    cairo_clip_t *dev_clip = NULL;
    cairo_matrix_t m;
    cairo_pattern_t *clear_pattern;

    if (unlikely (target->status))
	return target->status;

    if (_cairo_clip_is_all_clipped (clip))
	return CAIRO_STATUS_SUCCESS;

    if (! _cairo_matrix_is_identity (matrix)) {
	if (clip) {
	    dev_clip = _cairo_clip_copy (clip);
	    dev_clip = _cairo_clip_transform (dev_clip, matrix);
	}

	m = *matrix;
	status = cairo_matrix_invert (&m);
	_transformed_pattern (source, &m);
	_transformed_pattern (mask, &m);
    }

    if (clear_bg) {
	clear_pattern = cairo_pattern_create_rgba (0, 0, 0, 0);
	status = _cairo_surface_paint (target, CAIRO_OPERATOR_SOURCE,
				       clear_pattern, dev_clip);
	cairo_pattern_destroy (clear_pattern);
    }

    status = _cairo_surface_mask (target, op,
				  source, mask,
				  dev_clip);

    if (dev_clip && dev_clip != clip)
	_cairo_clip_destroy (dev_clip);

    return status;
}

cairo_private cairo_status_t
_cairo_surface_mask_get_offset_extents (cairo_surface_t *target,
					double x_offset, double y_offset,
					const cairo_pattern_t *source,
					const cairo_pattern_t *mask,
					const cairo_clip_t *clip,
					cairo_pattern_t *source_out,
					cairo_pattern_t *mask_out,
					cairo_rectangle_t *extents,
					cairo_bool_t *bounded)
{
    cairo_matrix_t m;
    cairo_rectangle_t rect, temp;
    cairo_rectangle_int_t int_rect;

    if (unlikely (target->status))
	return target->status;

    if (_cairo_clip_is_all_clipped (clip)) {
	extents->x = extents->y = 0;
	extents->width = extents->height = 0;
	return CAIRO_STATUS_SUCCESS;
    }

    cairo_matrix_init_translate (&m, -x_offset, -y_offset);
    _copy_transformed_pattern (source_out, source, &m);
    _copy_transformed_pattern (mask_out, mask, &m);

    _cairo_surface_get_extents (target, &int_rect);
    rect.x = int_rect.x;
    rect.y = int_rect.y;
    rect.width = int_rect.width;
    rect.height = int_rect.height;

    _cairo_pattern_get_exact_extents (source_out, &temp);
    _cairo_rectangle_exact_intersect (&rect, &temp);

    _cairo_pattern_get_exact_extents (mask_out, &temp);
    _cairo_rectangle_exact_intersect (&rect, &temp);

    *bounded = TRUE;

    if (rect.width == _cairo_unbounded_rectangle.width ||
	rect.height == _cairo_unbounded_rectangle.height) {
	const cairo_rectangle_int_t *clip_extent = _cairo_clip_get_extents (clip);
	*bounded = FALSE;
	temp.x = clip_extent->x;
	temp.y = clip_extent->y;
	temp.width = clip_extent->width;
	temp.height = clip_extent->height;
	_cairo_rectangle_exact_intersect (&rect, &temp);

	if (rect.width == _cairo_unbounded_rectangle.width ||
	    rect.height == _cairo_unbounded_rectangle.height)
	    rect.width = rect.height = 0;
    }

    *extents = rect;

    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t
_cairo_surface_scale_translate_stroke (cairo_surface_t *surface,
				       const cairo_color_t *bg_color,
				       const cairo_matrix_t *matrix,
				       cairo_operator_t		 op,
				       cairo_pattern_t	*source,
				       cairo_path_fixed_t	*path,
			  	       const cairo_stroke_style_t*stroke_style,
				       const cairo_matrix_t	*ctm,
				       const cairo_matrix_t	*ctm_inverse,
				       double			 tolerance,
				       cairo_antialias_t	 antialias,
				       const cairo_clip_t	*clip)
{
    cairo_path_fixed_t *dev_path = (cairo_path_fixed_t *) path;
    cairo_clip_t *dev_clip = NULL;
    cairo_matrix_t dev_ctm = *ctm;
    cairo_matrix_t dev_ctm_inverse = *ctm_inverse;
    cairo_status_t status;
    cairo_matrix_t m;
    cairo_pattern_t *clear_pattern;
    cairo_stroke_style_t style_copy;
    double dash[2];

    if (unlikely (surface->status))
	return surface->status;

    if (_cairo_clip_is_all_clipped (clip))
	return CAIRO_STATUS_SUCCESS;

    memcpy (&style_copy, stroke_style, sizeof (cairo_stroke_style_t));

    if (! _cairo_matrix_is_identity (matrix)) {
	if (clip) {
	    dev_clip = _cairo_clip_copy (clip);
	    dev_clip = _cairo_clip_transform (dev_clip, matrix);
	}

	_cairo_path_fixed_transform (dev_path, matrix);

	cairo_matrix_multiply (&dev_ctm, &dev_ctm, matrix);

	m = *matrix;
	status = cairo_matrix_invert (&m);

	_transformed_pattern (source, &m);
	cairo_matrix_multiply (&dev_ctm_inverse, &m, &dev_ctm_inverse);

	if (_cairo_stroke_style_dash_can_approximate (&style_copy, matrix, tolerance)) {
	    style_copy.dash = dash;
	    _cairo_stroke_style_dash_approximate (stroke_style, matrix,
						  tolerance,
						  &style_copy.dash_offset,
						  style_copy.dash,
						  &style_copy.num_dashes);
	}
    }

    if (bg_color) {
	clear_pattern = _cairo_pattern_create_solid (bg_color);
	status = _cairo_surface_paint (surface, CAIRO_OPERATOR_SOURCE,
				   clear_pattern, dev_clip);
	cairo_pattern_destroy (clear_pattern);
    }

    status = _cairo_surface_stroke (surface, op, source,
				    dev_path, &style_copy,
				    &dev_ctm, &dev_ctm_inverse,
				    tolerance, antialias,
				    dev_clip);

    if (dev_clip && dev_clip != clip)
	_cairo_clip_destroy (dev_clip);
    return status;
}

cairo_private cairo_status_t
_cairo_surface_stroke_get_offset_extents (cairo_surface_t *target,
					  cairo_bool_t     is_inset,
					  double x_offset, double y_offset,
					  const cairo_pattern_t *source,
					  const cairo_path_fixed_t *path,
					  const cairo_stroke_style_t *stroke_style,
					  const cairo_matrix_t *ctm,
					  const cairo_matrix_t *ctm_inverse,
					  double tolerance,
					  const cairo_clip_t *clip,
					  cairo_pattern_t *source_out,
					  cairo_path_fixed_t *path_out,
					  cairo_matrix_t *ctm_out,
					  cairo_matrix_t *ctm_inverse_out,
					  cairo_rectangle_t *extents)
{
    cairo_status_t status;
    cairo_matrix_t m;
    cairo_rectangle_t rect, temp;

    if (unlikely (target->status))
	return target->status;

    if (_cairo_clip_is_all_clipped (clip)) {
	extents->x = extents->y = 0;
	extents->width = extents->height = 0;
	return CAIRO_STATUS_SUCCESS;
    }

    *ctm_out = *ctm;
    *ctm_inverse_out = *ctm_inverse;

    cairo_matrix_init_translate (&m, -x_offset, -y_offset);
    _copy_transformed_pattern (source_out, source, &m);

    status = _cairo_path_fixed_init_copy (path_out, path);
    if (unlikely (status))
	return status;

    if (x_offset != 0.0 || y_offset != 0.0) {
	cairo_matrix_multiply (ctm_inverse_out, ctm_inverse_out, &m);

	_cairo_path_fixed_translate (path_out,
				     _cairo_fixed_from_double (x_offset),
				     _cairo_fixed_from_double (y_offset));

	cairo_matrix_init_translate (&m, x_offset, y_offset);
	cairo_matrix_multiply (ctm_out, ctm_out, &m);
    }

    _cairo_pattern_get_exact_extents (source_out, &rect);

    if (stroke_style->line_join != CAIRO_LINE_JOIN_MITER)
	_cairo_path_fixed_approximate_stroke_exact_extents (path_out,
							    stroke_style,
							    ctm_out, &temp);
    else {
	status = _cairo_path_fixed_stroke_exact_extents (path_out,
							 stroke_style,
							 ctm_out,
							 ctm_inverse_out,
							 tolerance, &temp);
	if (unlikely (status)) {
	    extents->width = extents->height = 0;
	    return status;
	}
    }

    _cairo_rectangle_exact_intersect (&rect, &temp);

    if (is_inset) {
	rect.x -= x_offset;
	rect.y -= y_offset;
	rect.width += fabs (x_offset);
	rect.height += fabs (y_offset);
    }
    *extents = rect;

    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t
_cairo_surface_scale_translate_fill (cairo_surface_t	*surface,
				     const cairo_color_t *bg_color,
				     const cairo_matrix_t *matrix,
				     cairo_operator_t	 op,
				     cairo_pattern_t    *source,
				     cairo_path_fixed_t	*path,
				     cairo_fill_rule_t	 fill_rule,
				     double		 tolerance,
				     cairo_antialias_t	 antialias,
				     const cairo_clip_t	 *clip)
{
    cairo_status_t status;
    cairo_path_fixed_t *dev_path = (cairo_path_fixed_t *) path;
    cairo_clip_t *dev_clip = NULL;
    cairo_matrix_t m;
    cairo_pattern_t *clear_pattern;

    if (unlikely (surface->status))
	return surface->status;

    if (_cairo_clip_is_all_clipped (clip))
	return CAIRO_STATUS_SUCCESS;

    if (! _cairo_matrix_is_identity (matrix)) {
	if (clip) {
	    dev_clip = _cairo_clip_copy (clip);
	    dev_clip = _cairo_clip_transform (dev_clip, matrix);
	}

	_cairo_path_fixed_transform (dev_path, matrix);

	m = *matrix;
	status = cairo_matrix_invert (&m);
	_transformed_pattern (source, &m);
    }

    if (bg_color) {
	clear_pattern = _cairo_pattern_create_solid (bg_color);
	status = _cairo_surface_paint (surface, CAIRO_OPERATOR_SOURCE,
				   clear_pattern, dev_clip);
	cairo_pattern_destroy (clear_pattern);
    }

    status = _cairo_surface_fill (surface, op, source,
				  dev_path, fill_rule,
				  tolerance, antialias,
				  dev_clip);

    if (dev_clip && dev_clip != clip)
	_cairo_clip_destroy (dev_clip);

    return status;
}

cairo_private cairo_status_t
_cairo_surface_fill_get_offset_extents (cairo_surface_t *target,
					cairo_bool_t     is_inset,
					double x_offset, double y_offset,
					const cairo_pattern_t *source,
					const cairo_path_fixed_t *path,
					const cairo_fill_rule_t fill_rule,
					const cairo_clip_t *clip,
					cairo_pattern_t *source_out,
					cairo_path_fixed_t *path_out,
					cairo_rectangle_t *extents)
{
    cairo_status_t status;
    cairo_matrix_t m;
    cairo_rectangle_t rect, temp;
    const cairo_rectangle_int_t *clip_rect;

    if (unlikely (target->status))
	return target->status;

    if (_cairo_clip_is_all_clipped (clip)) {
	extents->x = extents->y = 0;
	extents->width = extents->height = 0;
	return CAIRO_STATUS_SUCCESS;
    }

    cairo_matrix_init_translate (&m, -x_offset, -y_offset);
    _copy_transformed_pattern (source_out, source, &m);

    status = _cairo_path_fixed_init_copy (path_out, path);
    if (unlikely (status))
	return status;

    if (x_offset != 0.0 || y_offset != 0.0) {
	_cairo_path_fixed_translate (path_out,
				     _cairo_fixed_from_double (x_offset),
				     _cairo_fixed_from_double (y_offset));
    }

    _cairo_pattern_get_exact_extents (source_out, &rect);

    if (! source->shadow.path_is_fill_with_spread) {
	_cairo_path_fixed_approximate_fill_exact_extents (path_out, &temp);
	_cairo_rectangle_exact_intersect (&rect, &temp);
    }
    else {
	clip_rect = _cairo_clip_get_extents (clip);
	temp.x = clip_rect->x;
	temp.y = clip_rect->y;
	temp.width = clip_rect->width;
	temp.height = clip_rect->height;
	_cairo_rectangle_exact_intersect (&rect, &temp);
    }

    if (is_inset) {
	rect.x -= x_offset;
	rect.y -= y_offset;
	rect.width += abs (x_offset);
	rect.height += abs (y_offset);
    }
    *extents = rect;

    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t
_cairo_surface_translate_glyphs (cairo_surface_t 	*surface,
				 const cairo_color_t    *bg_color,
		 		 const cairo_matrix_t 	*matrix,
				 cairo_operator_t	 op,
				 cairo_pattern_t	*source,
				 cairo_scaled_font_t	*scaled_font,
				 cairo_glyph_t		*glyphs,
				 int			 num_glyphs,
				 const cairo_clip_t	*clip)
{
    cairo_status_t status;
    cairo_clip_t *dev_clip = (cairo_clip_t *) clip;
    cairo_glyph_t *dev_glyphs = glyphs;
    cairo_pattern_t *clear_pattern;
    int i;
    cairo_matrix_t inverse_matrix;

    if (unlikely (surface->status))
	return surface->status;

    if (_cairo_clip_is_all_clipped (clip))
	return CAIRO_STATUS_SUCCESS;

    inverse_matrix = *matrix;
    status = cairo_matrix_invert (&inverse_matrix);
    if (unlikely (status))
	return status;

    if (! _cairo_matrix_is_identity (matrix)) {
	dev_clip = _cairo_clip_copy_with_translation (clip, matrix->x0,
						      matrix->y0);

	_transformed_pattern (source, matrix);

	for (i = 0; i < num_glyphs; i++) {
	    dev_glyphs[i].x += matrix->x0;
	    dev_glyphs[i].y += matrix->y0;
	}
    }

    if (bg_color) {
	clear_pattern = _cairo_pattern_create_solid (bg_color);
	status = _cairo_surface_paint (surface, CAIRO_OPERATOR_SOURCE,
				   clear_pattern, NULL);
	cairo_pattern_destroy (clear_pattern);
    }

    status = _cairo_surface_show_text_glyphs (surface, op, source,
					      NULL, 0,
					      dev_glyphs, num_glyphs,
					      NULL, 0, 0,
					      scaled_font,
					      dev_clip);

    if (dev_clip != clip)
	_cairo_clip_destroy (dev_clip);

    _transformed_pattern (source, &inverse_matrix);
    return status;
}

cairo_private cairo_status_t
_cairo_surface_glyphs_get_offset_extents (cairo_surface_t *target,
					  cairo_bool_t    is_inset,
					  double x_offset, double y_offset,
					  const cairo_pattern_t *source,
					  cairo_scaled_font_t *scaled_font,
					  const cairo_glyph_t *glyphs,
					  int                 num_glyphs,
					  const cairo_clip_t *clip,
					  cairo_pattern_t *source_out,
					  cairo_glyph_t *glyphs_out,
					  cairo_rectangle_t *extents)
{
    cairo_matrix_t m;
    cairo_rectangle_t rect, temp;
    cairo_rectangle_int_t int_rect;
    const cairo_rectangle_int_t *clip_rect;
    int i;
    cairo_bool_t result;

    if (unlikely (target->status))
	return target->status;

    if (_cairo_clip_is_all_clipped (clip)) {
	extents->x = extents->y = 0;
	extents->width = extents->height = 0;
	return CAIRO_STATUS_SUCCESS;
    }

    memcpy (glyphs_out, glyphs, sizeof (cairo_glyph_t) * num_glyphs);

    cairo_matrix_init_translate (&m, -x_offset, -y_offset);
    _copy_transformed_pattern (source_out, source, &m);

    if (x_offset != 0.0) {
	for (i = 0; i < num_glyphs; i++)
	    glyphs_out[i].x += x_offset;
    }

    if (y_offset != 0.0) {
	for (i = 0; i < num_glyphs; i++)
	    glyphs_out[i].y += y_offset;
    }

    _cairo_surface_get_extents (target, &int_rect);
    clip_rect = _cairo_clip_get_extents (clip);
    _cairo_rectangle_intersect (&int_rect, clip_rect);

    rect.x = int_rect.x;
    rect.y = int_rect.y;
    rect.width = int_rect.width;
    rect.height = int_rect.height;

    _cairo_pattern_get_exact_extents (source_out, &temp);
    _cairo_rectangle_exact_intersect (&rect, &temp);

    result = _cairo_scaled_font_glyph_approximate_extents (scaled_font,
							   glyphs_out,
							   num_glyphs,
							   &int_rect);
    if (! result)
	return CAIRO_STATUS_USER_FONT_ERROR;

    temp.x = int_rect.x;
    temp.y = int_rect.y;
    temp.width = int_rect.width;
    temp.height = int_rect.height;
    _cairo_rectangle_exact_intersect (&rect, &temp);
    *extents = rect;

    return CAIRO_STATUS_SUCCESS;
}

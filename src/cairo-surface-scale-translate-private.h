/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2002 University of Southern California
 * Copyright © 2005 Red Hat, Inc.
 * Copyright © 2009 Chris Wilson
 * Copyright © 2013 Henry Song
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
 *	Henry Song <henry.song@samsung.h>
 */

#ifndef CAIRO_SURFACE_SCALE_TRANSLATE_PRIVATE_H
#define CAIRO_SURFACE_SCALE_TRANSLATE_PRIVATE_H

#include "cairo-types-private.h"

CAIRO_BEGIN_DECLS

cairo_private cairo_status_t
_cairo_surface_scale_translate_paint (cairo_surface_t *target,
				      const cairo_bool_t clear_bg,
				      const cairo_matrix_t *matrix,
				      cairo_operator_t	 op,
				      cairo_pattern_t *source,
				      const cairo_clip_t   *clip);

cairo_private cairo_status_t
_cairo_surface_paint_get_offset_extents (cairo_surface_t *target,
					 double x_offset, double y_offset,
					 const cairo_pattern_t *source,
					 const cairo_clip_t *clip,
					 cairo_pattern_t *source_out,
					 cairo_rectangle_t *extents,
					 cairo_bool_t *bounded);

cairo_private cairo_status_t
_cairo_surface_scale_translate_mask (cairo_surface_t *target,
				     const cairo_bool_t clear_bg,
				     const cairo_matrix_t *matrix,
				     cairo_operator_t	 op,
				     cairo_pattern_t *source,
				     cairo_pattern_t *mask,
				     const cairo_clip_t	    *clip);

cairo_private cairo_status_t
_cairo_surface_mask_get_offset_extents (cairo_surface_t *target,
					double x_offset, double y_offset,
					const cairo_pattern_t *source,
					const cairo_pattern_t *mask,
					 const cairo_clip_t *clip,
					 cairo_pattern_t *source_out,
					 cairo_pattern_t *mask_out,
					 cairo_rectangle_t *extents,
					 cairo_bool_t *bounded);

cairo_private cairo_status_t
_cairo_surface_scale_translate_stroke (cairo_surface_t *surface,
				       const cairo_color_t      *bg_color,
				       const cairo_matrix_t *matrix,
				       cairo_operator_t		 op,
				       cairo_pattern_t	*source,
				       cairo_path_fixed_t	*path,
			  	       const cairo_stroke_style_t*stroke_style,
				       const cairo_matrix_t	*ctm,
				       const cairo_matrix_t	*ctm_inverse,
				       double			 tolerance,
				       cairo_antialias_t	 antialias,
				       const cairo_clip_t	*clip);

cairo_private cairo_status_t
_cairo_surface_stroke_get_offset_extents (cairo_surface_t *target,
					  cairo_bool_t is_inset,
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
					  cairo_rectangle_t *extents);

cairo_private cairo_status_t
_cairo_surface_scale_translate_fill (cairo_surface_t	*surface,
				     const cairo_color_t      *bg_color,
				     const cairo_matrix_t *matrix,
				     cairo_operator_t	 op,
				     cairo_pattern_t    *source,
				     cairo_path_fixed_t	*path,
				     cairo_fill_rule_t	 fill_rule,
				     double		 tolerance,
				     cairo_antialias_t	 antialias,
				     const cairo_clip_t	 *clip);

cairo_private cairo_status_t
_cairo_surface_fill_get_offset_extents (cairo_surface_t *target,
					cairo_bool_t    is_inset,
					double x_offset, double y_offset,
					const cairo_pattern_t *source,
					const cairo_path_fixed_t *path,
					const cairo_fill_rule_t fill_rule,
					const cairo_clip_t *clip,
					cairo_pattern_t *source_out,
					cairo_path_fixed_t *path_out,
					cairo_rectangle_t *extents);

cairo_private cairo_status_t
_cairo_surface_translate_glyphs (cairo_surface_t 	*surface,
				 const cairo_color_t    *bg_color,
		 		 const cairo_matrix_t 	*matrix,
				 cairo_operator_t	 op,
				 cairo_pattern_t	*source,
				 cairo_scaled_font_t	*scaled_font,
				 cairo_glyph_t		*glyphs,
				 int			 num_glyphs,
				 const cairo_clip_t	*clip);

cairo_private cairo_status_t
_cairo_surface_glyphs_get_offset_extents (cairo_surface_t *target,
					  cairo_bool_t     is_inset,
					  double x_offset, double y_offset,
					  const cairo_pattern_t *source,
					  cairo_scaled_font_t *scaled_font,
					  const cairo_glyph_t *glyphs,
					  int                 num_glyphs,
					  const cairo_clip_t *clip,
					  cairo_pattern_t *source_out,
					  cairo_glyph_t *glyphs_out,
					  cairo_rectangle_t *extents);

#endif /* CAIRO_SURFACE_SCALE_TRANSLATE_PRIVATE_H */

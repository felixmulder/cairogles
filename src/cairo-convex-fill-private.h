/* cairo - a vector graphics library with display and print output
 *
 * Copyright © 2014 Samsung Research America, Silicon Valley
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
 * Srikanth Chevalam<chevalam.s@samsung.com>
 * Suyambulingam Rathinasamy<suyambu.rm@samsung.com>
 * Henry Song <henry.song@samsung.com>
 */
#ifndef __CAIRO_CONVEX_FILL_H_
#define __CAIRO_CONVEX_FILL_H_

#include "cairo-box-inline.h"
#include "cairo-list-inline.h"
#include "cairo-stroke-dash-private.h"
#include "cairo-slope-private.h"

#define NUM_SPLINE_CALLS 64

typedef struct cairo_convex_fill_closure {
	cairo_status_t (*add_triangle_fan) (void *closure,
					    const cairo_point_t *points,
					    int npoints);
	double tolerance;
	cairo_point_t current_point;
	cairo_point_t start_point;
	void *closure;
	unsigned int pcount;
	unsigned int capacity;
        cairo_bool_t midp_added;
        cairo_point_t embedded_points[NUM_SPLINE_CALLS];
	cairo_point_t *convex_points;
} cairo_convex_fill_closure_t;

typedef cairo_warn cairo_status_t
(*cairo_convex_fill_closure_add_triangle_fan) (void *closure,
					       const cairo_point_t *points,
					       int npoints);

cairo_status_t
_cairo_convex_fill_spline_to (void *closure,
			      const cairo_point_t *point,
			      const cairo_slope_t *tangent);


cairo_status_t
_cairo_convex_fill_curve_to (void *closure,
			     const cairo_point_t *b,
			     const cairo_point_t *c,
			     const cairo_point_t *d);

cairo_status_t
_cairo_convex_fill_move_to (void *closure,
			    const cairo_point_t *point);

cairo_status_t
_cairo_path_fixed_fill_to_convex (cairo_convex_fill_closure_add_triangle_fan add_triangle_fan,
				  const cairo_path_fixed_t       *path,
				  cairo_path_fixed_move_to_func_t    *move_to,
				  cairo_path_fixed_line_to_func_t    *line_to,
				  cairo_path_fixed_curve_to_func_t   *curve_to,
				  cairo_path_fixed_close_path_func_t *close_path,
				  void               *closure);


cairo_status_t
_cairo_path_fixed_convex_fill_interpret (const cairo_path_fixed_t       *path,
					 cairo_path_fixed_move_to_func_t *move_to,
					 cairo_path_fixed_line_to_func_t *line_to,
					 cairo_path_fixed_curve_to_func_t    *curve_to,
					 cairo_path_fixed_close_path_func_t  *close_path,
					 void                *closure);

cairo_status_t
_cairo_convex_fill_line_to (void *closure,
			    const cairo_point_t *point);

cairo_status_t
_cairo_convex_fill_close_path (void *closure);

cairo_status_t
_add_triangle (void           *closure,
	       const cairo_point_t     triangle[3]);

#endif

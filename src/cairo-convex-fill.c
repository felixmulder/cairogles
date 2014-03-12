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

#include "cairo-convex-fill-private.h"
#include <sys/time.h>

cairo_status_t
_add_triangle (void           *closure,
               const cairo_point_t*     triangle)
{
    cairo_convex_fill_closure_t *filler = closure;
    int required_space = 1;
    if (! filler->midp_added)
	required_space = 2;

    if(filler->capacity < filler->pcount + required_space) {
        filler->capacity += NUM_SPLINE_CALLS;
	if (filler->convex_points == filler->embedded_points) {
	    filler->convex_points = _cairo_malloc_ab (filler->capacity, sizeof(cairo_point_t));
	    memcpy (filler->convex_points, filler->embedded_points, sizeof(cairo_point_t) * NUM_SPLINE_CALLS);
	}
	else
	    filler->convex_points = _cairo_realloc_ab (filler->convex_points, filler->capacity, sizeof (cairo_point_t));
    }
    
    if (! filler->midp_added) {
	filler->convex_points[filler->pcount++] = filler->start_point;
	filler->midp_added = TRUE;
    }
    filler->convex_points[filler->pcount++] = *triangle;
    
    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t
_cairo_convex_fill_spline_to (void *closure,
			      const cairo_point_t *point,
                              const cairo_slope_t *tangent)
{
    cairo_convex_fill_closure_t *filler = closure;
    if (filler->current_point.x == point->x &&
        filler->current_point.y == point->y)
            return CAIRO_STATUS_SUCCESS;

    if ((filler->current_point.x != point->x ||
         filler->current_point.y != point->y))
            _add_triangle (filler, point);

    filler->current_point = *point;
    return CAIRO_STATUS_SUCCESS;
}


cairo_status_t
_cairo_convex_fill_move_to (void *closure,
			    const cairo_point_t *point)
{
    cairo_convex_fill_closure_t *path = closure;
    path->current_point = *point;
    path->start_point = path->current_point;
    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t
_cairo_convex_fill_line_to (void *closure,
			    const cairo_point_t *point)
{
    cairo_convex_fill_closure_t *filler = closure;

    _add_triangle(filler, point);

    filler->current_point = *point;

    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t
_cairo_convex_fill_close_path (void *closure)
{
    cairo_convex_fill_closure_t *filler = closure;
    _cairo_convex_fill_line_to (closure, &filler->start_point);
    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t
_cairo_path_fixed_fill_to_convex (cairo_convex_fill_closure_add_triangle_fan add_triangle_fan,
				  const cairo_path_fixed_t       *path,
				  cairo_path_fixed_move_to_func_t    *move_to,
				  cairo_path_fixed_line_to_func_t    *line_to,
				  cairo_path_fixed_curve_to_func_t   *curve_to,
				  cairo_path_fixed_close_path_func_t *close_path,
				  void               *closure)
{
    cairo_convex_fill_closure_t *filler = closure ;
    cairo_status_t status = CAIRO_STATUS_SUCCESS;
    filler->current_point = path->current_point;
    filler->start_point = filler->current_point;
    filler->add_triangle_fan = add_triangle_fan;
    filler->pcount =  0 ;
    filler->capacity = NUM_SPLINE_CALLS;
    filler->convex_points = filler->embedded_points;
    //filler->convex_points = _cairo_malloc_ab (filler->capacity, sizeof(cairo_point_t));
    status = _cairo_path_fixed_convex_fill_interpret (path,
 						      move_to,
						      line_to,
						      curve_to,
 						      close_path,
						      filler);
    return status;
}

cairo_status_t
_cairo_convex_fill_curve_to (void *closure,
			     const cairo_point_t *b,
			     const cairo_point_t *c,
			     const cairo_point_t *d)
{
    cairo_convex_fill_closure_t *filler = closure;
    cairo_spline_t spline;
    cairo_status_t status = CAIRO_STATUS_SUCCESS;
    if (! _cairo_spline_init (&spline,
                (cairo_spline_add_point_func_t) _cairo_convex_fill_spline_to,
                filler,
                &filler->current_point, b, c, d))
        return _cairo_convex_fill_line_to (closure, d);

    status = _cairo_spline_decompose (&spline, filler->tolerance);

    if (unlikely (status))
	return status;
    return CAIRO_STATUS_SUCCESS;
}

cairo_status_t
_cairo_path_fixed_convex_fill_interpret (const cairo_path_fixed_t *path,
					 cairo_path_fixed_move_to_func_t *move_to,
					 cairo_path_fixed_line_to_func_t *line_to,
					 cairo_path_fixed_curve_to_func_t *curve_to,
					 cairo_path_fixed_close_path_func_t	*close_path,
					 void				*closure)
{
    const cairo_path_buf_t *buf;
    cairo_status_t status;
    cairo_convex_fill_closure_t *filler = closure;

    cairo_path_foreach_buf_start (buf, path) {
    const cairo_point_t *points = buf->points;
    unsigned int i;

    for (i = 0; i < buf->num_ops; i++) {
        switch (buf->op[i]) {
        case CAIRO_PATH_OP_MOVE_TO:
        status = (*move_to) (closure, &points[0]);
        points += 1;
        break;
        case CAIRO_PATH_OP_LINE_TO:
        status = (*line_to) (closure, &points[0]);
        points += 1;
        break;
        case CAIRO_PATH_OP_CURVE_TO:
        status = (*curve_to) (closure, &points[0], &points[1], &points[2]);
        points += 3;
        break;
        default:
        ASSERT_NOT_REACHED;
        case CAIRO_PATH_OP_CLOSE_PATH:
        status = (*close_path) (closure);
        break;
        }

        if (unlikely (status))
            return status;
    }
    } cairo_path_foreach_buf_end (buf, path);

    if (filler->pcount) {
	filler->add_triangle_fan (filler->closure,
				  filler->convex_points,
				  filler->pcount);
    }
    if (filler->convex_points != filler->embedded_points)
	free (filler->convex_points);

   return status;
}

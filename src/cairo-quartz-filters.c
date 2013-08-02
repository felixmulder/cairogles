/* -*- Mode: c; c-basic-offset: 4; indent-tabs-mode: t; tab-width: 8; -*- */
/* cairo - a vector graphics library with display and print output
 *
 * Copyright � 2013 Samsung Research America - Silicon Valley
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
 * The Initial Developer of the Original Code is Mozilla Foundation.
 *
 * Contributor(s):
 *	Henry Song <henry.song@samsung.com>
 */

#define _GNU_SOURCE /* required for RTLD_DEFAULT */
#include "cairoint.h"
#include "cairo-pattern-private.h"
#include "cairo-quartz-private.h"
#include "cairo-quartz.h"
#include <Accelerate/Accelerate.h>

#define CAIRO_QUARTZ_MAX_SCALE 4

static int16_t *
_cairo_quartz_pattern_create_gaussian_matrix (const cairo_pattern_t *pattern,
					      int *row, int *col,
					      int *sum,
					      int *shrink_x, int *shrink_y)
{
    double x_sigma, y_sigma;
    double x_sigma_sq, y_sigma_sq;
    int n;
    double *buffer;
    int16_t *i_buffer;
    int i, x, y;
    double u, v;
    double u1, v1;
    int x_radius, y_radius;
    int i_row, i_col;
    int x_factor, y_factor;
    cairo_rectangle_int_t extents;
    int width, height;
    int max_factor;
    double max_sigma;

    max_factor = CAIRO_QUARTZ_MAX_SCALE;;
    max_sigma = CAIRO_MAX_SIGMA;

    width = CAIRO_MIN_SHRINK_SIZE;
    height = CAIRO_MIN_SHRINK_SIZE;

    if (_cairo_surface_get_extents (((cairo_surface_pattern_t *)pattern)->surface, &extents)) {
	width = extents.width;
	height = extents.height;
    }

    x_factor = y_factor = 1;
    x_sigma = pattern->x_sigma;
    y_sigma = pattern->y_sigma;

    /* no blur */
    if (x_sigma == 0.0 && y_sigma == 0.0) {
	return NULL;
    }

    if (x_sigma == 0.0)
        x_radius = 0;
    else {
	while (x_sigma >= max_sigma) {
	    if (width <= CAIRO_MIN_SHRINK_SIZE || x_factor >= max_factor)
	        break;

	    x_sigma *= 0.5;
	    x_factor *= 2;
	    width *= 0.5;
	}
    }

    if (y_sigma == 0.0)
        y_radius = 0;
    else {
	while (y_sigma >= max_sigma) {
	    if (height <= CAIRO_MIN_SHRINK_SIZE || y_factor >= max_factor)
	        break;

	    y_sigma *= 0.5;
	    y_factor *= 2;
	    height *= 0.5;
	}
    }

    /* 2D gaussian
     * f(x, y) = exp (-((x-x0)^2/(2*x_sigma^2)+(y-y0)^2/(2*y_sigma*2)))
     */
    x_radius = x_sigma * 2;
    y_radius = y_sigma * 2;

    i_row = y_radius;
    i_col = x_radius;
    n = (2 * i_row + 1) * (2 * i_col + 1);

    x_sigma_sq = 2 * x_sigma * x_sigma;
    y_sigma_sq = 2 * y_sigma * y_sigma;

    buffer = _cairo_malloc_ab (n, sizeof (double));
    if (! buffer)
	return NULL;
    i_buffer = _cairo_malloc_ab (n, sizeof (i_buffer));
    if (! i_buffer) {
	free (buffer);
	return NULL;
    }
    i = 0;
    *sum = 0;

    for (y = -i_row; y <= i_row; y++) {
        for (x = -i_col; x <= i_col; x++) {
            u = x * x;
            v = y * y;
	    if (u == 0.0)
		u1 = 0.0;
	    else
		u1 = u / x_sigma_sq;

	    if (v == 0.0)
		v1 = 0.0;
	    else
		v1 = v / y_sigma_sq;
            buffer[i] = exp (-(u1 + v1));
	    i_buffer[i] = ceil (buffer[i] - 0.5);
            *sum += i_buffer[i];
            i++;
        }
    }

    free (buffer);

    *row = i_row * 2 + 1;
    *col = i_col * 2 + 1;
    *shrink_x = x_factor;
    *shrink_y = y_factor;

    return i_buffer;
}

#if __MAC_OS_X_VERSION_MIN_REQUIRED < 1050
static CGContextRef
_cairo_quartz_get_image_context (CGImageRef image)
{
    int width, height;
    int bytes_per_row;
    CGContextRef context;
    CGColorSpaceRef color_space;
    CGBitmapInfo bitmap_info;
    CGRect size;

    void *buffer;

    if (image == NULL)
	return NULL;

    width = CGImageGetWidth (image);
    height = CGImageGetHeight (image);
    bytes_per_row = CGImageGetBytesPerRow (image);

    color_space = CGImageGetColorSpace (image);
    buffer = malloc (sizeof (char) * bytes_per_row * height);
    if (! buffer)
	return NULL;

    bitmap_info = CGImageGetBitmapInfo (image);

    /* create output image bitmap context */
    context = CGBitmapContextCreate (buffer, width, height,
				     CGImageGetBitsPerComponent (image),
				     bytes_per_row,
				     color_space,
				     bitmap_info);

    if (! context) {
	free (buffer);
	return NULL;
    }

    size = CGRectMake (0, 0, width, height);

    CGContextDrawImage (context, size, image);

    return context;
}
#endif

static cairo_int_status_t
_cairo_quartz_resize_image (CGImageRef src, double x_resize_factor,
			    double y_resize_factor, CGImageRef *out)
{
    int width, height;
    int bytes_per_row;
    int bytes_per_pixel;
    CGContextRef out_bitmap_context;
    CGColorSpaceRef color_space;
    CGBitmapInfo bitmap_info;
    CGRect size;

    void *buffer;

    if (src == NULL)
	return CAIRO_INT_STATUS_UNSUPPORTED;

    if (x_resize_factor <= 0.0 ||
	y_resize_factor <= 0.0)
	return CAIRO_INT_STATUS_UNSUPPORTED;

    width = CGImageGetWidth (src) * x_resize_factor;
    height = CGImageGetHeight (src) * y_resize_factor;
    bytes_per_pixel = CGImageGetBytesPerRow (src) / CGImageGetWidth (src);

    color_space = CGImageGetColorSpace (src);
    bytes_per_row = bytes_per_pixel * width;
    buffer = malloc (sizeof (char) * bytes_per_row * height);
    if (! buffer)
	return CAIRO_INT_STATUS_NO_MEMORY;

    bitmap_info = CGImageGetBitmapInfo (src);

    /* create output image bitmap context */
    out_bitmap_context = CGBitmapContextCreate (buffer, width, height,
						CGImageGetBitsPerComponent (src),
						bytes_per_row,
						color_space,
						bitmap_info);

    size = CGRectMake (0, 0, width, height);

    CGContextDrawImage (out_bitmap_context, size, src);

    *out = CGBitmapContextCreateImage (out_bitmap_context);

    /* clean up */
    CGContextRelease (out_bitmap_context);
    free (buffer);

    return CAIRO_INT_STATUS_SUCCESS;
}

static cairo_int_status_t
_cairo_quartz_convolve_pass (vImage_Buffer *src, 
			     const int16_t *kernel,
			     int kernel_width, int kernel_height,
			     const int32_t divisor,
			     unsigned char *edge_fill,
			     vImage_Buffer *dst)
{
    vImage_Error error;

    dst->data = malloc (src->rowBytes * src->height);
    if (! dst->data)
	return CAIRO_INT_STATUS_NO_MEMORY;

    dst->width = src->width;
    dst->height = src->height;
    dst->rowBytes = src->rowBytes;

    /* we always use background color beyond edge */
    error = vImageConvolve_ARGB8888 (src, dst, NULL, /* no temp buffer */
				     0, 0,
				     kernel, kernel_width, kernel_height,
				     divisor,
				     edge_fill,
				     kvImageNoFlags);

    if (error != kvImageNoError)
	return CAIRO_INT_STATUS_UNSUPPORTED;

    return CAIRO_INT_STATUS_SUCCESS;
}
    
cairo_status_t
_cairo_quartz_gaussian_filter (const cairo_pattern_t *src,
			       const CGImageRef image,
			       CGImageRef *out_image)
{
    cairo_int_status_t status = CAIRO_INT_STATUS_SUCCESS;

    vImage_Buffer src_buffer, dst_buffer;
    int16_t *kernel = NULL;
    int32_t divisor;
    int shrink_factor_x;
    int shrink_factor_y;

#if __MAC_OS_X_VERSION_MIN_REQUIRED < 1050
    CGContextRef image_ctx;
#else
    CGDataProviderRef image_provider;
    CFDataRef image_data_ref;
#endif
    CGImageRef resized_image;
    CGImageRef resized_out_image;

    CGContextRef ctx;
    CGColorSpaceRef color_space;
    CGBitmapInfo bitmap_info;

    int row, col;
    unsigned char edge_color[4] = {0, 0, 0, 0};

    if (src->type != CAIRO_PATTERN_TYPE_SURFACE ||
	! src->convolution_matrix) {
	*out_image = CGImageRetain (image);
	return CAIRO_INT_STATUS_SUCCESS;
    } 

    /* re-compute scaling */
    kernel = _cairo_quartz_pattern_create_gaussian_matrix ((cairo_pattern_t *)src,
							    &row, &col,
							    &divisor,
							    &shrink_factor_x,
							    &shrink_factor_y);
    if (! kernel) {
	*out_image = NULL;
	return CAIRO_INT_STATUS_NO_MEMORY;
    }

    if (shrink_factor_x == 1 &&
	shrink_factor_y == 1)
	resized_image = CGImageRetain (image);
    else {
	status = _cairo_quartz_resize_image (image,
					     1.0 / src->shrink_factor_x,
					     1.0 / src->shrink_factor_y,
					     &resized_image);
	if (unlikely (status)) {
	    free (kernel);
	    *out_image = NULL;
	    return status;
	}
    }

#if __MAC_OS_X_VERSION_MIN_REQUIRED < 1050
    image_ctx = _cairo_quartz_get_image_context (resized_image);
    if (! image_ctx) {
	free (kernel);
	*out_image = NULL;
 	return CAIRO_INT_STATUS_NO_MEMORY;
    }
#else
    image_provider = CGImageGetDataProvider (resized_image);
    image_data_ref = CGDataProviderCopyData (image_provider);
#endif

    src_buffer.width = CGImageGetWidth (resized_image);
    src_buffer.height = CGImageGetHeight (resized_image);
    src_buffer.rowBytes = CGImageGetBytesPerRow (resized_image);

#if __MAC_OS_X_VERSION_MIN_REQUIRED < 1050
    src_buffer.data = CGBitmapContextGetData (image_ctx);
    if (! src_buffer.data) {
	free (kernel);
	CGContextRelease (image_ctx);
	*out_image = NULL;
	return CAIRO_INT_STATUS_NO_MEMORY;
    }
#else
    src_buffer.data = (void *) CFDataGetBytePtr (image_data_ref);
#endif

    dst_buffer.data = NULL;

    status = _cairo_quartz_convolve_pass (&src_buffer,
					  kernel,
					  col, row,
					  divisor,
					  edge_color,
					  &dst_buffer);

#if __MAC_OS_X_VERSION_MIN_REQUIRED < 1050
    CGContextRelease (image_ctx);
    free (src_buffer.data);
#else
    CFRelease (image_data_ref);
#endif

    free (kernel);
    CGImageRelease (resized_image);

    if (unlikely (status)) {
	if (dst_buffer.data)
	    free (dst_buffer.data);
	*out_image = NULL;
	return status;
    }

    /* create resized_out_image from blur */
    color_space = CGImageGetColorSpace (resized_image);
    bitmap_info = CGImageGetBitmapInfo (resized_image);

    ctx = CGBitmapContextCreate (dst_buffer.data,
				 dst_buffer.width,
				 dst_buffer.height,
				 CGImageGetBitsPerComponent (resized_image),
				 dst_buffer.rowBytes,
				 color_space,
				 bitmap_info);

    resized_out_image = CGBitmapContextCreateImage (ctx);

    CGContextRelease (ctx);
    free (dst_buffer.data);

    /* scale back from resized_out_image to out_image */
    if (shrink_factor_x == 1 &&
	shrink_factor_y == 1) {
	*out_image = resized_out_image;
	return CAIRO_INT_STATUS_SUCCESS;
    }

    status = _cairo_quartz_resize_image (resized_out_image,
					 src->shrink_factor_x,
					 src->shrink_factor_y,
					 out_image);
    if (unlikely (status)) {
	CGImageRelease (resized_out_image);
	*out_image = NULL;
	return status;
    }
    CGImageRelease (resized_out_image);
    return status;
}

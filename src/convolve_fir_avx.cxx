// fastfilters
// Copyright (c) 2016 Sven Peter
// sven.peter@iwr.uni-heidelberg.de or mail@svenpeter.me
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
#include "fastfilters.hxx"
#include "util.hxx"
#include "config.h"
#include "convolve_fir.hxx"

#include <immintrin.h>
#include <stdlib.h>
#include <type_traits>
#include <stdexcept>
#include <iostream>
#include <string.h>

#if defined(__FMA__) && defined(__AVX__)

#define tpl_avx true
#define tpl_fma true
#define _wrap_mm256_fmadd_ps(a, b, c) (_mm256_fmadd_ps((a), (b), (c)))

#elif defined(__AVX__)

#define tpl_avx true
#define tpl_fma false

static inline __m256 _wrap_mm256_fmadd_ps(const __m256 a, const __m256 b, const __m256 c)
{
    __m256 product = _mm256_mul_ps(a, b);
    return _mm256_add_ps(product, c);
}

#else

#error "convovle_fir_avx.cxx needs to be compiled with AVX support"

#endif

namespace fastfilters
{

namespace fir
{

template <bool is_symmetric, unsigned half_kernel_len>
static void internal_convolve_fir_inner_single(const float *input, const unsigned int n_pixels, const unsigned n_times,
                                               const unsigned dim_stride, float *output, Kernel &kernel)
{
    const unsigned int kernel_len = 2 * half_kernel_len + 1;
    // const unsigned int half_kernel_len = kernel.half_len();
    const unsigned int avx_end = (n_pixels - half_kernel_len) & ~31;
    const unsigned int avx_end_single = (n_pixels - half_kernel_len) & ~7;

    float *tmp = (float *)detail::avx_memalign(32 * sizeof(float));

    for (unsigned int dim = 0; dim < n_times; ++dim) {
        // take next line of pixels
        float *cur_output = output + dim * dim_stride;
        const float *cur_input = input + dim * dim_stride;

        // this function is only used for small kernels (<25 pixel)
        // such that non-vectorized code can be used for the border
        // treament without speed penalties
        unsigned int i = 0;
        for (i = 0; i < ((half_kernel_len + 7) & ~7); ++i) {
            float sum = kernel.coefs[0] * cur_input[i];

            for (unsigned int k = 1; k <= half_kernel_len; ++k) {

                unsigned int offset;
                if (i < k)
                    offset = k - i;
                else
                    offset = i - k;

                float diff;
                if (is_symmetric)
                    diff = cur_input[i + k] + cur_input[offset];
                else
                    diff = cur_input[i + k] - cur_input[offset];

                sum += kernel.coefs[k] * diff;
            }

#if 0
            for (unsigned int k = 0; k < kernel_len; ++k) {
                const int kreal = k - kernel_len / 2;
                unsigned int offset;
                if (kreal + (int)i < 0)
                    offset = -i - kreal;
                else if (kreal + i >= n_pixels)
                    offset = n_pixels - ((kreal + i) % n_pixels) - 2;
                else
                    offset = i + kreal;
                sum += kernel[k] * cur_input[offset];
            }
#endif

            tmp[i] = sum;
        }

        // align to 32 pixel boundary
        const unsigned int stop = std::min((unsigned int)32, avx_end_single);
        for (; i < stop; i += 8) {
            __m256 result = _mm256_loadu_ps(cur_input + i);
            __m256 kernel_val = _mm256_broadcast_ss(&kernel.coefs[0]);

            result = _mm256_mul_ps(result, kernel_val);

            for (unsigned j = 1; j <= half_kernel_len; ++j) {
                __m256 pixels;
                kernel_val = _mm256_broadcast_ss(&kernel.coefs[j]);

                if (is_symmetric)
                    pixels = _mm256_add_ps(_mm256_loadu_ps(cur_input + i + j), _mm256_loadu_ps(cur_input + i - j));
                else
                    pixels = _mm256_sub_ps(_mm256_loadu_ps(cur_input + i + j), _mm256_loadu_ps(cur_input + i - j));

                result = _wrap_mm256_fmadd_ps(pixels, kernel_val, result);
            }

            _mm256_store_ps(tmp + i, result);
        }

        if (stop < 32) {
            for (; i < n_pixels; ++i) {
                float sum = 0.0;

                for (unsigned int k = 0; k < kernel_len; ++k) {
                    const int kreal = k - kernel_len / 2;
                    unsigned int offset;
                    if (kreal + i >= n_pixels)
                        offset = n_pixels - ((kreal + i) % n_pixels) - 2;
                    else
                        offset = i + kreal;
                    sum += kernel[k] * cur_input[offset];
                }

                tmp[i] = sum;
            }

            memcpy(cur_output, tmp, n_pixels * sizeof(float));
            continue;
        }

        // main loop - work on 32 pixels at the same time
        for (; i < avx_end; i += 32) {
            // load next 32 pixels
            __m256 result0 = _mm256_loadu_ps(cur_input + i);
            __m256 result1 = _mm256_loadu_ps(cur_input + i + 8);
            __m256 result2 = _mm256_loadu_ps(cur_input + i + 16);
            __m256 result3 = _mm256_loadu_ps(cur_input + i + 24);

            // multiply current pixels with center value of kernel
            __m256 kernel_val = _mm256_broadcast_ss(&kernel.coefs[0]);
            result0 = _mm256_mul_ps(result0, kernel_val);
            result1 = _mm256_mul_ps(result1, kernel_val);
            result2 = _mm256_mul_ps(result2, kernel_val);
            result3 = _mm256_mul_ps(result3, kernel_val);

            // work on both sides of symmetric kernel simultaneously
            for (unsigned int j = 1; j <= half_kernel_len; ++j) {
                kernel_val = _mm256_broadcast_ss(&kernel.coefs[j]);

                // sum pixels for both sides of kernel (kernel[-j] * image[i-j] + kernel[j] * image[i+j] = (image[i-j] +
                // image[i+j]) * kernel[j])
                // since kernel[-j] = kernel[j] or kernel[-j] = -kernel[j]
                __m256 pixels0, pixels1, pixels2, pixels3;

                if (is_symmetric) {
                    pixels0 = _mm256_add_ps(_mm256_loadu_ps(cur_input + i + j), _mm256_loadu_ps(cur_input + i - j));
                    pixels1 =
                        _mm256_add_ps(_mm256_loadu_ps(cur_input + i + j + 8), _mm256_loadu_ps(cur_input + i - j + 8));
                    pixels2 =
                        _mm256_add_ps(_mm256_loadu_ps(cur_input + i + j + 16), _mm256_loadu_ps(cur_input + i - j + 16));
                    pixels3 =
                        _mm256_add_ps(_mm256_loadu_ps(cur_input + i + j + 24), _mm256_loadu_ps(cur_input + i - j + 24));
                } else {
                    pixels0 = _mm256_sub_ps(_mm256_loadu_ps(cur_input + i + j), _mm256_loadu_ps(cur_input + i - j));
                    pixels1 =
                        _mm256_sub_ps(_mm256_loadu_ps(cur_input + i + j + 8), _mm256_loadu_ps(cur_input + i - j + 8));
                    pixels2 =
                        _mm256_sub_ps(_mm256_loadu_ps(cur_input + i + j + 16), _mm256_loadu_ps(cur_input + i - j + 16));
                    pixels3 =
                        _mm256_sub_ps(_mm256_loadu_ps(cur_input + i + j + 24), _mm256_loadu_ps(cur_input + i - j + 24));
                }

                // multiply with kernel value and add to result
                result0 = _wrap_mm256_fmadd_ps(pixels0, kernel_val, result0);
                result1 = _wrap_mm256_fmadd_ps(pixels1, kernel_val, result1);
                result2 = _wrap_mm256_fmadd_ps(pixels2, kernel_val, result2);
                result3 = _wrap_mm256_fmadd_ps(pixels3, kernel_val, result3);
            }

            _mm256_storeu_ps(cur_output + i - 32, _mm256_load_ps(tmp));
            _mm256_storeu_ps(cur_output + i - 24, _mm256_load_ps(tmp + 8));
            _mm256_storeu_ps(cur_output + i - 16, _mm256_load_ps(tmp + 16));
            _mm256_storeu_ps(cur_output + i - 8, _mm256_load_ps(tmp + 24));
            _mm256_store_ps(tmp, result0);
            _mm256_store_ps(tmp + 8, result1);
            _mm256_store_ps(tmp + 16, result2);
            _mm256_store_ps(tmp + 24, result3);
        }

        unsigned int k;
        for (k = 0; i < avx_end_single; i += 8, k += 8) {
            __m256 result = _mm256_loadu_ps(cur_input + i);
            __m256 kernel_val = _mm256_broadcast_ss(&kernel.coefs[0]);

            result = _mm256_mul_ps(result, kernel_val);

            for (unsigned j = 1; j <= half_kernel_len; ++j) {
                __m256 pixels;
                kernel_val = _mm256_broadcast_ss(&kernel.coefs[j]);

                if (is_symmetric)
                    pixels = _mm256_add_ps(_mm256_loadu_ps(cur_input + i + j), _mm256_loadu_ps(cur_input + i - j));
                else
                    pixels = _mm256_sub_ps(_mm256_loadu_ps(cur_input + i + j), _mm256_loadu_ps(cur_input + i - j));

                result = _wrap_mm256_fmadd_ps(pixels, kernel_val, result);
            }

            k &= 31;
            _mm256_storeu_ps(cur_output + i - 32, _mm256_load_ps(tmp + k));
            _mm256_store_ps(tmp + k, result);
        }

        for (; i < n_pixels; ++i, ++k) {
            float sum = cur_input[i] * kernel.coefs[0];

            for (unsigned int k = 0; k <= half_kernel_len; ++k) {
                float right;
                if (i + k >= n_pixels)
                    right = cur_input[n_pixels - ((k + i) % n_pixels) - 2];
                else
                    right = cur_input[i + k];

                if (is_symmetric)
                    sum += kernel.coefs[k] * (right + cur_input[i - k]);
                else
                    sum += kernel.coefs[k] * (right - cur_input[i - k]);
            }

            k &= 31;
            cur_output[i - 32] = tmp[k];
            tmp[k] = sum;
        }

        for (unsigned int j = n_pixels - 32; j < n_pixels; ++j, ++k)
            cur_output[j] = tmp[k & 31];
    }

    detail::avx_free(tmp);
}

template <bool is_symmetric, unsigned half_kernel_len>
void internal_convolve_fir_outer_single(const float *input, const unsigned int n_pixels,
                                        const unsigned int pixel_stride, const unsigned n_times, float *output,
                                        Kernel &kernel)
{
    // const unsigned int half_kernel_len = kernel.half_len();
    const unsigned int dim_avx_end = n_times & ~7;
    const unsigned int dim_left = n_times - dim_avx_end;
    const unsigned int n_dims_aligned = (n_times + 8) & ~7;

    const __m256i mask = _mm256_set_epi32(0, dim_left >= 7 ? 0xffffffff : 0, dim_left >= 6 ? 0xffffffff : 0,
                                          dim_left >= 5 ? 0xffffffff : 0, dim_left >= 4 ? 0xffffffff : 0,
                                          dim_left >= 3 ? 0xffffffff : 0, dim_left >= 2 ? 0xffffffff : 0, 0xffffffff);

    float *test = (float *)detail::avx_memalign(n_dims_aligned * sizeof(float) * (half_kernel_len + 1));

    // left border
    for (unsigned pixel = 0; pixel < half_kernel_len; ++pixel) {
        const float *inptr = input + pixel * pixel_stride;
        float *tmpptr = test + pixel * n_dims_aligned;

        unsigned dim;
        for (dim = 0; dim < dim_avx_end; dim += 8) {
            __m256 pixels = _mm256_loadu_ps(inptr + dim);
            __m256 kernel_val = _mm256_broadcast_ss(&kernel.coefs[0]);
            __m256 result = _mm256_mul_ps(pixels, kernel_val);

            for (unsigned int i = 1; i <= half_kernel_len; ++i) {
                kernel_val = _mm256_broadcast_ss(&kernel.coefs[i]);
                __m256 pixel_mirrored;

                if (i > pixel)
                    pixel_mirrored = _mm256_loadu_ps(input + (i - pixel) * pixel_stride + dim);
                else
                    pixel_mirrored = _mm256_loadu_ps(input + (pixel - i) * pixel_stride + dim);

                if (is_symmetric)
                    pixels = _mm256_add_ps(_mm256_loadu_ps(input + (pixel + i) * pixel_stride + dim), pixel_mirrored);
                else
                    pixels = _mm256_sub_ps(_mm256_loadu_ps(input + (pixel + i) * pixel_stride + dim), pixel_mirrored);
                result = _wrap_mm256_fmadd_ps(pixels, kernel_val, result);
            }

            _mm256_store_ps(tmpptr + dim, result);
        }

        if (dim_left > 0) {
            __m256 pixels = _mm256_maskload_ps(inptr + dim, mask);
            __m256 kernel_val = _mm256_broadcast_ss(&kernel.coefs[0]);
            __m256 result = _mm256_mul_ps(pixels, kernel_val);

            for (unsigned int i = 1; i <= half_kernel_len; ++i) {
                kernel_val = _mm256_broadcast_ss(&kernel.coefs[i]);
                __m256 pixel_mirrored;

                if (i > pixel)
                    pixel_mirrored = _mm256_maskload_ps(input + (i - pixel) * pixel_stride + dim, mask);
                else
                    pixel_mirrored = _mm256_maskload_ps(input + (pixel - i) * pixel_stride + dim, mask);

                if (is_symmetric)
                    pixels = _mm256_add_ps(_mm256_maskload_ps(input + (pixel + i) * pixel_stride + dim, mask),
                                           pixel_mirrored);
                else
                    pixels = _mm256_sub_ps(_mm256_maskload_ps(input + (pixel + i) * pixel_stride + dim, mask),
                                           pixel_mirrored);
                result = _wrap_mm256_fmadd_ps(pixels, kernel_val, result);
            }

            _mm256_store_ps(tmpptr + dim, result);
        }
    }

    for (unsigned pixel = half_kernel_len; pixel < n_pixels - half_kernel_len; ++pixel) {
        const float *inptr = input + pixel * pixel_stride;
        const unsigned tmpidx = pixel % (half_kernel_len + 1);
        float *tmpptr = test + tmpidx * n_dims_aligned;

        unsigned dim;
        for (dim = 0; dim < dim_avx_end; dim += 8) {
            __m256 pixels = _mm256_loadu_ps(inptr + dim);
            __m256 kernel_val = _mm256_broadcast_ss(&kernel.coefs[0]);
            __m256 result = _mm256_mul_ps(pixels, kernel_val);

            for (unsigned int i = 1; i <= half_kernel_len; ++i) {
                kernel_val = _mm256_broadcast_ss(&kernel.coefs[i]);

                if (is_symmetric)
                    pixels = _mm256_add_ps(_mm256_loadu_ps(input + (pixel + i) * pixel_stride + dim),
                                           _mm256_loadu_ps(input + (pixel - i) * pixel_stride + dim));
                else
                    pixels = _mm256_sub_ps(_mm256_loadu_ps(input + (pixel + i) * pixel_stride + dim),
                                           _mm256_loadu_ps(input + (pixel - i) * pixel_stride + dim));
                result = _wrap_mm256_fmadd_ps(pixels, kernel_val, result);
            }

            _mm256_store_ps(tmpptr + dim, result);
        }

        if (dim_left > 0) {
            __m256 pixels = _mm256_maskload_ps(inptr + dim, mask);
            __m256 kernel_val = _mm256_broadcast_ss(&kernel.coefs[0]);
            __m256 result = _mm256_mul_ps(pixels, kernel_val);

            for (unsigned int i = 1; i <= half_kernel_len; ++i) {
                kernel_val = _mm256_broadcast_ss(&kernel.coefs[i]);

                if (is_symmetric)
                    pixels = _mm256_add_ps(_mm256_maskload_ps(input + (pixel + i) * pixel_stride + dim, mask),
                                           _mm256_maskload_ps(input + (pixel - i) * pixel_stride + dim, mask));
                else
                    pixels = _mm256_sub_ps(_mm256_maskload_ps(input + (pixel + i) * pixel_stride + dim, mask),
                                           _mm256_maskload_ps(input + (pixel - i) * pixel_stride + dim, mask));
                result = _wrap_mm256_fmadd_ps(pixels, kernel_val, result);
            }

            _mm256_store_ps(tmpptr + dim, result);
        }

        const unsigned writeidx = (pixel + 1) % (half_kernel_len + 1);
        float *writeptr = test + writeidx * n_dims_aligned;
        memcpy(output + (pixel - half_kernel_len) * pixel_stride, writeptr, n_times * sizeof(float));
    }

    // right border
    for (unsigned pixel = n_pixels - half_kernel_len; pixel < n_pixels; ++pixel) {
        const float *inptr = input + pixel * pixel_stride;
        const unsigned tmpidx = pixel % (half_kernel_len + 1);
        float *tmpptr = test + tmpidx * n_dims_aligned;

        unsigned dim;
        for (dim = 0; dim < dim_avx_end; dim += 8) {
            __m256 pixels = _mm256_loadu_ps(inptr + dim);
            __m256 kernel_val = _mm256_broadcast_ss(&kernel.coefs[0]);
            __m256 result = _mm256_mul_ps(pixels, kernel_val);

            for (unsigned int i = 1; i <= half_kernel_len; ++i) {
                kernel_val = _mm256_broadcast_ss(&kernel.coefs[i]);
                __m256 pixel_mirrored;

                if (pixel + i < n_pixels)
                    pixel_mirrored = _mm256_loadu_ps(input + (pixel + i) * pixel_stride + dim);
                else
                    pixel_mirrored =
                        _mm256_loadu_ps(input + (n_pixels - ((i + pixel) % n_pixels) - 2) * pixel_stride + dim);

                if (is_symmetric)
                    pixels = _mm256_add_ps(pixel_mirrored, _mm256_loadu_ps(input + (pixel - i) * pixel_stride + dim));
                else
                    pixels = _mm256_sub_ps(pixel_mirrored, _mm256_loadu_ps(input + (pixel - i) * pixel_stride + dim));
                result = _wrap_mm256_fmadd_ps(pixels, kernel_val, result);
            }

            _mm256_store_ps(tmpptr + dim, result);
        }

        if (dim_left > 0) {
            __m256 pixels = _mm256_maskload_ps(inptr + dim, mask);
            __m256 kernel_val = _mm256_broadcast_ss(&kernel.coefs[0]);
            __m256 result = _mm256_mul_ps(pixels, kernel_val);

            for (unsigned int i = 1; i <= half_kernel_len; ++i) {
                kernel_val = _mm256_broadcast_ss(&kernel.coefs[i]);
                __m256 pixel_mirrored;

                if (pixel + i < n_pixels)
                    pixel_mirrored = _mm256_maskload_ps(input + (pixel + i) * pixel_stride + dim, mask);
                else
                    pixel_mirrored = _mm256_maskload_ps(
                        input + (n_pixels - ((i + pixel) % n_pixels) - 2) * pixel_stride + dim, mask);

                if (is_symmetric)
                    pixels = _mm256_add_ps(pixel_mirrored,
                                           _mm256_maskload_ps(input + (pixel - i) * pixel_stride + dim, mask));
                else
                    pixels = _mm256_sub_ps(pixel_mirrored,
                                           _mm256_maskload_ps(input + (pixel - i) * pixel_stride + dim, mask));
                result = _wrap_mm256_fmadd_ps(pixels, kernel_val, result);
            }

            _mm256_store_ps(tmpptr + dim, result);
        }

        const unsigned writeidx = (pixel + 1) % (half_kernel_len + 1);
        float *writeptr = test + writeidx * n_dims_aligned;
        memcpy(output + (pixel - half_kernel_len) * pixel_stride, writeptr, n_times * sizeof(float));
    }

    for (unsigned i = 0; i < half_kernel_len; ++i) {
        unsigned pixel = n_pixels + i;
        const unsigned writeidx = (pixel + 1) % (half_kernel_len + 1);
        float *writeptr = test + writeidx * n_dims_aligned;
        memcpy(output + (pixel - half_kernel_len) * pixel_stride, writeptr, n_times * sizeof(float));
    }

    detail::avx_free(test);
}

namespace
{

template <bool symmetric>
static inline void dispatch_inner_single(const float *input, const unsigned int n_pixels, const unsigned n_times,
                                         const unsigned dim_stride, float *output, Kernel &kernel)
{
    switch (kernel.half_len()) {
    case 1:
        internal_convolve_fir_inner_single<symmetric, 1>(input, n_pixels, n_times, dim_stride, output, kernel);
        break;
    case 2:
        internal_convolve_fir_inner_single<symmetric, 2>(input, n_pixels, n_times, dim_stride, output, kernel);
        break;
    case 3:
        internal_convolve_fir_inner_single<symmetric, 3>(input, n_pixels, n_times, dim_stride, output, kernel);
        break;
    case 4:
        internal_convolve_fir_inner_single<symmetric, 4>(input, n_pixels, n_times, dim_stride, output, kernel);
        break;
    case 5:
        internal_convolve_fir_inner_single<symmetric, 5>(input, n_pixels, n_times, dim_stride, output, kernel);
        break;
    case 6:
        internal_convolve_fir_inner_single<symmetric, 6>(input, n_pixels, n_times, dim_stride, output, kernel);
        break;
    case 7:
        internal_convolve_fir_inner_single<symmetric, 7>(input, n_pixels, n_times, dim_stride, output, kernel);
        break;
    case 8:
        internal_convolve_fir_inner_single<symmetric, 8>(input, n_pixels, n_times, dim_stride, output, kernel);
        break;
    case 9:
        internal_convolve_fir_inner_single<symmetric, 9>(input, n_pixels, n_times, dim_stride, output, kernel);
        break;
    case 10:
        internal_convolve_fir_inner_single<symmetric, 10>(input, n_pixels, n_times, dim_stride, output, kernel);
        break;
    case 11:
        internal_convolve_fir_inner_single<symmetric, 11>(input, n_pixels, n_times, dim_stride, output, kernel);
        break;
    case 12:
        internal_convolve_fir_inner_single<symmetric, 12>(input, n_pixels, n_times, dim_stride, output, kernel);
        break;
    default:
        throw std::logic_error("Kernel too long.");
    }
}
}

template <>
void convolve_fir_inner_single<tpl_avx, tpl_fma>(const float *input, const unsigned int n_pixels,
                                                 const unsigned n_times, const unsigned int dim_stride, float *output,
                                                 Kernel &kernel)
{
    if (kernel.is_symmetric)
        dispatch_inner_single<true>(input, n_pixels, n_times, dim_stride, output, kernel);
    else
        dispatch_inner_single<false>(input, n_pixels, n_times, dim_stride, output, kernel);
}

namespace
{
template <bool symmetric>
static inline void dispatch_outer_single(const float *input, const unsigned int n_pixels, const unsigned pixel_stride,
                                         const unsigned n_times, float *output, Kernel &kernel)
{
    switch (kernel.half_len()) {
    case 1:
        internal_convolve_fir_outer_single<symmetric, 1>(input, n_pixels, pixel_stride, n_times, output, kernel);
        break;
    case 2:
        internal_convolve_fir_outer_single<symmetric, 2>(input, n_pixels, pixel_stride, n_times, output, kernel);
        break;
    case 3:
        internal_convolve_fir_outer_single<symmetric, 3>(input, n_pixels, pixel_stride, n_times, output, kernel);
        break;
    case 4:
        internal_convolve_fir_outer_single<symmetric, 4>(input, n_pixels, pixel_stride, n_times, output, kernel);
        break;
    case 5:
        internal_convolve_fir_outer_single<symmetric, 5>(input, n_pixels, pixel_stride, n_times, output, kernel);
        break;
    case 6:
        internal_convolve_fir_outer_single<symmetric, 6>(input, n_pixels, pixel_stride, n_times, output, kernel);
        break;
    case 7:
        internal_convolve_fir_outer_single<symmetric, 7>(input, n_pixels, pixel_stride, n_times, output, kernel);
        break;
    case 8:
        internal_convolve_fir_outer_single<symmetric, 8>(input, n_pixels, pixel_stride, n_times, output, kernel);
        break;
    case 9:
        internal_convolve_fir_outer_single<symmetric, 9>(input, n_pixels, pixel_stride, n_times, output, kernel);
        break;
    case 10:
        internal_convolve_fir_outer_single<symmetric, 10>(input, n_pixels, pixel_stride, n_times, output, kernel);
        break;
    case 11:
        internal_convolve_fir_outer_single<symmetric, 11>(input, n_pixels, pixel_stride, n_times, output, kernel);
        break;
    case 12:
        internal_convolve_fir_outer_single<symmetric, 12>(input, n_pixels, pixel_stride, n_times, output, kernel);
        break;
    default:
        throw std::logic_error("Kernel too long.");
    }
}
}

template <>
void convolve_fir_outer_single<tpl_avx, tpl_fma>(const float *input, const unsigned int n_pixels,
                                                 const unsigned int pixel_stride, const unsigned n_times,
                                                 const unsigned dim_stride, float *output, Kernel &kernel)
{
    (void)dim_stride;
    if (kernel.is_symmetric)
        dispatch_outer_single<true>(input, n_pixels, pixel_stride, n_times, output, kernel);
    else
        dispatch_outer_single<false>(input, n_pixels, pixel_stride, n_times, output, kernel);
}

} // namespace detail

} // namespace fastfilters

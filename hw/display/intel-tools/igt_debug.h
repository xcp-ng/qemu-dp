/*
 * Copyright © 2007,2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *
 */


#ifndef IGT_DEBUG_H
#define IGT_DEBUG_H

#include "qemu/osdep.h"
#include "qemu/log.h"

/**
 * IGT_EXIT_FAILURE
 *
 * Exit status indicating a test failure
 */
#define IGT_EXIT_FAILURE 99

#define igt_fail(exitcode) exit(exitcode)
#define igt_exit()         exit(0)

/**
 * igt_assert:
 * @expr: condition to test
 *
 * Fails (sub-)test if the condition is not met.
 *
 * Should be used everywhere where a test checks results.
 */
#define igt_assert(expr) \
	do { if (!(expr)) {\
                igt_info("igt_assert failed check %s in %s:%d %s\n", #expr, __FILE__, __LINE__, __func__); \
                igt_fail(-1); \
             } \
	} while (0)

/**
 * igt_info:
 * @...: format string and optional arguments
 *
 */
#define igt_info(s,f...) qemu_log("vGT: %s:"s,__FILE__, f)

#endif /* IGT_DEBUG_H */

/*
 * Copyright © 2008 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "drm/i915_drm.h"

#include "intel_chipset.h"
#include "igt_debug.h"
/**
 * SECTION:intel_chipset
 * @short_description: Feature macros and chipset helpers
 * @title: intel chipset
 * @include: intel_chipset.h
 *
 * This library mostly provides feature macros which use raw pci device ids. It
 * also provides a few more helper functions to handle pci devices, chipset
 * detection and related issues.
 */

/**
 * intel_get_drm_devid:
 * @fd: open i915 drm file descriptor
 *
 * Queries the kernel for the pci device id corresponding to the drm file
 * descriptor.
 *
 * Returns:
 * The devid, exits the program on any failures.
 */
uint32_t
intel_get_drm_devid(int fd)
{
	const char *override;

	struct drm_i915_getparam gp;
	int devid = 0;

	memset(&gp, 0, sizeof(gp));
	gp.param = I915_PARAM_CHIPSET_ID;
	gp.value = &devid;

	if (ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp, sizeof(gp)))
		return 0;

	override = getenv("INTEL_DEVID_OVERRIDE");
	if (override)
		return strtol(override, NULL, 0);
	else
		return devid;
}
/**
 * intel_gen:
 * @devid: pci device id
 *
 * Computes the Intel GFX generation for the give device id.
 *
 * Returns:
 * The GFX generation on successful lookup, -1 on failure.
 */
int intel_gen(uint32_t devid)
{
	if (IS_GEN2(devid))
		return 2;
	if (IS_GEN3(devid))
		return 3;
	if (IS_GEN4(devid))
		return 4;
	if (IS_GEN5(devid))
		return 5;
	if (IS_GEN6(devid))
		return 6;
	if (IS_GEN7(devid))
		return 7;
	if (IS_GEN8(devid))
		return 8;
	if (IS_GEN9(devid))
		return 9;

	return -1;
}

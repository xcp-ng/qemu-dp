/***********************************************************************
 * Copyright 2016 Advanced Micro Devices, Inc. All rights reserved.
 *
 * AMD is granting you permission to use this software and documentation
 * (if any) (collectively, the "Materials") pursuant to the terms and
 * conditions of the Software License Agreement included with the
 * Materials.  If you do not have a copy of the Software License
 * Agreement, contact your AMD representative for a copy.
 *
 * You agree that you will not reverse engineer or decompile the
 * Materials, in whole or in part, except as allowed by applicable law.
 *
 * WARRANTY DISCLAIMER: THE MATERIALS ARE PROVIDED "AS IS" WITHOUT
 * WARRANTY OF ANY KIND.  AMD DISCLAIMS ALL WARRANTIES, EXPRESS, IMPLIED,
 * OR STATUTORY, INCLUDING BUT NOT LIMITED TO THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE,
 * NON-INFRINGEMENT, THAT THE MATERIALS WILL RUN UNINTERRUPTED OR
 * ERROR-FREE OR WARRANTIES ARISING FROM CUSTOM OF TRADE OR COURSE OF
 * USAGE.  THE ENTIRE RISK ASSOCIATED WITH THE USE OF THE MATERIAL IS
 * ASSUMED BY YOU.  Some jurisdictions do not allow the exclusion of
 * implied warranties, so the above exclusion may not apply to You.
 *
 * LIMITATION OF LIABILITY AND INDEMNIFICATION:  AMD AND ITS LICENSORS
 * WILL NOT, UNDER ANY CIRCUMSTANCES BE LIABLE TO YOU FOR ANY PUNITIVE,
 * DIRECT, INCIDENTAL, INDIRECT, SPECIAL OR CONSEQUENTIAL DAMAGES ARISING
 * FROM USE OF THE MATERIALS OR THIS AGREEMENT EVEN IF AMD AND ITS
 * LICENSORS HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.  In no
 * event shall AMD's total liability to You for all damages, losses, and
 * causes of action (whether in contract, tort (including negligence) or
 * otherwise) exceed the amount of $100 USD.  You agree to defend,
 * indemnify and hold harmless AMD and its licensors, and any of their
 * directors, officers, employees, affiliates or agents from and against
 * any and all loss, damage, liability and other expenses (including
 * reasonable attorneys' fees), resulting from Your use of the Materials
 * or violation of the terms and conditions of this Agreement.
 *
 * U.S. GOVERNMENT RESTRICTED RIGHTS: The Materials are provided with
 * "RESTRICTED RIGHTS." Use, duplication, or disclosure by the Government
 * is subject to the restrictions as set forth in FAR 52.227-14 and
 * DFAR252.227-7013, et seq., or its successor.  Use of the Materials by
 * the Government constitutes acknowledgement of AMD's proprietary rights
 * in them.

 * EXPORT RESTRICTIONS: The Materials may be subject to export
 * restrictions as stated in the Software License Agreement.
 */

/*****************************************************************************\
*  Module Name    gim_ioctl.h
*  Project        GPU IOV MODULE
\*****************************************************************************/
#ifndef _GPU_IOV_MODULE__gim_ioctl_H
#define _GPU_IOV_MODULE__gim_ioctl_H

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/fs.h>


#define GIM_IOCTL_ALLOC_VF		_IOW('g', 1, struct gim_ioctl_alloc_vf *)
#define GIM_IOCTL_FREE_VF		_IO('g', 2)
#define GIM_IOCTL_MMIO_IS_BLOCKED	_IO('g', 3)
#define GIM_IOCTL_MMIO_IS_PASS_THROUGH	_IO('g', 4)
#define GIM_IOCTL_RECONFIG_PF		_IOW('g', 5, struct gim_ioctl_reconfig_pf *)

/*
 * QEMU will send commands to GIM via the IOCTL mechanism.
 * The supported command are as listed above; Alloc/Free a VF and notification that the
 * blockable MMIO range is either Blocked or Unblocked.
 */
struct gim_ioctl_alloc_vf
{
	uint	domid;
	uint	fb_size;		// optional frame buffer size in MB
	uint	bdf;			// bdf of the VF that is attached to this instance of QEMU.
};

struct gim_ioctl_reconfig_pf
{
	uint	bdf;			// bdf of the PF to reconfig
	uint	num_vfs;
	uint	vf_fb_size;
	uint	reserved1;
	uint	reserved2;
	uint	reserved3;
	uint	reserved4;
	uint	reserved5;
};

#endif

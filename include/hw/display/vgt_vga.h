/*
 * QEMU VGA Emulator.
 *
 * Copyright (c) Citrix Systems, Inc
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef QEMU_HW_DISPLAY_VGT_VGA_H
#define QEMU_HW_DISPLAY_VGT_VGA_H

extern bool vgt_vga_enabled;
extern int vgt_low_gm_sz;
extern int vgt_high_gm_sz;
extern int vgt_fence_sz;
extern int vgt_cap;
extern PCIDevice *vgt_bridge;

void vgt_vga_init(PCIBus *pci_bus);
void vgt_bridge_pci_conf_init(PCIDevice *dev);
void vgt_bridge_pci_write(PCIDevice *dev,
                          uint32_t address, uint32_t val, int len);

void xengt_drm_init(void);
bool xengt_is_enabled(void);
void xengt_draw_primary(QemuConsole *con, int full_update);

#endif

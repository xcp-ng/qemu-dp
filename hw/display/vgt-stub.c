/*
 * QEMU VGT support
 *
 * Copyright (c) Intel
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */
#include "qemu/osdep.h"
#include "hw/display/vgt_vga.h"

bool vgt_vga_enabled;
PCIDevice *vgt_bridge = NULL;

void vgt_vga_init(PCIBus *pci_bus)
{
}

void vgt_bridge_pci_conf_init(PCIDevice *pdev)
{
}

void vgt_bridge_pci_write(PCIDevice *dev,
                          uint32_t address, uint32_t val, int len)
{
}

bool xengt_is_enabled(void)
{
    return false;
}

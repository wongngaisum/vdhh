/*
 * ACPI implementation
 *
 * Copyright (c) 2006 Fabrice Bellard
 * Copyright (c) 2009 Isaku Yamahata <yamahata at valinux co jp>
 *                    VA Linux Systems Japan K.K.
 * Copyright (C) 2012 Jason Baron <jbaron@redhat.com>
 *
 * This is based on acpi.c.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>
 */
#include "hw.h"
#include "qapi/visitor.h"
#include "ipc.h"
#include "pci.h"
#include "qemu/timer.h"
#include "sysemu.h"
#include "nacpi.h"
#include "veertuemu.h"
#include "address-spaces.h"

#include "iich9.h"

//#define DEBUG

#ifdef DEBUG
#define ICH9_DEBUG(fmt, ...) \
do { printf("%s "fmt, __func__, ## __VA_ARGS__); } while (0)
#else
#define ICH9_DEBUG(fmt, ...)    do { } while (0)
#endif

static void ich9_pm_update_sci_fn(ACPIREGS *regs)
{
    ICH9LPCPMRegs *pm = container_of(regs, ICH9LPCPMRegs, acpi_regs);
    acpi_update_sci(&pm->acpi_regs, pm->irq);
}

static uint64_t ich9_gpe_readb(void *opaque, hwaddr addr, unsigned width)
{
    ICH9LPCPMRegs *pm = opaque;
    return acpi_gpe_ioport_readb(&pm->acpi_regs, addr);
}

static void ich9_gpe_writeb(void *opaque, hwaddr addr, uint64_t val,
                            unsigned width)
{
    ICH9LPCPMRegs *pm = opaque;
    acpi_gpe_ioport_writeb(&pm->acpi_regs, addr, val);
    acpi_update_sci(&pm->acpi_regs, pm->irq);
}

static const MemAreaOps ich9_gpe_ops = {
    .read = ich9_gpe_readb,
    .write = ich9_gpe_writeb,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t ich9_smi_readl(void *opaque, hwaddr addr, unsigned width)
{
    ICH9LPCPMRegs *pm = opaque;
    switch (addr) {
    case 0:
        return pm->smi_en;
    case 4:
        return pm->smi_sts;
    default:
        return 0;
    }
}

static void ich9_smi_writel(void *opaque, hwaddr addr, uint64_t val,
                            unsigned width)
{
    ICH9LPCPMRegs *pm = opaque;
    switch (addr) {
    case 0:
        pm->smi_en = val;
        break;
    }
}

static const MemAreaOps ich9_smi_ops = {
    .read = ich9_smi_readl,
    .write = ich9_smi_writel,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void ich9_pm_iospace_update(ICH9LPCPMRegs *pm, uint32_t pm_io_base)
{
    ICH9_DEBUG("to 0x%x\n", pm_io_base);

    assert((pm_io_base & ICH9_PMIO_MASK) == 0);

    pm->pm_io_base = pm_io_base;
    mem_area_set_enable(&pm->io, pm->pm_io_base != 0, true);
    mem_area_set_addr(&pm->io, pm->pm_io_base);
    veertu_mem_referesh();
}

static int ich9_pm_post_load(void *opaque, int version_id)
{
    ICH9LPCPMRegs *pm = opaque;
    uint32_t pm_io_base = pm->pm_io_base;
    pm->pm_io_base = 0;
    ich9_pm_iospace_update(pm, pm_io_base);
    return 0;
}

#define VMSTATE_GPE_ARRAY(_field, _state)                            \
 {                                                                   \
     .name       = (stringify(_field)),                              \
     .version_id = 0,                                                \
     .num        = ICH9_PMIO_GPE0_LEN,                               \
     .info       = &vmstate_info_uint8,                              \
     .size       = sizeof(uint8_t),                                  \
     .flags      = VMS_ARRAY | VMS_POINTER,                          \
     .offset     = vmstate_offset_pointer(_state, _field, uint8_t),  \
 }

const VMStateDescription vmstate_ich9_pm = {
    .name = "ich9_pm",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ich9_pm_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT16(acpi_regs.pm1.evt.sts, ICH9LPCPMRegs),
        VMSTATE_UINT16(acpi_regs.pm1.evt.en, ICH9LPCPMRegs),
        VMSTATE_UINT16(acpi_regs.pm1.cnt.cnt, ICH9LPCPMRegs),
        VMSTATE_TIMER(acpi_regs.tmr.timer, ICH9LPCPMRegs),
        VMSTATE_INT64(acpi_regs.tmr.overflow_time, ICH9LPCPMRegs),
        VMSTATE_GPE_ARRAY(acpi_regs.gpe.sts, ICH9LPCPMRegs),
        VMSTATE_GPE_ARRAY(acpi_regs.gpe.en, ICH9LPCPMRegs),
        VMSTATE_UINT32(smi_en, ICH9LPCPMRegs),
        VMSTATE_UINT32(smi_sts, ICH9LPCPMRegs),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (VMStateSubsection[]) {
        {
        },
        VMSTATE_END_OF_LIST()
    }
};

static void pm_reset(void *opaque)
{
    ICH9LPCPMRegs *pm = opaque;
    ich9_pm_iospace_update(pm, 0);

    acpi_pm1_evt_reset(&pm->acpi_regs);
    acpi_pm1_cnt_reset(&pm->acpi_regs);
    acpi_pm_tmr_reset(&pm->acpi_regs);
    acpi_gpe_reset(&pm->acpi_regs);

    if (vmx_enabled()) {
        /* Mark SMM as already inited to prevent SMM from running. KVM does not
         * support SMM mode. */
        pm->smi_en |= ICH9_PMIO_SMI_EN_APMC_EN;
    }

    acpi_update_sci(&pm->acpi_regs, pm->irq);
}

static void pm_powerdown_req(Notifier *n, void *opaque)
{
    ICH9LPCPMRegs *pm = container_of(n, ICH9LPCPMRegs, powerdown_notifier);

    acpi_pm1_evt_power_down(&pm->acpi_regs);
}

void ich9_pm_init(PCIDevice *lpc_pci, ICH9LPCPMRegs *pm,
                  vmx_irq sci_irq)
{
    memory_area_init(&pm->io, "ich9-pm", ICH9_PMIO_SIZE);
    mem_area_set_enable(&pm->io, false, true);
    mem_area_add_child(pci_address_space_io(lpc_pci),
                                0, &pm->io);

    acpi_pm_tmr_init(&pm->acpi_regs, ich9_pm_update_sci_fn, &pm->io);
    acpi_pm1_evt_init(&pm->acpi_regs, ich9_pm_update_sci_fn, &pm->io);
    acpi_pm1_cnt_init(&pm->acpi_regs, &pm->io, 2);

    acpi_gpe_init(&pm->acpi_regs, ICH9_PMIO_GPE0_LEN);
    memory_area_init_io(&pm->io_gpe, VeertuTypeHold(lpc_pci), &ich9_gpe_ops, pm,
                          "acpi-gpe0", ICH9_PMIO_GPE0_LEN);
    mem_area_add_child(&pm->io, ICH9_PMIO_GPE0_STS, &pm->io_gpe);

    memory_area_init_io(&pm->io_smi, VeertuTypeHold(lpc_pci), &ich9_smi_ops, pm,
                          "acpi-smi", 8);
    mem_area_add_child(&pm->io, ICH9_PMIO_SMI_EN, &pm->io_smi);

    pm->irq = sci_irq;
    vmx_register_reset(pm_reset, pm);
    pm->powerdown_notifier.notify = pm_powerdown_req;
    vmx_register_powerdown_notifier(&pm->powerdown_notifier);
}

static void ich9_pm_get_gpe0_blk(VeertuType *obj, Visitor *v,
                                 void *opaque, const char *name,
                                 Error **errp)
{
    ICH9LPCPMRegs *pm = opaque;
    uint32_t value = pm->pm_io_base + ICH9_PMIO_GPE0_STS;

    visit_type_uint32(v, &value, name, errp);
}

void ich9_pm_add_properties(VeertuType *obj, ICH9LPCPMRegs *pm, Error **errp)
{
}

void ich9_pm_ospm_status(AcpiDeviceIf *adev, ACPIOSTInfoList ***list)
{
    ICH9LPCState *s = ICH9_LPC_DEVICE(adev);

}

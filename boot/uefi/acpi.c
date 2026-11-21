/* AxiomOS UEFI loader - ACPI table discovery.
 *
 * The loader does not parse tables; it only locates the RSDP and forwards the
 * physical address to the kernel.  Both the EFI configuration table (ACPI 2.0
 * GUID) and the legacy EBDA/BIOS search are implemented because some virtual
 * machines expose only the latter.
 */

#include "loader.h"

static int guid_equal(const efi_guid *a, const efi_guid *b)
{
    return a->data1 == b->data1 && a->data2 == b->data2 && a->data3 == b->data3 &&
           u_memcmp(a->data4, b->data4, 8) == 0;
}

static int rsdp_valid(u64 addr)
{
    char *sig = (char *)addr;
    if (u_memcmp(sig, "RSD PTR ", 8) != 0)
        return 0;
    /* Simple checksum over the first 20 bytes (ACPI 1.0 structure). */
    {
        u8 sum = 0;
        for (int i = 0; i < 20; i++)
            sum = (u8)(sum + ((u8 *)addr)[i]);
        if (sum != 0)
            return 0;
    }
    return 1;
}

static u64 scan_ebda(void)
{
    /* The EBDA segment pointer lives at 0x40:0x0E and is a real-mode segment
     * address; the first 1 KiB of the EBDA is searched for the RSDP signature. */
    u16 ebda_seg = *(volatile u16 *)0x40E;
    u64 ebda = (u64)ebda_seg << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000) {
        for (u64 p = ebda; p < ebda + 1024; p += 16) {
            if (rsdp_valid(p))
                return p;
        }
    }
    /* BIOS read-only area 0xE0000..0xFFFFF, 16-byte aligned. */
    for (u64 p = 0xE0000; p < 0x100000; p += 16) {
        if (rsdp_valid(p))
            return p;
    }
    return 0;
}

u64 acpi_find_rsdp(efi_system_table *st)
{
    efi_guid acpi20 = EFI_ACPI_TABLE_GUID;
    efi_guid acpi10 = EFI_GUID(0xeb9d2d30, 0x2d88, 0x11d3, 0x9a, 0x16, 0x00,
                               0x90, 0x27, 0x3f, 0xc1, 0x4d);
    u64 found = 0;

    if (st && st->configuration_table) {
        for (efi_uintn i = 0; i < st->number_of_table_entries; i++) {
            efi_configuration_table *ct = &st->configuration_table[i];
            if (guid_equal(&ct->vendor_guid, &acpi20) ||
                guid_equal(&ct->vendor_guid, &acpi10)) {
                found = (u64)ct->vendor_table;
                if (found && rsdp_valid(found))
                    break;
                found = 0;
            }
        }
    }
    if (!found) {
        con_puts("[acpi] no configuration table entry, scanning legacy areas\n");
        found = scan_ebda();
    }
    if (found) {
        char *rsdp = (char *)found;
        con_printf("[acpi] RSDP at %p revision %u\n", (void *)found,
                   (u64)rsdp[15]);
        /* OemId and OEM table id are handy in the boot log. */
        con_printf("[acpi] oem '%.6s' table '%.8s'\n", rsdp + 9, rsdp + 16);
    } else {
        con_puts("[acpi] RSDP not found; kernel will run without ACPI\n");
    }
    return found;
}

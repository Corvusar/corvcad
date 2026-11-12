/* SPDX-License-Identifier: MIT */
/* CorvCAD: the memory ngspice may use on macOS and iOS, linked into libngspice.a by
 * ngspice_ios.sh (ios_prefix.h renames ngspice's host_statistics call to this function).
 *
 * Before keeping a result in memory, ngspice compares its size with getAvailableMemorySize()
 * (frontend/get_avail_mem_size.c), which on Apple platforms is the free page count alone. macOS
 * and iOS keep most memory in inactive and purgeable pages, so that figure swings with the
 * system's caches and refuses runs at random. On iOS an app's real limit is
 * os_proc_available_memory(), what it can still allocate before iOS ends it; it is 0 where no
 * such limit applies (the simulator). Elsewhere free, inactive and purgeable pages count.
 * MIT licence (CorvCAD). */
#include <TargetConditionals.h>
#include <mach/mach.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

kern_return_t corvcad_host_statistics(host_t host, host_flavor_t flavor, host_info_t info,
                                      mach_msg_type_number_t *count);

kern_return_t corvcad_host_statistics(host_t host, host_flavor_t flavor, host_info_t info,
                                      mach_msg_type_number_t *count) {
    kern_return_t status = host_statistics(host, flavor, info, count);
    if (status != KERN_SUCCESS || flavor != HOST_VM_INFO)
        return status;
    vm_statistics_data_t *vm = (vm_statistics_data_t *)info;
    size_t limit = 0;
#if TARGET_OS_IPHONE
    limit = os_proc_available_memory();
#endif
    vm_size_t page = 0;
    host_page_size(host, &page);
    if (limit > 0 && page > 0)
        vm->free_count = (natural_t)(limit / page);
    else
        vm->free_count += vm->inactive_count + vm->purgeable_count;
    return status;
}

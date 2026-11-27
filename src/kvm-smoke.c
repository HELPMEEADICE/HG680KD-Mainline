#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/kvm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_RAM UINT64_C(0x40000000)
#define GUEST_MMIO UINT64_C(0x10000000)
#define GUEST_RAM_SIZE (64 * 1024)

static int checked_ioctl(int fd, unsigned long request, unsigned long arg,
                         const char *name)
{
    int ret = ioctl(fd, request, arg);

    if (ret < 0)
        err(1, "%s", name);

    return ret;
}

#define kvm_ioctl(fd, request, arg) \
    checked_ioctl(fd, request, (unsigned long)(arg), #request)

static void set_vgic_attr(int fd, __u32 group, __u64 attr, const void *value)
{
    struct kvm_device_attr request = {
        .group = group,
        .attr = attr,
        .addr = (uintptr_t)value,
    };

    kvm_ioctl(fd, KVM_SET_DEVICE_ATTR, &request);
}

static void init_vgic(int vm)
{
    struct kvm_create_device device = { .type = KVM_DEV_TYPE_ARM_VGIC_V3 };
    const __u64 dist = 0x08000000;
    const __u64 redist = 0x080a0000;

    kvm_ioctl(vm, KVM_CREATE_DEVICE, &device);
    set_vgic_attr(device.fd, KVM_DEV_ARM_VGIC_GRP_ADDR,
                  KVM_VGIC_V3_ADDR_TYPE_DIST, &dist);
    set_vgic_attr(device.fd, KVM_DEV_ARM_VGIC_GRP_ADDR,
                  KVM_VGIC_V3_ADDR_TYPE_REDIST, &redist);
    set_vgic_attr(device.fd, KVM_DEV_ARM_VGIC_GRP_CTRL,
                  KVM_DEV_ARM_VGIC_CTRL_INIT, NULL);
    close(device.fd);
    puts("VGICv3 create/init OK");
}

int main(void)
{
    /* AArch64 guest at reset EL1h. Write CurrentEL to MMIO, then PSCI SYSTEM_OFF. */
    static const uint32_t program[] = {
        0xd5384240,  /* mrs x0, CurrentEL */
        0xd2a20001,  /* mov x1, #0x10000000 */
        0xf9000020,  /* str x0, [x1] */
        0xd2800100,  /* mov x0, #8 */
        0xf2b08000,  /* movk x0, #0x8400, lsl #16 */
        0xd4000002,  /* hvc #0 */
        0x14000000,  /* b . (timeout if SYSTEM_OFF failed) */
    };
    bool saw_mmio = false;
    uint64_t pc = GUEST_RAM;
    void *ram = mmap(NULL, GUEST_RAM_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct kvm_userspace_memory_region memory = {
        .slot = 0,
        .guest_phys_addr = GUEST_RAM,
        .memory_size = GUEST_RAM_SIZE,
        .userspace_addr = (uintptr_t)ram,
    };
    struct kvm_one_reg pc_reg = {
        .id = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
              KVM_REG_ARM_CORE_REG(regs.pc),
        .addr = (uintptr_t)&pc,
    };
    struct kvm_vcpu_init init = {};
    struct kvm_run *run;
    int kvm, vm, vcpu, mmap_size, version;

    if (ram == MAP_FAILED)
        err(1, "guest RAM mmap");
    memcpy(ram, program, sizeof(program));
    alarm(10);
    kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0)
        err(1, "open /dev/kvm");
    version = kvm_ioctl(kvm, KVM_GET_API_VERSION, 0);
    if (version != KVM_API_VERSION)
        errx(1, "KVM API version %d, expected %d", version, KVM_API_VERSION);
    printf("KVM API version %d\n", version);

    vm = kvm_ioctl(kvm, KVM_CREATE_VM, 0);
    kvm_ioctl(vm, KVM_SET_USER_MEMORY_REGION, &memory);
    vcpu = kvm_ioctl(vm, KVM_CREATE_VCPU, 0);
    kvm_ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init);
    init.features[0] |= 1U << KVM_ARM_VCPU_PSCI_0_2;
    kvm_ioctl(vcpu, KVM_ARM_VCPU_INIT, &init);
    kvm_ioctl(vcpu, KVM_SET_ONE_REG, &pc_reg);
    if (kvm_ioctl(kvm, KVM_CHECK_EXTENSION, KVM_CAP_IRQCHIP))
        init_vgic(vm);
    else
        puts("In-kernel VGIC unavailable");
    mmap_size = kvm_ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
    if (run == MAP_FAILED)
        err(1, "KVM run mmap");

    for (;;) {
        uint64_t value;

        kvm_ioctl(vcpu, KVM_RUN, 0);
        switch (run->exit_reason) {
        case KVM_EXIT_MMIO:
            if (saw_mmio || !run->mmio.is_write ||
                run->mmio.phys_addr != GUEST_MMIO ||
                run->mmio.len != sizeof(value))
                errx(1, "unexpected guest MMIO");
            memcpy(&value, run->mmio.data, sizeof(value));
            if (value != 4)
                errx(1, "guest CurrentEL=0x%" PRIx64 ", expected EL1", value);
            printf("Guest MMIO OK: CurrentEL=0x%" PRIx64 " (EL1)\n", value);
            saw_mmio = true;
            break;
        case KVM_EXIT_SYSTEM_EVENT:
            if (!saw_mmio || run->system_event.type != KVM_SYSTEM_EVENT_SHUTDOWN)
                errx(1, "unexpected guest system event %u", run->system_event.type);
            puts("Guest PSCI SYSTEM_OFF OK");
            puts("KVM-GUEST: PASS");
            return 0;
        default:
            errx(1, "KVM_RUN exit %u", run->exit_reason);
        }
    }
}

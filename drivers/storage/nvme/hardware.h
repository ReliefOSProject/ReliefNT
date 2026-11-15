/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_NVME_HARDWARE_H
#define RELIEFNT_NVME_HARDWARE_H
#include <stdint.h>

struct __attribute__((packed)) nvme_command {
    uint32_t cdw0,nsid;
    uint64_t reserved0,metadata,prp1,prp2;
    uint32_t cdw10,cdw11,cdw12,cdw13,cdw14,cdw15;
};
struct __attribute__((packed)) nvme_completion {
    uint32_t result,reserved0;
    uint16_t sq_head,sq_id,command_id,status;
};
_Static_assert(sizeof(struct nvme_command)==64,"NVMe submission entry");
_Static_assert(sizeof(struct nvme_completion)==16,"NVMe completion entry");
#endif

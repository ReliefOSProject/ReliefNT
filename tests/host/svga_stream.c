/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdint.h>
#include "../../drivers/video/drm/svga/stream.h"

int main(void)
{
    uint32_t words[] = { 1071, 4, 7, 1072, 0 };
    struct svga_packet packet;
    assert(svga_packet_read(words, 4, &packet) == -EINVAL);
    assert(svga_packet_read(words, 8, &packet) == -EINVAL);
    assert(!svga_packet_read(words, sizeof(words), &packet));
    assert(packet.command == 1071 && packet.bytes == 4 && packet.next == (void *)(words + 3));
    assert(!svga_packet_read(packet.next, 8, &packet) && packet.bytes == 0);
    words[1] = 3;
    assert(svga_packet_read(words, sizeof(words), &packet) == -EINVAL);
    words[1] = UINT32_MAX - 3;
    assert(svga_packet_read(words, sizeof(words), &packet) == -E2BIG);

    static uint32_t max_packet[2 + SVGA_CMD_MAX_DATASIZE / 4 + 1];
    max_packet[1] = SVGA_CMD_MAX_DATASIZE;
    assert(!svga_packet_read(max_packet, sizeof(max_packet), &packet));
    max_packet[1] += sizeof(uint32_t);
    assert(svga_packet_read(max_packet, sizeof(max_packet), &packet) == -E2BIG);

    assert(svga_array_count(24, 8, 4, 4) == 4);
    assert(svga_array_count(8, 8, 4, 0) == 0);
    assert(svga_array_count(7, 8, 4, 4) < 0);
    assert(svga_array_count(24, 8, 4, 3) < 0);
    assert(svga_array_count(20, 8, 8, 4) < 0);
    assert(svga_buffer_fits(4096, 4080, 16));
    assert(!svga_buffer_fits(4096, 4080, 17));
    assert(!svga_buffer_fits(4096, UINT64_MAX - 16, 32));
    return 0;
}

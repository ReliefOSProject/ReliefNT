#include "internal.h"



/**
 * @brief Append length bytes into the ring, overwriting the oldest when full; returns bytes written.
 */
uint32_t ring_push(uint8_t *ring, uint32_t cap, uint32_t *head, uint32_t *tail,
                          const char *buffer, uint32_t length)
{
    uint32_t written = 0;
    for (uint32_t i = 0; i < length; ++i) {
        uint32_t next = (*head + 1) % cap;
        if (next == *tail) {
            *tail = (*tail + 1) % cap;
        }
        ring[*head] = (uint8_t)buffer[i];
        *head = next;
        ++written;
    }
    return written;
}

/**
 * @brief Remove up to length bytes from the ring into buffer; returns bytes read.
 */
uint32_t ring_pop(uint8_t *ring, uint32_t cap, uint32_t *head, uint32_t *tail,
                         char *buffer, uint32_t length)
{
    uint32_t read = 0;
    while (*tail != *head && read < length) {
        buffer[read++] = (char)ring[*tail];
        *tail = (*tail + 1) % cap;
    }
    return read;
}

/*
 * ReliefOS driver-manager interface: declares driver registration and dispatch.
 * Defines the kernel-facing contract for device discovery and lifecycle.
 */
#ifndef RELIEFNT_DRIVER_MANAGER_H
#define RELIEFNT_DRIVER_MANAGER_H

#include <reliefos/driver.h>
#include <reliefos/audio_abi.h>
#include <reliefnt/mouse.h>
#include <reliefnt/types.h>

/**
 * @brief Initialize the driver manager and its driver/device registries.
 */
void driver_manager_init(void);
/**
 * @brief Initialize the drivers linked into the kernel image.
 * @return None. Boot has no outer execution transaction; the manager acquires
 * its own transaction and releases it only around waitable module phases.
 */
void driver_manager_load_builtin(void);
/**
 * @brief Fill query with the registered drivers and set query->count.
 * @param query Kernel-owned output and capacity descriptor.
 * @return 0 on success, -EINVAL for null input, or -EBUSY during a lifecycle operation.
 */
int driver_manager_list(struct reliefos_driver_list *query);
/**
 * @brief Reject runtime driver control actions; drivers are built into the
 * kernel image, so nothing can be loaded, unloaded or disabled at runtime.
 * @param request Kernel-owned, validated control request. The syscall caller holds
 * the outer execution transaction and driverctl uses nonblocking manager admission.
 * @return Always -EOPNOTSUPP (-95), also stored into request->status.
 */
int driver_manager_control(struct reliefos_driver_control *request);

/**
 * @brief Copy the current mouse position and button state into out.
 */
void driver_manager_mouse_state(struct mouse_state *out);
/**
 * @brief Return how many mouse events are waiting in the queue.
 */
uint32_t driver_manager_mouse_event_count(void);
/**
 * @brief Return the last PS/2 mouse status byte received.
 */
uint8_t driver_manager_mouse_last_status(void);
/**
 * @brief Return the last PS/2 mouse data byte received.
 */
uint8_t driver_manager_mouse_last_data(void);
/**
 * @brief Return the last PS/2 mouse acknowledge byte received.
 */
uint8_t driver_manager_mouse_last_ack(void);
/**
 * @brief Service pending mouse events and refresh the shared mouse state.
 */
void driver_manager_mouse_poll(void);
/**
 * @brief Configure the audio device with the given sample format; returns status.
 */
int driver_manager_audio_configure(const struct reliefos_audio_format *format);
/**
 * @brief Write length bytes of audio samples, reporting the device status in out_status.
 */
long driver_manager_audio_write(const void *data, uint32_t length,
                                uint32_t *out_status);
/**
 * @brief Copy the current audio device state into out.
 */
void driver_manager_audio_get_state(struct reliefos_audio_state *out);

/** @brief Acquire the sole OSS playback lease on the current v1 backend.
 * @param generation Receives a non-reused backend generation on success.
 * @param state Receives the pinned hardware's current format and queue state.
 * @return Zero, -ENODEV or -EBUSY. Task context; no capture lease is implied.
 */
int driver_manager_audio_acquire(uint32_t *generation, struct reliefos_audio_state *state);
/** @brief Release an OSS lease without affecting a newer backend generation.
 * @param generation Generation acquired by this open-file description.
 * @return None. Task context; a stale release is harmless.
 */
void driver_manager_audio_release(uint32_t generation);
/** @brief Configure the exact leased v1 backend, rejecting stale descriptions.
 * @param generation Acquired generation; zero preserves the native v1 API.
 * @param format Borrowed hardware format.
 * @return Driver result or -ENODEV. Task callback pinned through return.
 */
int driver_manager_audio_configure_bound(uint32_t generation, const struct reliefos_audio_format *format);
/** @brief Write to the exact leased v1 backend, rejecting stale descriptions.
 * @param generation Acquired generation; zero preserves the native v1 API.
 * @param data Borrowed sample bytes. @param length Byte count.
 * @param status Optional status output initialized to NO_DEVICE.
 * @return Transferred bytes or errno. Task callback pinned through return.
 */
long driver_manager_audio_write_bound(uint32_t generation, const void *data,
                                      uint32_t length, uint32_t *status);
/** @brief Snapshot the exact leased backend without consulting its replacement.
 * @param generation Acquired generation; zero preserves the native v1 API.
 * @param state Receives zero on absence or the pinned hardware state.
 * @return Zero or -ENODEV. Task callback pinned through return.
 */
int driver_manager_audio_state_bound(uint32_t generation, struct reliefos_audio_state *state);

#endif

#define _POSIX_C_SOURCE 200809L
#include <reliefnt/console.h>
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "ahci_memory_fixture.h"
#include "../../block/core/storage_internal.h"
#include "../../block/core/storage_state_data.c"

static int task_file_errors;
static int timeouts;
static uint32_t fixture_pid=7;
bool storage_async_can_yield(void) { return storage_io_async_context; }
void storage_memzero(void *out, size_t n) { memset(out, 0, n); }
uint64_t time_ticks(void) { return 1; }
uint32_t sched_current_pid(void) { return fixture_pid; }
void storage_release_task_io(uint32_t pid) { (void)pid; }
static void fixture_console_vprintf(const char *format, va_list arguments)
{
    if (strstr(format, "ahci %s")) {
        const char *reason = va_arg(arguments, const char *);
        task_file_errors += !strcmp(reason, "task-file error");
        timeouts += strstr(reason, "timeout") != NULL;
    }
}

void console_printf(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fixture_console_vprintf(format, arguments);
    va_end(arguments);
}

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level;
    va_list arguments;
    va_start(arguments, format);
    fixture_console_vprintf(format, arguments);
    va_end(arguments);
}

#include "../../drivers/storage/ahci/storage_ahci.c"

/* Model hardware completing an already-issued slot after synchronous polling
 * begins. These are fake MMIO registers, not a hardware or guest test. */
struct fixture_completion {struct ahci_hba_port *port; bool fail;};
static void *fixture_complete_later(void *argument)
{
    struct fixture_completion *completion=argument;
    struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);
    if(completion->fail)completion->port->is=AHCI_PORT_IS_TFES;
    else completion->port->ci=0;
    return NULL;
}

int main(void)
{
    struct ahci_hba_port port = {0};
    for (int asynchronous = 0; asynchronous < 2; ++asynchronous) {
        storage_io_async_context = asynchronous;
        port.ci = 1;
        port.is = AHCI_PORT_IS_TFES;
        ahci_pending_command.port = &port;
        ahci_pending_command.active = 1;
        ahci_pending_command.start_tick = 1;
        assert(ahci_pending_poll(NULL) == -5);
        assert(!ahci_pending_command.active);
        assert(port.ci == 1); /* Software must not clear a hardware-owned bit. */
        assert(!timeouts && task_file_errors == asynchronous + 1);
    }
    port.ci = 0;
    port.is = 0;
    ahci_pending_command.port = &port;
    ahci_pending_command.active = 1;
    assert(ahci_pending_poll(NULL) == 0 && !ahci_pending_command.active);
    port.ci = 1;
    ahci_pending_command.port = &port;
    ahci_pending_command.active = 1;
    ahci_pending_command.start_tick = 1;
    assert(ahci_pending_poll(NULL) == -RELIEFOS_EAGAIN && ahci_pending_command.active);
    ahci_pending_clear();
    assert(!ahci_setup_port(&port, UINT32_MAX));
    struct ahci_port_dma *dma = ahci_dma_find(&port);
    assert(dma && port.clb == dma->descriptors.physical && !port.clbu);
    assert((((uint64_t)port.fbu << 32) | port.fb) == dma->descriptors.physical + AHCI_DMA_FIS_OFFSET);
    port.ci = 0; port.is = 0; /* Hardware boundary after W1C/completion. */
    char current[513], expected[512];
    memset(current, 0x7b, sizeof(current)); memset(expected, 0x2a, sizeof(expected));
    assert(!ahci_prepare_command(dma, AHCI_READ, 99, 1, current + 1));
    memcpy(dma->data.cpu, expected, 512);
    /* Deliberately stale identity: the poll function must not dereference it. */
    ahci_pending_command = (struct ahci_pending_command){ .active = 1,
        .port = &port, .dma = dma, .buffer = (void *)(uintptr_t)1, .owner_pid = 7 };
    assert(!ahci_pending_poll(current + 1));
    assert(current[0] == 0x7b && !memcmp(current + 1, expected, 512));
    assert(!dma->data.busy && !ahci_pending_command.active);

    assert(!ahci_prepare_command(dma, AHCI_READ, 99, 1, current + 1));
    ahci_pending_command = (struct ahci_pending_command){ .active = 1,
        .port = &port, .dma = dma, .buffer = (void *)(uintptr_t)1, .owner_pid = 7 };
    ahci_cancel_task(7); /* Discard completed data, never copy to address 1. */
    assert(!dma->data.busy && !ahci_pending_command.active);

    assert(!ahci_prepare_command(dma, AHCI_PACKET_READ, 2, 32, current));
    struct ahci_cmd_table *table = ahci_dma_table(dma);
    assert(table->acmd[0] == SCSI_CMD_READ10 && table->acmd[5] == 2 && table->acmd[8] == 32);
    assert(table->prdt[0].dbc == 65535);
    assert(!dma_buffer_end(&dma->data, NULL, true));
    assert(!ahci_prepare_command(dma,AHCI_PACKET_CAPACITY,0,1,current));
    assert(table->acmd[0]==0x25 && !table->acmd[8] && table->prdt[0].dbc==7);
    assert(!dma_buffer_end(&dma->data,NULL,true));
    /* Deferred cleanup may execute as the real idle PID. Forced synchronous
     * completion still owns its DMA and must not be cancelled as PID 7. */
    fixture_pid=0;storage_io_async_context=false;
    assert(!ahci_prepare_command(dma, AHCI_READ, 99, 1, current+1));
    memcpy(dma->data.cpu,expected,512);
    ahci_pending_command=(struct ahci_pending_command){.active=1,.port=&port,.dma=dma,
        .buffer=current+1,.owner_pid=0,.lba=99,.sector_count=1,.operation=AHCI_READ};
    assert(ahci_pending_matches(&port,99,1,current+1,AHCI_READ));
    ahci_cancel_task(7);
    assert(ahci_pending_command.active && dma->data.busy);
    port.ci=1;port.is=0;
    struct fixture_completion completion={.port=&port};pthread_t hardware;
    assert(!pthread_create(&hardware,NULL,fixture_complete_later,&completion));
    assert(!ahci_transfer_once(&port,AHCI_READ,99,1,current+1));
    assert(!pthread_join(hardware,NULL));
    assert(!ahci_pending_command.active && !dma->data.busy && !memcmp(current+1,expected,512));
    assert(!ahci_prepare_command(dma, AHCI_IDENTIFY, 0, 1, current));
    assert(((struct fis_reg_h2d *)(void *)table->cfis)->command == ATA_CMD_IDENTIFY_DEVICE);
    ahci_pending_command = (struct ahci_pending_command){ .active = 1,
        .port = &port, .dma = dma, .buffer = current, .owner_pid = 0,
        .operation = AHCI_IDENTIFY, .sector_count = 1 };
    port.ci = 1; port.is = 0;completion.fail=true;
    assert(!pthread_create(&hardware,NULL,fixture_complete_later,&completion));
    assert(ahci_transfer_once(&port,AHCI_IDENTIFY,0,1,current) == -5 && !ahci_pending_command.active);
    assert(!pthread_join(hardware,NULL));
    assert(dma->data.busy && dma->quarantined && port.ci == 1);
    port.cmd = AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR | AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE;
    unsigned before = freed;
    assert(ahci_release_ports() == -5);
    assert(freed == before && ahci_dma_find(&port) == dma && dma->data.busy);
    port.cmd = 0; /* External hardware STOP finally acknowledged. */
    assert(!ahci_release_ports() && !ahci_dma_find(&port));
    assert(!heap_objects && allocated == freed);
    puts("AHCI completion: retry/cancel, real owned DMA/IDENTIFY/ATAPI, failed STOP retention PASS");
}

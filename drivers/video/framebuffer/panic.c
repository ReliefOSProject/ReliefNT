#include <reliefnt/framebuffer.h>
#include <reliefnt/graphics_service.h>

void framebuffer_panic_prepare(void)
{
    graphics_service_panic_prepare();
}

void framebuffer_panic_present(void)
{
    graphics_service_panic_present();
}

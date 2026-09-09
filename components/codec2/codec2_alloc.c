#include <stddef.h>
#include "esp_heap_caps.h"

/* Codec2 runs in task context with the cache enabled. Its private working
 * storage can use PSRAM; the USB/I2S DMA buffers and task stacks need SRAM. */
void *codec2_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *codec2_calloc(size_t count, size_t size)
{
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void codec2_free(void *ptr)
{
    heap_caps_free(ptr);
}

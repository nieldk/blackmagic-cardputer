#include "target_lock.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t g_target_lock;

void target_lock_init(void)
{
	if (!g_target_lock)
		g_target_lock = xSemaphoreCreateMutex();
}

void target_lock(void)
{
	if (g_target_lock)
		xSemaphoreTake(g_target_lock, portMAX_DELAY);
}

void target_unlock(void)
{
	if (g_target_lock)
		xSemaphoreGive(g_target_lock);
}

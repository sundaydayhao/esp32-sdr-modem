#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_FALSE(test, err, tag, ...) do { if (!(test)) return (err); } while (0)

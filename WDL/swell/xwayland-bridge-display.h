#pragma once

#include <sys/types.h>
#include <stddef.h>

pid_t xw_spawn_xwayland(char *display_name, size_t size);

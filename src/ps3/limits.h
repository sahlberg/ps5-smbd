/* The toolchain's fixed limits.h does not define PATH_MAX. The newlib syslimits.h value is 1024. */
#pragma once
#include_next <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

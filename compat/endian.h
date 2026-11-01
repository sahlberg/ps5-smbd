/* libdcerpc includes <endian.h> (glibc). FreeBSD, and so the PS5, has the same functions in <sys/endian.h>.
   The PS3 is big-endian and has neither header. */
#pragma once
#if defined(__PPU__) || defined(PS3_PPU_PLATFORM)
#include <stdint.h>
#define htobe16(x) ((uint16_t)(x))
#define htobe32(x) ((uint32_t)(x))
#define htobe64(x) ((uint64_t)(x))
#define be16toh(x) ((uint16_t)(x))
#define be32toh(x) ((uint32_t)(x))
#define be64toh(x) ((uint64_t)(x))
#define htole16(x) __builtin_bswap16((uint16_t)(x))
#define htole32(x) __builtin_bswap32((uint32_t)(x))
#define htole64(x) __builtin_bswap64((uint64_t)(x))
#define le16toh(x) __builtin_bswap16((uint16_t)(x))
#define le32toh(x) __builtin_bswap32((uint32_t)(x))
#define le64toh(x) __builtin_bswap64((uint64_t)(x))
#elif defined(__linux__)
#include_next <endian.h>
#else
#include <sys/endian.h>
#endif

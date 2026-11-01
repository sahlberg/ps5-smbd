/* lstat is declared on SPU and RTEMS only. The PS3 has no symlinks; ps3.c's lstat is stat. */
#pragma once
#include_next <sys/stat.h>
int lstat(const char *path, struct stat *st);

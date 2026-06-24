#pragma once

#include <stdio.h>

extern FILE *logFile;

#define LOG(fmt, ...) do { \
    if (logFile) fprintf(logFile, fmt "\n", ##__VA_ARGS__); \
} while (0)

#pragma once

#if defined(__GNUC__) || defined(__clang__)
#define BES_API __attribute__((visibility("default")))
#define BES_HIDDEN __attribute__((visibility("hidden")))
#else
#define BES_API
#define BES_HIDDEN
#endif

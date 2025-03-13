/* Wrapper for libusockets.h */
#ifndef LIBUSOCKETS_H
#define LIBUSOCKETS_H

/* This is a stubbed header - the real implementation is linked differently */
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Just enough definitions to make uWebSockets compile */
struct us_socket_context_t;
struct us_listen_socket_t;
struct us_socket_t;
struct us_loop_t;

/* These are just stubs - the real functionality is provided by the build system */
#ifdef __cplusplus
}
#endif

#endif /* LIBUSOCKETS_H */

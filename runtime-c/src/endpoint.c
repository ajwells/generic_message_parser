/* Library build of the GMP C runtime: the functions declared in gmp/c/endpoint.h (see GMSG_RUNTIME_LIBRARY in
 * gmp/c/config.h). Every GMSG_ option must have the same value here as in the code using the library. */
#ifndef GMSG_RUNTIME_LIBRARY
#define GMSG_RUNTIME_LIBRARY 1
#endif
#if !GMSG_RUNTIME_LIBRARY
#error "runtime-c/src is the library build: GMSG_RUNTIME_LIBRARY must be 1"
#endif
#define GMSG_IMPL_ENDPOINT_
#include "gmp/c/endpoint.h"



#ifndef INCANDESCENT_LOOKUP_H
#define INCANDESCENT_LOOKUP_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <GL/gl.h>

    GLAPI GLAPIENTRY void* glXGetProcAddress(const char* name);
    GLAPI GLAPIENTRY void* glXGetProcAddressARB(const char* name);

#ifdef __cplusplus
}
#endif

#endif

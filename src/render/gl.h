#pragma once
//
// Cross-platform GL header. Linux pulls in GL/gl.h with GL_GLEXT_PROTOTYPES
// so the 3.x core entry points are linked from libGL. macOS exposes GL 3.2/4.1
// core via OpenGL/gl3.h.
//

#ifdef __APPLE__
  #include <OpenGL/gl3.h>
#else
  #ifndef GL_GLEXT_PROTOTYPES
    #define GL_GLEXT_PROTOTYPES
  #endif
  #include <GL/gl.h>
#endif

/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once
// OpenGLClean.h - Clean OpenGL headers without Windows conflicts

#ifdef _WIN32
// Include Windows headers first with minimal pollution
#include "platform/WindowsHeaders.h"
#endif

// OpenGL headers - platform independent
#ifdef _WIN32
    #include <GL/gl.h>
    #include <GL/glu.h>
    // OpenGL extensions
    #include <GL/glext.h>
    #include <GL/wglext.h>
#elif defined(__APPLE__)
    #include <OpenGL/gl.h>
    #include <OpenGL/glu.h>
    #include <OpenGL/glext.h>
#else
    #include <GL/gl.h>
    #include <GL/glu.h>
    #include <GL/glext.h>
#endif

namespace Quanta {
namespace Graphics {

// OpenGL utility functions
class OpenGLContext {
public:
    static bool Initialize();
    static void Cleanup();
    static bool IsSupported();
    static const char* GetVersion();
    static const char* GetRenderer();
    static const char* GetVendor();
    
private:
#ifdef _WIN32
    static HGLRC context;
    static HDC deviceContext;
#endif
};

// OpenGL error checking
#ifdef DEBUG
#define GL_CHECK(call) \
    do { \
        call; \
        GLenum error = glGetError(); \
        if (error != GL_NO_ERROR) { \
            std::cerr << "OpenGL error " << error << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        } \
    } while(0)
#else
#define GL_CHECK(call) call
#endif

} // namespace Graphics
} // namespace Quanta
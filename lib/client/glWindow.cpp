
/* Copyright (c) 2005-2009, Stefan Eilemann <eile@equalizergraphics.com>
                          , Makhinya Maxim
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *  
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "glWindow.h"

#include "global.h"
#include "pipe.h"

#include <eq/util/frameBufferObject.h>

namespace eq
{

GLWindow::GLWindow( Window* parent )
    : OSWindow( parent )
    , _glewInitialized( false )
    , _glewContext( new GLEWContext )
    , _fbo( 0 )
{
}

GLWindow::~GLWindow()
{
    _glewInitialized = false;
    delete _glewContext;
}

void GLWindow::makeCurrent() const 
{
    bindFrameBuffer();
    getPipe()->setCurrent( _window );
}
    
void GLWindow::initGLEW()
{
    if( _glewInitialized )
        return;

    const GLenum result = glewInit();
    if( result != GLEW_OK )
        _window->setErrorMessage( "GLEW initialization failed: " + result );
    else
        _glewInitialized = true;
}
    
GLEWContext* GLWindow::glewGetContext()
{
    return _glewContext;
}

const GLEWContext* GLWindow::glewGetContext() const
{
    return _glewContext;
}

bool GLWindow::configInitFBO()
{
    if( !_glewInitialized ||
        !GLEW_ARB_texture_non_power_of_two ||
        !GLEW_EXT_framebuffer_object )
    {
        _window->setErrorMessage( "Framebuffer objects unsupported" );
         return false;
    }
    
    // needs glew initialized (see above)
    _fbo = new util::FrameBufferObject( _glewContext );
    
    const PixelViewport& pvp = _window->getPixelViewport();
    const GLuint colorFormat = _window->getColorFormat();

    int depthSize = getIAttribute( Window::IATTR_PLANES_DEPTH );
    if( depthSize == AUTO )
         depthSize = 24;

    int stencilSize = getIAttribute( Window::IATTR_PLANES_STENCIL );
    if( stencilSize == AUTO )
        stencilSize = 1;

    if( _fbo->init( pvp.w, pvp.h, colorFormat, depthSize, stencilSize ))
    {
        return true;
    }
    if( getIAttribute( Window::IATTR_PLANES_STENCIL ) == AUTO &&
        _fbo->init( pvp.w, pvp.h, colorFormat, depthSize, 0 ))
    {
        return true;
    }

    _window->setErrorMessage( "FBO initialization failed: " + 
                              _fbo->getErrorMessage( ));
    delete _fbo;
    _fbo = 0;
    return false;
}

void GLWindow::configExitFBO()
{   
    if( _fbo )
        _fbo->exit();

    delete _fbo;
    _fbo = 0;
}

void GLWindow::bindFrameBuffer() const 
{
   if( !_glewInitialized )
       return;
    
   if( _fbo )
       _fbo->bind();
   else if( GLEW_EXT_framebuffer_object )
       glBindFramebufferEXT( GL_FRAMEBUFFER_EXT, 0 );
}

void GLWindow::queryDrawableConfig( DrawableConfig& drawableConfig )
{
    // GL version
    const char* glVersion = (const char*)glGetString( GL_VERSION );
    if( !glVersion ) // most likely no context - fail
    {
        EQWARN << "glGetString(GL_VERSION) returned 0, assuming GL version 1.1" 
               << std::endl;
        drawableConfig.glVersion = 1.1f;
    }
    else
        drawableConfig.glVersion = static_cast<float>( atof( glVersion ));
        
    // Framebuffer capabilities
    GLboolean result;
    glGetBooleanv( GL_STEREO,       &result );
    drawableConfig.stereo = result;
        
    glGetBooleanv( GL_DOUBLEBUFFER, &result );
    drawableConfig.doublebuffered = result;

    GLint stencilBits;
    glGetIntegerv( GL_STENCIL_BITS, &stencilBits );
    drawableConfig.stencilBits = stencilBits;
        
    GLint colorBits;
    glGetIntegerv( GL_RED_BITS, &colorBits );
    drawableConfig.colorBits = colorBits;

    GLint alphaBits;
    glGetIntegerv( GL_ALPHA_BITS, &alphaBits );
    drawableConfig.alphaBits = alphaBits;

    GLint accumBits;
    glGetIntegerv( GL_ACCUM_RED_BITS, &accumBits );
    drawableConfig.accumBits = accumBits * 4;
        
    EQINFO << "Window drawable config: " << drawableConfig << std::endl;
}
    
}


/*
 * Copyright (c)
 *   2008-2009, Thomas McGuire <thomas.mcguire@student.uni-siegen.de>
 *   2010, Stefan Eilemann <eile@eyescale.ch>
 *   2010, Sarah Amsellem <sarah.amsellem@gmail.com>
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

#include "node.h"

#include "config.h"
#include "sceneReader.h"
#include "quad.h"

#include <osg/MatrixTransform>
#include <osg/Texture2D>

namespace osgScaleViewer
{

Node::Node( eq::Config* parent )
    : eq::Node( parent )
{
}

bool Node::configInit( const uint32_t initID )
{
    if( !eq::Node::configInit( initID ))
        return false;

    Config* config = static_cast<Config*>( getConfig( ));
    if( !config->mapData( initID ))
        return false;

    _frameStamp = new osg::FrameStamp;
    _updateVisitor = new osg::NodeVisitor;
    _updateVisitor->setFrameStamp( _frameStamp );
    _contextID = 0;

    // load model at first config run
    if( !_model )
    {
        const InitData& initData = config->getInitData();
        const std::string& modelFile = initData.getModelFileName();    
        if( !modelFile.empty( ))
        {
            SceneReader sceneReader;
            _model = sceneReader.readModel( modelFile );

            if( _model.valid( ))
            {
                osg::Matrix matrix;
                matrix.makeRotate( -osg::PI_2, osg::Vec3( 1., 0., 0. ));

                osg::ref_ptr<osg::MatrixTransform> transform =
                    new osg::MatrixTransform();
                transform->setMatrix( matrix );
                transform->addChild( _model );
                transform->setDataVariance( osg::Object::STATIC );

                _model = transform;
            }
        }
    }

    if( !_model )
    {
        const InitData& initData = config->getInitData();
        std::string imageFile = initData.getImageFileName();
        if( !imageFile.empty( ))
        {
            SceneReader sceneReader;
            osg::ref_ptr<osg::Image> image = sceneReader.readImage( imageFile );
            if( image.valid( ))
                _model = _createSceneGraph( image );
        }
    }

    if( !_model )
        _model = _createSceneGraph();
    return true;
}

bool Node::configExit()
{
    _contextID = 0;
    _frameStamp = 0;
    _updateVisitor = 0;
    return eq::Node::configExit();
}


void Node::frameStart( const uint32_t frameID, const uint32_t frameNumber )
{
    _frameStamp->setFrameNumber( frameNumber );

    // TODO use global time saved in FrameData, use one FrameData per node
    const double time = static_cast< double >( getConfig()->getTime( )) / 1000.;
    _frameStamp->setReferenceTime( time );
    _frameStamp->setSimulationTime( time );
    _updateVisitor->setTraversalNumber( frameNumber );
    _model->accept( *_updateVisitor );
    _model->getBound();

    eq::Node::frameStart( frameID, frameNumber );
}

osg::ref_ptr< osg::Node > Node::_createSceneGraph()
{
    // init scene graph
    osg::ref_ptr<osg::Group> root = _initSceneGraph(); 

    // draw a red quad
    Quad quad;
    osg::ref_ptr<osg::Node> geometryChild = quad.createQuad();
    root->addChild( geometryChild );

    return root.get();
}

osg::ref_ptr< osg::Node > Node::_createSceneGraph(
    osg::ref_ptr< osg::Image > image )
{
    // init scene graph
    osg::ref_ptr<osg::Group> root = _initSceneGraph(); 

    // det the image as a texture
    osg::Texture2D* texture = new osg::Texture2D();
    texture->setImage( image );
    
    // draw a textured quad
    Quad quad;
    osg::ref_ptr<osg::Node> geometryChild = quad.createQuad( image->s(),
                                                             image->t( ));

    osg::StateSet* stateOne = new osg::StateSet();
    stateOne->setTextureAttributeAndModes( 0, texture, osg::StateAttribute::ON);
    geometryChild->setStateSet( stateOne );

    root->addChild( geometryChild );

    return root.get();
}

osg::ref_ptr< osg::Group > Node::_initSceneGraph()
{
    osg::ref_ptr<osg::Group> root = new osg::Group();
    root->setDataVariance( osg::Object::STATIC );

    // enable lighting and LIGHT0 in root node
    osg::StateSet* state = root->getOrCreateStateSet();
    state->setMode( GL_LIGHTING, osg::StateAttribute::ON );
    state->setMode( GL_LIGHT0, osg::StateAttribute::ON );

    // lightsource
    osg::ref_ptr<osg::LightSource> ls0 = _createLightSource();

    // translation of lightsource (with light0)
    osg::Matrix matrix;
    osg::ref_ptr<osg::MatrixTransform> lightTranslateNode = 
             new osg::MatrixTransform();

    matrix.makeTranslate( 0.f, -5.f, 0.f );
    lightTranslateNode->setMatrix( matrix );
    lightTranslateNode->addChild( ls0.get( ));
    lightTranslateNode->setDataVariance( osg::Object::STATIC );

    root->addChild( lightTranslateNode.get( ));

    return root;
}

osg::ref_ptr< osg::LightSource > Node::_createLightSource()
{
    osg::ref_ptr<osg::Light> light0 = new osg::Light();
    light0->setLightNum( 0 ); //light0 is now GL-Light0
    light0->setPosition( osg::Vec4( 0.f, -50.f, 0.f, 1.f ));
    light0->setAmbient( osg::Vec4( 1.0f, 1.0f, 1.0f, 0.0f ));
    light0->setDiffuse( osg::Vec4( 1.0f, 1.0f, 1.0f, 0.0f ));
    light0->setDirection( osg::Vec3( 0.f, 1.f, 0.f ));
    light0->setSpotCutoff( 30.f );
    light0->setSpotExponent( 50.0f );

    osg::ref_ptr< osg::LightSource > ls0 = new osg::LightSource();
    ls0->setLight( light0.get( ));
    ls0->setDataVariance( osg::Object::STATIC );

    return ls0;
}


}
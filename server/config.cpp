
/* Copyright (c) 2005-2008, Stefan Eilemann <eile@equalizergraphics.com> 
   All rights reserved. */

#include <pthread.h>
#include "config.h"

#include "compound.h"
#include "compoundVisitor.h"
#include "constCompoundVisitor.h"
#include "configUpdateDataVisitor.h"
#include "global.h"
#include "loadBalancer.h"
#include "log.h"
#include "node.h"
#include "server.h"

#include <eq/net/command.h>
#include <eq/net/global.h>
#include <eq/base/sleep.h>

using namespace eq::base;
using namespace std;
using eq::net::ConnectionDescriptionVector;

namespace eq
{
namespace server
{
typedef net::CommandFunc<Config> ConfigFunc;

#define MAKE_ATTR_STRING( attr ) ( string("EQ_CONFIG_") + #attr )
std::string Config::_fAttributeStrings[FATTR_ALL] = 
{
    MAKE_ATTR_STRING( FATTR_EYE_BASE )
};

#define TRAVERSE_COMPOUNDS( compounds, visitor, activeOnly )    \
    for( CompoundVector::const_iterator i = compounds.begin();  \
         i != compounds.end(); ++i )                            \
    {                                                           \
        Compound* compound = *i;                                \
        compound->accept( &visitor, activeOnly );               \
    }                                                           

namespace
{
class ViewMapper : public CompoundVisitor
{
public:
    ViewMapper( const uint32_t nViews, const uint32_t* viewIDs ) 
            : _current( 0 ), _nViews( nViews ), _viewIDs( viewIDs ) {}
    virtual ~ViewMapper(){}

    virtual Compound::VisitorResult visit( Compound* compound )
        { 
            switch( compound->getLatestView( ))
            {
                case eq::View::TYPE_WALL:
                case eq::View::TYPE_PROJECTION:
                {
                    EQASSERT( _current < _nViews );
                    eq::View& view   = compound->getView();
                    Config*   config = compound->getConfig();
                    
                    EQCHECK( config->mapObject( &view, _viewIDs[ _current ] ));
                    ++_current;
                    break;
                }

                case eq::View::TYPE_NONE:
                    break;
                default:
                    EQUNIMPLEMENTED;
            }

            return Compound::TRAVERSE_CONTINUE; 
        }

private:
    uint32_t        _current;
    uint32_t        _nViews;
    const uint32_t* _viewIDs;
};

class ViewUnmapper : public CompoundVisitor
{
public:
    virtual ~ViewUnmapper(){}

    virtual Compound::VisitorResult visit( Compound* compound )
        { 
            switch( compound->getLatestView( ))
            {
                case eq::View::TYPE_WALL:
                case eq::View::TYPE_PROJECTION:
                {
                    eq::View& view = compound->getView();
                    if( view.getID() != EQ_ID_INVALID )
                    {
                        Config* config = compound->getConfig();
                        config->unmapObject( &view );
                    }
                    break;
                }

                case eq::View::TYPE_NONE:
                    break;
                default:
                    EQUNIMPLEMENTED;
            }

            return Compound::TRAVERSE_CONTINUE; 
        }
};
}

void Config::_construct()
{
    _latency       = 1;
    _currentFrame  = 0;
    _finishedFrame = 0;
    _state         = STATE_STOPPED;
    _appNode       = 0;
    _distributor   = 0;

    EQINFO << "New config @" << (void*)this << endl;
}

Config::Config()
{
    _construct();

    const Global* global = Global::instance();
    
    for( int i=0; i<FATTR_ALL; ++i )
        _fAttributes[i] = global->getConfigFAttribute( (FAttribute)i );
}

Config::Config( const Config& from )
        : net::Session()
        , _server( from._server )
{
    _construct();
    _appNetNode = from._appNetNode;
    _latency    = from._latency;

    const CompoundVector& compounds = from.getCompounds();
    for( CompoundVector::const_iterator i = compounds.begin(); 
         i != compounds.end(); ++i )
    {
        const Compound* compound      = *i;
        Compound*       compoundClone = new Compound( *compound );

        addCompound( compoundClone );
        // channel is replaced in channel copy constructor
    }

    for( int i=0; i<FATTR_ALL; ++i )
        _fAttributes[i] = from.getFAttribute( (FAttribute)i );
        
    const NodeVector& nodes = from.getNodes();
    for( NodeVector::const_iterator i = nodes.begin(); i != nodes.end(); ++i )
    {
        const Node* node      = *i;
        Node*       nodeClone = new Node( *node, _compounds );
        
        (node == from._appNode) ? 
            addApplicationNode( nodeClone ) : addNode( nodeClone );
    }
}

Config::~Config()
{
    EQINFO << "Delete config @" << (void*)this << endl;
    _server     = 0;
    _appNode    = 0;
    _appNetNode = 0;

    ViewUnmapper unmapper;
    for( vector<Compound*>::const_iterator i = _compounds.begin(); 
         i != _compounds.end(); ++i )
    {
        Compound* compound = *i;

        compound->accept( &unmapper );
        compound->_config = 0;
        delete compound;
    }
    _compounds.clear();

    for( NodeVector::const_iterator i = _nodes.begin(); i != _nodes.end(); ++i )
    {
        Node* node = *i;

        node->_config = 0;
        delete node;
    }
    _nodes.clear();
}

void Config::setLocalNode( net::NodePtr node )
{
    net::Session::setLocalNode( node );
    
    if( !node ) 
        return;

    net::CommandQueue* serverQueue  = getServerThreadQueue();
    net::CommandQueue* commandQueue = getCommandThreadQueue();

    registerCommand( eq::CMD_CONFIG_START_INIT,
                     ConfigFunc( this, &Config::_cmdStartInit), serverQueue );
    registerCommand( eq::CMD_CONFIG_FINISH_INIT,
                     ConfigFunc(this, &Config::_cmdFinishInit), serverQueue );
    registerCommand( eq::CMD_CONFIG_EXIT,
                     ConfigFunc( this, &Config::_cmdExit ), serverQueue );
    registerCommand( eq::CMD_CONFIG_CREATE_REPLY,
                     ConfigFunc( this, &Config::_cmdCreateReply ),
                     commandQueue );
    registerCommand( eq::CMD_CONFIG_CREATE_NODE_REPLY,
                     ConfigFunc( this, &Config::_cmdCreateNodeReply ),
                     commandQueue );
    registerCommand( eq::CMD_CONFIG_START_FRAME, 
                     ConfigFunc( this, &Config::_cmdStartFrame ), serverQueue );
    registerCommand( eq::CMD_CONFIG_FINISH_ALL_FRAMES, 
                     ConfigFunc( this, &Config::_cmdFinishAllFrames ),
                     serverQueue );
    registerCommand( eq::CMD_CONFIG_FREEZE_LOAD_BALANCING, 
                     ConfigFunc( this, &Config::_cmdFreezeLoadBalancing ), 
                     serverQueue );
    registerCommand( eq::CMD_CONFIG_MAP_VIEWS,
                     ConfigFunc( this, &Config::_cmdMapViews ), serverQueue );
}

void Config::addNode( Node* node )
{
    _nodes.push_back( node ); 
    
    node->_config = this; 
}

bool Config::removeNode( Node* node )
{
    NodeVector::iterator i = find( _nodes.begin(), _nodes.end(), node );
    if( i == _nodes.end( ))
        return false;

    _nodes.erase( i );
    node->_config = 0; 

    return true;
}

void Config::addCompound( Compound* compound )
{
    compound->_config = this;
    _compounds.push_back( compound );
}

bool Config::removeCompound( Compound* compound )
{
    vector<Compound*>::iterator i = find( _compounds.begin(), _compounds.end(),
                                          compound );
    if( i == _compounds.end( ))
        return false;

    _compounds.erase( i );
    compound->_config = 0;
    return true;
}

Channel* Config::findChannel( const std::string& name )
{
    for( NodeVector::const_iterator i = _nodes.begin(); i != _nodes.end(); ++i )
    {
        const Node*       node  = *i;
        const PipeVector& pipes = node->getPipes();
        for( PipeVector::const_iterator j = pipes.begin();
             j != pipes.end(); ++j )
        {
            const Pipe*         pipe    = *j;
            const WindowVector& windows = pipe->getWindows();
            for( WindowVector::const_iterator k = windows.begin(); 
                 k != windows.end(); ++k )
            {
                const Window*        window   = *k;
                const ChannelVector& channels = window->getChannels();
                for( ChannelVector::const_iterator l = channels.begin();
                     l != channels.end(); ++l )
                {
                    const Channel* channel = *l;
                    if( channel->getName() == name )
                        return *l;
                }
            }
        }
    }
    return 0;
}

void Config::addApplicationNode( Node* node )
{
    EQASSERT( _state == STATE_STOPPED );
    EQASSERTINFO( !_appNode, "Only one application node per config possible" );

    _appNode = node;
    addNode( node );
}

void Config::setApplicationNetNode( net::NodePtr node )
{
    EQASSERT( _state == STATE_STOPPED );
    EQASSERT( !_appNetNode );

    _appNetNode = node;
}

ConfigVisitor::Result Config::accept( ConfigVisitor* visitor )
{ 
    NodeVisitor::Result result = visitor->visitPre( this );
    if( result != NodeVisitor::TRAVERSE_CONTINUE )
        return result;

    for( NodeVector::const_iterator i = _nodes.begin(); 
         i != _nodes.end(); ++i )
    {
        Node* node = *i;
        switch( node->accept( visitor ))
        {
            case NodeVisitor::TRAVERSE_TERMINATE:
                return NodeVisitor::TRAVERSE_TERMINATE;

            case NodeVisitor::TRAVERSE_PRUNE:
                result = NodeVisitor::TRAVERSE_PRUNE;
                break;
                
            case NodeVisitor::TRAVERSE_CONTINUE:
            default:
                break;
        }
    }

    switch( visitor->visitPost( this ))
    {
        case NodeVisitor::TRAVERSE_TERMINATE:
	  return NodeVisitor::TRAVERSE_TERMINATE;

        case NodeVisitor::TRAVERSE_PRUNE:
	  return NodeVisitor::TRAVERSE_PRUNE;
	  break;
                
        case NodeVisitor::TRAVERSE_CONTINUE:
        default:
	  break;
    }

    return result;
}

//===========================================================================
// operations
//===========================================================================

//---------------------------------------------------------------------------
// init
//---------------------------------------------------------------------------
namespace
{
class ViewSerializer : public CompoundVisitor
{
public:
    ViewSerializer( net::DataOStream& os ) : _os( os ) {}
    virtual ~ViewSerializer(){}

    virtual Compound::VisitorResult visit( Compound* compound )
        { 
            View& view = compound->getView();
            switch( view.getCurrentType( ))
            {
                case eq::View::TYPE_WALL:
                case eq::View::TYPE_PROJECTION:
                    if( view.getEyeBase() == 0.f )
                    {
                        Config* config = compound->getConfig();
                        EQASSERT( config );
                        view.setEyeBase( 
                            config->getFAttribute( Config::FATTR_EYE_BASE ));
                    }

                    view.getInstanceData( _os );
                    break;

                case eq::View::TYPE_NONE:
                    break;
                default:
                    EQUNIMPLEMENTED;
            }

            return Compound::TRAVERSE_CONTINUE; 
        }

private:
    net::DataOStream& _os;
};
}

void Config::Distributor::getInstanceData( net::DataOStream& os )
{
    os << _config->_latency;

    ViewSerializer        serializer( os );
    const CompoundVector& compounds = _config->getCompounds();
    TRAVERSE_COMPOUNDS( compounds, serializer, false /* activeOnly */ );

    os << eq::View::TYPE_NONE; // end token

#ifdef EQ_TRANSMISSION_API
#  error TODO transmit node identifiers of used nodes
#endif
}

uint32_t Config::getDistributorID()
{
    EQASSERT( !_distributor );

    _distributor = new Distributor( this );
    registerObject( _distributor );

    return _distributor->getID();
}

bool Config::_startInit( const uint32_t initID )
{
    EQASSERT( _state == STATE_STOPPED );
    _currentFrame  = 0;
    _finishedFrame = 0;

    for( vector< Compound* >::const_iterator i = _compounds.begin();
         i != _compounds.end(); ++i )
    {
        Compound* compound = *i;
        compound->init();
    }

    if( !_connectNodes() || !_initNodes( initID ))
    {
        exit();
        return false;
    }

    _state = STATE_INITIALIZED;
    return true;
}

static net::NodePtr _createNode( Node* node )
{
    net::NodePtr netNode = new net::Node;

    const ConnectionDescriptionVector& descriptions = 
        node->getConnectionDescriptions();
    for( ConnectionDescriptionVector::const_iterator i = descriptions.begin();
         i != descriptions.end(); ++i )
    {
        const net::ConnectionDescription* desc = (*i).get();
        
        netNode->addConnectionDescription( 
            new net::ConnectionDescription( *desc ));
    }

    netNode->setAutoLaunch( true );
    return netNode;
}

bool Config::_connectNodes()
{
    net::NodePtr localNode = getLocalNode();
    EQASSERT( localNode.isValid( ));

    for( NodeVector::const_iterator i = _nodes.begin(); i != _nodes.end(); ++i )
    {
        Node* node = *i;
        if( !node->isUsed( ))
            continue;

        net::NodePtr netNode;

        if( node == _appNode )
            netNode = _appNetNode;
        else
        {
            netNode = _createNode( node );
            netNode->setProgramName( _renderClient );
            netNode->setWorkDir( _workDir );
        }

        if( !localNode->initConnect( netNode ))
        {
            stringstream nodeString;
            nodeString << node;

            _error = string( "Connection to node failed, node does not run " ) +
                     string( "and launch command failed:\n " ) + 
                     nodeString.str();
            EQERROR << "Connection to " << netNode->getNodeID() << " failed." 
                    << endl;
            return false;
        }

        node->setNode( netNode );
    }
    return true;
}

bool Config::_initNodes( const uint32_t initID )
{
    const string& name    = getName();
    bool          success = true;

    net::NodePtr localNode = getLocalNode();
    EQASSERT( localNode.isValid( ));

    eq::ServerCreateConfigPacket createConfigPacket;
    deque<uint32_t>              createConfigRequests;
    createConfigPacket.configID  = getID();
    createConfigPacket.appNodeID = _appNetNode->getNodeID();
    createConfigPacket.appNodeID.convertToNetwork();

    // sync node connect and create configs
    for( NodeVector::const_iterator i = _nodes.begin(); i != _nodes.end(); ++i )
    {
        Node* node = *i;
        if( !node->isUsed( ))
            continue;
        
        net::NodePtr netNode = node->getNode();

        if( !localNode->syncConnect( netNode ))
        {
            stringstream nodeString;
            nodeString << node;

            _error = "Connection of node failed, node did not start:\n " +
                     nodeString.str();
            EQERROR << "Connection of node " << node << " failed." << endl;
            success = false;
        }
        
        if( !success ) 
            continue;

        // create config (session) on each non-app node
        //   app-node already has config
        if( node != _appNode )
        {
            createConfigPacket.requestID = _requestHandler.registerRequest();
            createConfigRequests.push_back( createConfigPacket.requestID );

            netNode->send( createConfigPacket, name );
        }
    }

    if( !success )
    {
        // sync already issued requests
        while( !createConfigRequests.empty( ))
        {
            _requestHandler.waitRequest( createConfigRequests.front( ));
            createConfigRequests.pop_front();
        }
        return false;
    }

    // sync config creation and start node init
    eq::ConfigCreateNodePacket createNodePacket;
    vector<uint32_t>           createNodeRequests;
    for( NodeVector::const_iterator i = _nodes.begin(); i != _nodes.end(); ++i )
    {
        Node* node = *i;
        if( !node->isUsed( ))
            continue;

        registerObject( node );

        if( node != _appNode )
        {
            _requestHandler.waitRequest( createConfigRequests.front( ));
            createConfigRequests.pop_front();
        }

        net::NodePtr netNode = node->getNode();

        createNodePacket.nodeID = node->getID();
        createNodePacket.requestID = _requestHandler.registerRequest();
        createNodeRequests.push_back( createNodePacket.requestID );
        send( netNode, createNodePacket );

        // start node-pipe-window-channel init in parallel on all nodes
        node->startConfigInit( initID );

#ifdef EQ_TRANSMISSION_API
        // TODO move me
        nodeIDs.resize( nodeIDs.size() + 1 );
        netNode->getNodeID().getData( nodeIDs.back( ));
#endif
    }

    // Need to sync eq::Node creation: It is possible, though unlikely, that
    // Config::startInit returns and Config::broadcastData is used before the
    // NodeCreatePacket has been processed by the render node.
    for( vector<uint32_t>::const_iterator i = createNodeRequests.begin();
         i != createNodeRequests.end(); ++i )
    {
        _requestHandler.waitRequest( *i );
    }
    return success;
}

bool Config::_finishInit()
{
    bool success = true;
    for( NodeVector::const_iterator i = _nodes.begin(); i != _nodes.end(); ++i )
    {
        Node*               node    = *i;
        net::NodePtr netNode = node->getNode();
        if( !node->isUsed() || !netNode->isConnected( ))
            continue;
        
        if( !node->syncConfigInit( ))
        {
            _error += ( ' ' + node->getErrorMessage( ));
            EQERROR << "Init of node " << (void*)node << " failed." 
                    << endl;
            success = false;
        }
    }

    // start global clock on all nodes
    eq::ConfigStartClockPacket packet;
    for( NodeVector::const_iterator i = _nodes.begin(); i != _nodes.end(); ++i )
    {
        Node*               node    = *i;
        net::NodePtr netNode = node->getNode();
        if( !node->isUsed() || !netNode->isConnected( ))
            continue;

        send( netNode, packet );
    }
    send( _appNetNode, packet );
    return success;
}

bool Config::exit()
{
    if( _state != STATE_INITIALIZED )
        EQWARN << "Exiting non-initialized config" << endl;

    bool cleanExit = _exitNodes();
    if( !cleanExit )
        EQERROR << "nodes exit failed" << endl;

    for( vector< Compound* >::const_iterator i = _compounds.begin();
         i != _compounds.end(); ++i )
    {
        Compound* compound = *i;
        compound->exit();
    }

    if( _headMatrix.getID() != EQ_ID_INVALID )
        deregisterObject( &_headMatrix );

    _currentFrame  = 0;
    _finishedFrame = 0;
    _state         = STATE_STOPPED;
    return cleanExit;
}

bool Config::_exitNodes()
{
    // invoke configExit task methods and delete resource instances on clients 
    NodeVector exitingNodes;
    for( NodeVector::const_iterator i = _nodes.begin(); i != _nodes.end(); ++i )
    {
        Node*          node = *i;
        net::NodePtr netNode = node->getNode();

        if( !node->isUsed() || !netNode.isValid() ||
            netNode->getState() == net::Node::STATE_STOPPED )

            continue;

        exitingNodes.push_back( node );
        node->startConfigExit();
    }

    // wait for the above and then delete the config and request disconnect
    eq::ServerDestroyConfigPacket destroyConfigPacket;
    destroyConfigPacket.configID  = getID();

    eq::ConfigDestroyNodePacket destroyNodePacket;
    eq::ClientExitPacket        clientExitPacket;
    net::NodePtr         localNode         = getLocalNode();
    EQASSERT( localNode.isValid( ));

    bool success = true;
    for( NodeVector::const_iterator i = exitingNodes.begin();
         i != exitingNodes.end(); ++i )
    {
        Node*          node    = *i;
        net::NodePtr netNode = node->getNode();

        if( !node->syncConfigExit( ))
        {
            EQERROR << "Could not exit cleanly: " << node << endl;
            success = false;
        }
        
        destroyNodePacket.nodeID = node->getID();
        send( netNode, destroyNodePacket );

        if( node != _appNode )
        {
            netNode->send( destroyConfigPacket );
            netNode->send( clientExitPacket );
        }

        deregisterObject( node );
    }

    // now wait that the clients disconnect
    bool hasSlept = false;
    for( NodeVector::const_iterator i = exitingNodes.begin();
        i != exitingNodes.end(); ++i )
    {
        Node*        node    = *i;
        net::NodePtr netNode = node->getNode();

        node->setNode( 0 );

        if( node != _appNode )
        {
            if( netNode->getState() == net::Node::STATE_CONNECTED )
            {
                if( !hasSlept )
                {
                    base::sleep( 1000 );
                    hasSlept = true;
                }

                if( netNode->getState() == net::Node::STATE_CONNECTED )
                    localNode->disconnect( netNode );
            }
        }
    }
    return success;
}

namespace
{
class ViewUpdater : public CompoundVisitor
{
public:
    virtual ~ViewUpdater(){}

    virtual Compound::VisitorResult visit( Compound* compound )
        { 
            if( compound->getLatestView() != eq::View::TYPE_NONE )
                compound->getView().updateHead();

            return Compound::TRAVERSE_CONTINUE; 
        }
};
}

void Config::_updateHead()
{
    if( _headMatrix.getVersion() == _headMatrix.sync( )) // no changes
        return;

    ViewUpdater updater;
    TRAVERSE_COMPOUNDS( _compounds, updater, false /* activeOnly */ );
}

void Config::_prepareFrame( vector< net::NodeID >& nodeIDs )
{
    EQASSERT( _state == STATE_INITIALIZED );
    ++_currentFrame;
    EQLOG( LOG_ANY ) << "----- Start Frame ----- " << _currentFrame << endl;

    _updateHead();

    for( NodeVector::const_iterator i = _nodes.begin(); i != _nodes.end(); ++i )
    {
        Node* node = *i;
        if( node->isUsed( ))
        {
            net::NodePtr netNode = node->getNode();
            nodeIDs.push_back( netNode->getNodeID( ));
        }
    }
}

void Config::_startFrame( const uint32_t frameID )
{
    EQASSERT( _state == STATE_INITIALIZED );

    for( vector< Compound* >::const_iterator i = _compounds.begin(); 
         i != _compounds.end(); ++i )
    {
        Compound* compound = *i;
        compound->update( _currentFrame );
    }
    
    ConfigUpdateDataVisitor configDataVisitor;
    accept( &configDataVisitor );

    for( vector< Node* >::const_iterator i = _nodes.begin(); 
         i != _nodes.end(); ++i )
    {
        Node* node = *i;
        if( node->isUsed( ))
            node->update( frameID, _currentFrame );
    }
}

void Config::notifyNodeFrameFinished( const uint32_t frameNumber )
{
    if( _finishedFrame >= frameNumber ) // node finish already done
        return;

    for( NodeVector::const_iterator i = _nodes.begin(); i != _nodes.end(); ++i )
    {
        const Node* node = *i;
        if( node->isUsed() && node->getFinishedFrame() < frameNumber )
            return;
    }

    // All nodes have finished the frame. Notify the application's config that
    // the frame is finished
    eq::ConfigFrameFinishPacket packet;
    packet.frameNumber = frameNumber;

    // do not use send/_bufferedTasks, not thread-safe!
    packet.sessionID   = getID();
    _appNetNode->send( packet );
    EQLOG( eq::LOG_TASKS ) << "TASK config frame finished  " << &packet << endl;
}

void Config::_flushFrames()
{
    if( _currentFrame == 0 )
        return;

    for( NodeVector::const_iterator i = _nodes.begin(); i != _nodes.end(); ++i )
    {
        Node* node = *i;
        if( node->isUsed( ))
            node->flushFrames( _currentFrame );
    }

    EQLOG( LOG_ANY ) << "--- Flush All Frames -- " << endl;
}

//---------------------------------------------------------------------------
// command handlers
//---------------------------------------------------------------------------
net::CommandResult Config::_cmdStartInit( net::Command& command )
{
    const eq::ConfigStartInitPacket* packet = 
        command.getPacket<eq::ConfigStartInitPacket>();
    eq::ConfigStartInitReplyPacket   reply( packet );
    EQINFO << "handle config start init " << packet << endl;

    _error.clear();
    reply.result = _startInit( packet->initID );
    
    EQINFO << "Config start init " << (reply.result ? "successful": "failed: ") 
           << _error << endl;

    send( command.getNode(), reply, _error );
    return net::COMMAND_HANDLED;
}

net::CommandResult Config::_cmdFinishInit( net::Command& command )
{
    const eq::ConfigFinishInitPacket* packet = 
        command.getPacket<eq::ConfigFinishInitPacket>();
    eq::ConfigFinishInitReplyPacket   reply( packet );
    EQINFO << "handle config finish init " << packet << endl;

    _error.clear();
    reply.result = _finishInit();

    EQINFO << "Config finish init " << (reply.result ? "successful":"failed: ") 
           << _error << endl;

    if( reply.result )
        mapObject( &_headMatrix, packet->headMatrixID );

    send( command.getNode(), reply, _error );
    return net::COMMAND_HANDLED;
}

net::CommandResult Config::_cmdExit( net::Command& command ) 
{
    const eq::ConfigExitPacket* packet = 
        command.getPacket<eq::ConfigExitPacket>();
    eq::ConfigExitReplyPacket   reply( packet );
    EQINFO << "handle config exit " << packet << endl;

    ViewUnmapper unmapper;
    TRAVERSE_COMPOUNDS( _compounds, unmapper, false /* activeOnly */ );

    if( _state == STATE_INITIALIZED )
        reply.result = exit();
    else
        reply.result = false;

    EQINFO << "config exit result: " << reply.result << endl;
    send( command.getNode(), reply );
    return net::COMMAND_HANDLED;
}

net::CommandResult Config::_cmdStartFrame( net::Command& command ) 
{
    const eq::ConfigStartFramePacket* packet = 
        command.getPacket<eq::ConfigStartFramePacket>();
    eq::ConfigStartFrameReplyPacket   reply( packet );
    EQVERB << "handle config frame start " << packet << endl;

    vector< net::NodeID > nodeIDs;
    _prepareFrame( nodeIDs );

    reply.frameNumber = _currentFrame;
    reply.nNodeIDs    = nodeIDs.size();

    for( vector< net::NodeID >::iterator i = nodeIDs.begin(); 
         i != nodeIDs.end(); ++i )

         (*i).convertToNetwork();

    command.getNode()->send( reply, nodeIDs );

    _startFrame( packet->frameID );
    return net::COMMAND_HANDLED;
}

net::CommandResult Config::_cmdFinishAllFrames( net::Command& command ) 
{
    const eq::ConfigFinishAllFramesPacket* packet = 
        command.getPacket<eq::ConfigFinishAllFramesPacket>();
    EQVERB << "handle config all frames finish " << packet << endl;

    _flushFrames();
    return net::COMMAND_HANDLED;
}

net::CommandResult Config::_cmdCreateReply( net::Command& command ) 
{
    const eq::ConfigCreateReplyPacket* packet = 
        command.getPacket<eq::ConfigCreateReplyPacket>();

    _requestHandler.serveRequest( packet->requestID );
    return net::COMMAND_HANDLED;
}

net::CommandResult Config::_cmdCreateNodeReply( net::Command& command ) 
{
    const eq::ConfigCreateNodeReplyPacket* packet = 
        command.getPacket<eq::ConfigCreateNodeReplyPacket>();

    _requestHandler.serveRequest( packet->requestID );
    return net::COMMAND_HANDLED;
}

namespace
{
class FreezeVisitor : public CompoundVisitor
{
public:
    FreezeVisitor( const bool freeze ) : _freeze( freeze ) {}

        /** Visit a non-leaf compound on the down traversal. */
        virtual Compound::VisitorResult visitPre( Compound* compound )
            { 
                LoadBalancer* loadBalancer = compound->getLoadBalancer();
                if( loadBalancer )
                    loadBalancer->setFreeze( _freeze );

                return Compound::TRAVERSE_CONTINUE; 
            }
private:
    const bool _freeze;
};
}

net::CommandResult Config::_cmdFreezeLoadBalancing( net::Command& command ) 
{
    const eq::ConfigFreezeLoadBalancingPacket* packet = 
        command.getPacket<eq::ConfigFreezeLoadBalancingPacket>();

    FreezeVisitor visitor( packet->freeze );
    TRAVERSE_COMPOUNDS( _compounds, visitor, false /* activeOnly */ );

    return net::COMMAND_HANDLED;
}

net::CommandResult Config::_cmdMapViews( net::Command& command ) 
{
    const eq::ConfigMapViewsPacket* packet = 
        command.getPacket<eq::ConfigMapViewsPacket>();
    EQINFO << "handle config map views " << packet << endl;

    // clients have retrieved distributed data
    EQASSERT( _distributor );
    deregisterObject( _distributor );
    delete _distributor;
    _distributor = 0;

    // map views to appNode master instances
    ViewMapper mapper( packet->nViews, packet->viewIDs );
    TRAVERSE_COMPOUNDS( _compounds, mapper, false /* activeOnly */ );

    return net::COMMAND_HANDLED;
}

ostream& operator << ( ostream& os, const Config* config )
{
    if( !config )
        return os;

    os << disableFlush << disableHeader << "config " << endl;
    os << "{" << endl << indent;

    if( !config->getName().empty( ))
        os << "name    \"" << config->getName() << '"' << endl;

    if( config->getLatency() != 1 )
        os << "latency " << config->getLatency() << endl;
    os << endl;

    const float value = config->getFAttribute( Config::FATTR_EYE_BASE );
    if( value != 
        Global::instance()->getConfigFAttribute( Config::FATTR_EYE_BASE ))
    {
        os << "attributes" << endl << "{" << endl << indent;
        os << "eyeBase " << value << endl;
        os << exdent << "}" << endl;
    }

    const NodeVector& nodes = config->getNodes();
    for( NodeVector::const_iterator i = nodes.begin(); i != nodes.end(); ++i )
        os << *i;

    const CompoundVector& compounds = config->getCompounds();
    for( CompoundVector::const_iterator i = compounds.begin(); 
         i != compounds.end(); ++i )

        os << *i;

    os << exdent << "}" << endl << enableHeader << enableFlush;

    return os;
}

}
}

//
//  Server.cpp
//
//  Created by Sarah Denoux on 13/05/13.
//  Copyright (c) 2013 GRAME. All rights reserved.
//
//

#include "Server.h"
#include "utilities.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <openssl/sha.h>

#include "faust/gui/meta.h"
#include "faust/gui/JSONUI.h"

enum {
    ERROR_FACTORY_NOTFOUND,
    ERROR_INSTANCE_NOTCREATED
};

// Declare is called for every metadata coded in the faust DSP
// That way, we get the faust name declared in the faust DSP
struct myMeta : public Meta
{
    string name;
    
    virtual void declare(const char* key, const char* value) {
        if (strcmp(key, "name") == 0) {
            name = value;
        }
    }
};

connection_info_struct::connection_info_struct() 
{   
    fPostprocessor = 0;
    fAnswercode = -1;
    fAnswerstring = "";
    fNameApp = "";
    fFaustCode = "";
    fFactoryKey = "";
    fOptLevel = "";
    fLLVMFactory = 0;
    fIP = "";
    fPort = "";
    fCompression = "";
    fMTU = "";
    fLatency = "";
    fSHAKey = "";
    fInstanceKey = "";
}
    
connection_info_struct::~connection_info_struct()
{}

//------------SLAVE DSP FACTORY-------------------------------

string getJson(connection_info_struct* con_info) 
{
    myMeta metadata;
    metadataDSPFactory(con_info->fLLVMFactory, &metadata);
    con_info->fNameApp = metadata.name;
        
    // This instance is used only to build JSON interface, then it's deleted
    llvm_dsp* dsp = createDSPInstance(con_info->fLLVMFactory);
    JSONUI json(dsp->getNumInputs(), dsp->getNumOutputs());
    dsp->buildUserInterface(&json);
    deleteDSPInstance(dsp);  
      
    return json.JSON();
}

//--------------SLAVE DSP INSTANCE-----------------------------

netjack_dsp::netjack_dsp(llvm_dsp_factory* factory, 
                        const string& compression, 
                        const string& ip, 
                        const string& port, 
                        const string& mtu, 
                        const string& latency) 
                        :fIP(ip), fPort(port), fCompression(compression), 
                        fMTU(mtu), fLatency(latency), 
                        fAudio(NULL)
{
    if (!(fDSP = createDSPInstance(factory))) {
        throw -1;
    }
}

// Desallocation of slave dsp resources
netjack_dsp::~netjack_dsp()
{
    delete fAudio;
    deleteDSPInstance(fDSP);
}

bool netjack_dsp::start() 
{
    return fAudio->start();
}

void netjack_dsp::stop()
{
    fAudio->stop();
}

bool netjack_dsp::open()
{
    fAudio = new netjackaudio_server(atoi(fCompression.c_str()), 
                                        fIP, 
                                        atoi(fPort.c_str()), 
                                        atoi(fMTU.c_str()), 
                                        atoi(fLatency.c_str()));
                                        
    if (!fAudio->init(fName.c_str(), fDSP)) {
        printf("RemoteDSPServer : Init slave audio failed\n");
        return false;
    }
    
     if (!fAudio->start()) {
        printf("RemoteDSPServer : Start slave audio failed\n");
        return false;
    }
      
    return true;
}

//----------------SERVER----------------------------------------

DSPServer::DSPServer(int argc, const char* argv[]):fPort(-1), fDaemon(NULL)
{}

DSPServer::~DSPServer() {}
    
//---- START/STOP SERVER
bool DSPServer::start(int port) 
{
    fPort = port;
    fDaemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY,
                               port, 
                               NULL, 
                               NULL, 
                               &answerToConnection, 
                               this, MHD_OPTION_NOTIFY_COMPLETED, 
                               requestCompleted, NULL, MHD_OPTION_END);
    if (!fDaemon) {
        printf("RemoteDSPServer : MHD_start_daemon failed\n");
        return false;
    }                           

    if (pthread_create(&fThread, NULL, DSPServer::registration, this) != 0) {
        printf("RemoteDSPServer : pthread_create failed\n");
        return false;
    }
    
    return true;
}

void DSPServer::stop()
{
    if (fDaemon) {
//      Faire un truc pour arrêter la boucle
        pthread_cancel(fThread);
        pthread_join(fThread, NULL);
        MHD_stop_daemon(fDaemon);
        fDaemon = 0;
    }
}


//---- Callback of another thread to wait netjack audio connection without blocking the server
void* DSPServer::openAudio(void* arg) 
{
    AudioStarter* starter = (AudioStarter*)arg;
    starter->fServer->openAudio(starter->fDSP);
    delete starter;
    return NULL;
}

void DSPServer::openAudio(netjack_dsp* dsp)
{
    if (fLocker.Lock()) {
        if (dsp->open()) {
            fRunningDsp.push_back(dsp);
        } else {
            delete dsp;
        }
        fLocker.Unlock();
    }
}
 
//---- Creating response page with right header syntaxe
int DSPServer::sendPage(MHD_Connection* connection, const string& page, int status_code, const string& type)
{
    struct MHD_Response *response = MHD_create_response_from_buffer(page.size(), (void*)page.c_str(), MHD_RESPMEM_PERSISTENT);
    if (response) {
        MHD_add_response_header(response, "Content-Type", type.c_str());
        return MHD_queue_response(connection, status_code, response);
    } else {
        return MHD_NO;
    }
}

//-------------------RESPONSE TO CONNEXION

// Checking if every running DSP is really running or if any has stopped
void DSPServer::stopNotActiveDSP()
{
    list<netjack_dsp*>::iterator it = fRunningDsp.begin();
    
    while (it != fRunningDsp.end()) {
        if (!(*it)->isActive()) {
            netjack_dsp* dsp = *it;
            it = fRunningDsp.erase(it); 
            dsp->stop();
            delete dsp;
        } else {
            it++;
        }
    }
}

// Allocation/Initialization of connection struct
connection_info_struct* DSPServer::allocateConnectionStruct(MHD_Connection* connection, const char* method)
{
    connection_info_struct* con_info = new connection_info_struct();
    if (!con_info) {
        return NULL;
    }
     
    if (strcmp(method, "POST") == 0) {
        con_info->fPostprocessor = MHD_create_post_processor(connection, POSTBUFFERSIZE, &iteratePost, (void*)con_info);
        if (!con_info->fPostprocessor) {
            delete con_info;
            return NULL;
        }
        con_info->fConnectiontype = POST;
        con_info->fAnswercode = MHD_HTTP_OK;
    } else {
        con_info->fConnectiontype = GET;
    }

    return con_info;
}

int DSPServer::createConnection(MHD_Connection* connection, const char* method, void** con_cls)
{
    // If connection is new, a connection structure is allocated
    connection_info_struct* con_struct = allocateConnectionStruct(connection, method);
    if (con_struct) {
        *con_cls = (void*)con_struct;
        return MHD_YES;
    } else {
        return MHD_NO;
    }
}

//---- Callback for any type of connection to the server
int DSPServer::answerToConnection(void* cls, 
                                MHD_Connection* connection, 
                                const char* url, 
                                const char* method, 
                                const char* version, 
                                const char* upload_data, 
                                size_t* upload_data_size, 
                                void** con_cls)
{

    DSPServer* server = (DSPServer*)cls;
    server->stopNotActiveDSP();
    
    // If connection is new, a connection structure is allocated
    if (!*con_cls) {
        return server->createConnection(connection, method, con_cls);
    }
    
    // Once connection struct is allocated, the request is treated
    if (strcmp(method, "GET") == 0) {
        return server->answerGet(connection, url);
    } else if (strcmp(method, "POST") == 0) {
        return server->answerPost(connection, url, upload_data, upload_data_size, con_cls);
    } else {
        return server->sendPage(connection, "", MHD_HTTP_BAD_REQUEST, "text/html");
    }
}
    
// For now GET is not a request supported for now
int DSPServer::answerGet(MHD_Connection* connection, const char* url)
{
    printf("answerGet %s\n", url);
    
    if (strcmp(url, "/") == 0) {
        return sendPage(connection, pathToContent("remote-server.html"), MHD_HTTP_OK, "text/html");
    } else if (strcmp(url, "/GetAvailableFactories") == 0) {
        stringstream answer;
        for (map<string, pair<string, llvm_dsp_factory*> >::iterator it = fAvailableFactories.begin(); it != fAvailableFactories.end(); it++) {
            answer << it->first << " " << it->second.first << " ";
        }
        return sendPage(connection, answer.str(), MHD_HTTP_OK, "text/plain");
    } else {
        return MHD_NO;
    }
}

// Response to all POST request
// 3 requests are correct : 
// - /GetJson --> Receive Faust code / Compile Data / Send back jsonInterface
// - /CreateInstance --> Receive factoryIndex / Create instance 
// - /DeleteFactory --> Receive factoryIndex / Delete Factory
int DSPServer::answerPost(MHD_Connection* connection, const char* url, const char* upload_data, size_t* upload_data_size, void** con_cls)
{
    connection_info_struct* con_info = (connection_info_struct*)*con_cls;
    
    if (0 != *upload_data_size) {
        
        MHD_post_process(con_info->fPostprocessor, upload_data, *upload_data_size);
        *upload_data_size = 0;
        return MHD_YES;
        
    } else {
        
        if (strcmp(url, "/GetJson") == 0) {
            if (createFactory(con_info)) {
                return sendPage(connection, con_info->fAnswerstring, MHD_HTTP_OK, "application/json"); 
            } else {
                return sendPage(connection, con_info->fAnswerstring, MHD_HTTP_BAD_REQUEST, "text/html");
            }
        } else if (strcmp(url, "/GetJsonFromKey") == 0) {
            if (getJsonFromKey(con_info)) {
                return sendPage(connection, con_info->fAnswerstring, MHD_HTTP_OK, "application/json"); 
            } else {
                return sendPage(connection, con_info->fAnswerstring, MHD_HTTP_BAD_REQUEST, "text/html");
            }
        } else if (strcmp(url, "/CreateInstance") == 0) {
            if (createInstance(con_info)) {
                return sendPage(connection, "", MHD_HTTP_OK, "text/html");
            } else {
                return sendPage(connection, con_info->fAnswerstring, MHD_HTTP_BAD_REQUEST, "text/html");
            }
        }
//        else if(strcmp(url, "/DeleteFactory") == 0){
//                    
//            llvm_dsp_factory* toDelete = fAvailableFactories[con_info->fSHAKey];
//            
//            if (toDelete) {
//                
//                fAvailableFactories.erase(con_info->fSHAKey);
//                deleteSlaveDSPFactory(toDelete);
//                
//                return send_page(connection, "", 0, MHD_HTTP_OK, "application/html"); 
//            } else {
//                return send_page(connection, "", 0, MHD_HTTP_BAD_REQUEST, "text/html"); 
//            }
//        }
        else if (strcmp(url, "/StartAudio") == 0) {
            startAudio(con_info->fSHAKey);
            return sendPage(connection, "", MHD_HTTP_OK, "text/html");
        } else if(strcmp(url, "/StopAudio") == 0){
            stopAudio(con_info->fSHAKey);
            return sendPage(connection, "", MHD_HTTP_OK, "text/html");
        } else {
            return sendPage(connection, "", MHD_HTTP_BAD_REQUEST, "text/html"); 
        }
    }
}

// Callback processing the received data.
// The datas are stocked in connection_info_struct
int DSPServer::iteratePost(void* coninfo_cls, MHD_ValueKind /*kind*/, 
                            const char* key, 
                            const char* /*filename*/, 
                            const char* /*content_type*/, 
                            const char* /*transfer_encoding*/, 
                            const char* data, 
                            uint64_t /*off*/, 
                            size_t size) {
    
    struct connection_info_struct* con_info = (connection_info_struct*)coninfo_cls;
     
    if (size > 0) {
        
        if (strcmp(key,"name") == 0){
            con_info->fNameApp += nameWithoutSpaces(data);
            if (con_info->fNameApp == "")
                con_info->fNameApp = "RemoteDSPServer_DefaultName";
        }
        
        if (strcmp(key,"dsp_data") == 0)
            con_info->fFaustCode += data;   
              
        if (strcmp(key,"NJ_ip") == 0) 
            con_info->fIP = data;
        
        if (strcmp(key,"NJ_port") == 0) 
            con_info->fPort = data;   
        
        if (strcmp(key,"NJ_latency") == 0)
            con_info->fLatency = data;
        
        if (strcmp(key,"NJ_compression") == 0)
            con_info->fCompression = data;        
        
        if (strcmp(key,"NJ_mtu") == 0) 
            con_info->fMTU = data;
        
        if (strcmp(key,"shaKey") == 0)
            con_info->fSHAKey = data;
        
        if (strcmp(key,"instanceKey") == 0)
            con_info->fInstanceKey = data;
        
        if (strcmp(key,"factoryKey") == 0)
            con_info->fFactoryKey = data;
        
        if (strcmp(key,"options") == 0)
            con_info->fCompilationOptions.push_back(string(data));
        
        if (strcmp(key,"opt_level") == 0)
            con_info->fOptLevel = data;
    }
    
    con_info->fAnswercode = MHD_HTTP_OK;
    return MHD_YES;
}

// Callback when connection is ended
void DSPServer::requestCompleted(void* cls, MHD_Connection* connection, void** con_cls, MHD_RequestTerminationCode toe) 
{
    struct connection_info_struct* con_info = (connection_info_struct*)*con_cls;
    
    if (con_info) {
        if (con_info->fConnectiontype == POST) {
            if (con_info->fPostprocessor) {
                MHD_destroy_post_processor(con_info->fPostprocessor);
            }
        }
        delete con_info;
        *con_cls = NULL;
    }
}

// Start/Stop DSP Instance from its SHAKEY
bool DSPServer::startAudio(const string& shakey)
{
    list<netjack_dsp*>::iterator it;
    
    for (it = fRunningDsp.begin(); it != fRunningDsp.end(); it++) {
        if (shakey == (*it)->getKey()) {
            if ((*it)->start()) {
                return true;
            }
        }
    }

    return false;
}

void DSPServer::stopAudio(const string& shakey)
{
    list<netjack_dsp*>::iterator it;
    
    for (it = fRunningDsp.begin(); it != fRunningDsp.end(); it++) {
        if (shakey == (*it)->getKey()) {
            (*it)->stop();
        }
    }
}

bool DSPServer::getJsonFromKey(connection_info_struct* con_info)
{
    con_info->fNameApp = fAvailableFactories[con_info->fSHAKey].first;
    con_info->fLLVMFactory = fAvailableFactories[con_info->fSHAKey].second;
    
    if (con_info->fLLVMFactory) {
        con_info->fAnswerstring = getJson(con_info);
        return true;
    } else {
        con_info->fAnswerstring = "Factory not found";
        return false;
    }
}

// Create DSP Factory 
bool DSPServer::createFactory(connection_info_struct* con_info) {
    
    // Factory already compiled ?
    if (fAvailableFactories.find(con_info->fSHAKey) != fAvailableFactories.end()) {
        con_info->fLLVMFactory = fAvailableFactories[con_info->fSHAKey].second;
        con_info->fAnswerstring = getJson(con_info);
        return true;
    }
    
    string error = "Incorrect machine code";
    
    // Sort out compilation options
    int argc = con_info->fCompilationOptions.size();
    const char* argv[argc];
    for (int i = 0; i < argc; i++) {
        argv[i] = (con_info->fCompilationOptions[i]).c_str();
    }
    
    if (isopt(argc, argv, "-machine")) {
        // Machine code
        con_info->fLLVMFactory = readDSPFactoryFromMachine(con_info->fFaustCode);
    } else {
        // DSP code
        con_info->fLLVMFactory = createDSPFactoryFromString(con_info->fNameApp, 
                                                            con_info->fFaustCode, 
                                                            argc, argv, "", 
                                                            error, atoi(con_info->fOptLevel.c_str()));
    }
   
    if (con_info->fLLVMFactory) {
        fAvailableFactories[con_info->fSHAKey] = make_pair(con_info->fNameApp, con_info->fLLVMFactory);
        // Once the factory is compiled, the JSON is stored as answerstring
        con_info->fAnswerstring = getJson(con_info);
        return true;
    } else {
        con_info->fAnswerstring = error;
        return false;
    }
}

// Create DSP Instance
bool DSPServer::createInstance(connection_info_struct* con_info)
{
    llvm_dsp_factory* factory = fAvailableFactories[con_info->fFactoryKey].second;
    
    if (factory) {
    
        try {
            netjack_dsp* dsp = new netjack_dsp(factory, con_info->fCompression, con_info->fIP, con_info->fPort, con_info->fMTU, con_info->fLatency);
            dsp->setName(fAvailableFactories[con_info->fFactoryKey].first);
            pthread_t myThread;
            AudioStarter* starter = new AudioStarter(this, dsp);
            
            if (pthread_create(&myThread, NULL, DSPServer::openAudio, starter) == 0){
                dsp->setKey(con_info->fInstanceKey);
                return true;
            } else {
                stringstream s;
                s << ERROR_INSTANCE_NOTCREATED;
                con_info->fAnswerstring = s.str();
                return false;
            }
        
        } catch (...) {
            return false;
        }
        
    } else {
        stringstream s;
        s << ERROR_FACTORY_NOTFOUND;
        con_info->fAnswerstring = s.str();
        return false;
    }    
}

#include "lo/lo.h"

// Register server as available
void* DSPServer::registration(void* arg) 
{
    char host_name[256];
    gethostname(host_name, sizeof(host_name));
    DSPServer* serv = (DSPServer*)arg;
    stringstream nameRegisterService;
    nameRegisterService << "._" << searchIP() << ":" << serv->fPort << "._" << host_name;
    lo_address t = lo_address_new("224.0.0.1", "7770");
    
    while (true) {
    #ifdef WIN32
        Sleep(1);
    #else
        usleep(1000000);
    #endif
        pthread_testcancel();
        lo_send(t, "/faustcompiler", "is", getpid(), nameRegisterService.str().c_str());
    }
    
    pthread_exit(NULL);
}

// DSP server API

EXPORT remote_dsp_server* createRemoteDSPServer(int argc, const char* argv[])
{
     return reinterpret_cast<remote_dsp_server*>(new DSPServer(argc, argv));
}

EXPORT void deleteRemoteDSPServer(remote_dsp_server* server)
{
     delete reinterpret_cast<remote_dsp_server*>(server);
}

EXPORT bool remote_dsp_server::start(int port)
{
    return reinterpret_cast<DSPServer*>(this)->start(port);
}

EXPORT void remote_dsp_server::stop()
{
    return reinterpret_cast<DSPServer*>(this)->stop();
}
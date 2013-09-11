/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

//
// sandesh.cc
//
// Sandesh Implementation
//

#include <boost/bind.hpp>
#include <boost/tokenizer.hpp>
#include <base/logging.h>
#include <base/parse_object.h>
#include <base/queue_task.h>
#include <http/http_session.h>
#include <io/tcp_session.h>

#include <sandesh/transport/TBufferTransports.h>
#include <sandesh/protocol/TBinaryProtocol.h>
#include <sandesh/protocol/TProtocol.h>

#include <sandesh/sandesh_types.h>
#include <sandesh/sandesh.h>
#include <sandesh/sandesh_trace.h>
#include "sandesh_uve.h"
#include "sandesh_session.h"
#include "sandesh_http.h"
#include "sandesh_client.h"
#include "sandesh_connection.h"
#include "sandesh_state_machine.h"

using boost::asio::ip::tcp;
using boost::asio::ip::address;

using namespace contrail::sandesh::protocol;
using namespace contrail::sandesh::transport;

// Statics
Sandesh::SandeshRole Sandesh::role_ = Invalid;
bool Sandesh::enable_local_log_ = false;
uint32_t Sandesh::http_port_ = 0;
bool Sandesh::enable_trace_print_ = false;
SandeshClient *Sandesh::client_ = NULL;
std::auto_ptr<Sandesh::SandeshRxQueue> Sandesh::recv_queue_;
std::string Sandesh::module_;
std::string Sandesh::source_;
int Sandesh::recv_task_id_ = -1;
SandeshContext* Sandesh::client_context_ = NULL;
Sandesh::SandeshCallback Sandesh::response_callback_ = 0;
SandeshLevel::type Sandesh::logging_level_ = SandeshLevel::INVALID;
std::string Sandesh::logging_category_;
EventManager* Sandesh::event_manager_ = NULL;
SandeshGenStatsCollection Sandesh::stats_;
tbb::mutex Sandesh::stats_mutex_;

const char * Sandesh::SandeshRoleToString(SandeshRole role) {
    switch (role) {
    case Generator:
        return "Generator";
    case Collector:
        return "Collector";
    case Test:
        return "Test";
    case Invalid: 
        return "Invalid";
    default:
        return "Unknown";
    }
}

void Sandesh::InitReceive(int recv_task_inst) {
    assert(recv_task_id_ == -1);
    TaskScheduler *scheduler = TaskScheduler::GetInstance();
    recv_task_id_ = scheduler->GetTaskId("sandesh::RecvQueue");
    recv_queue_.reset(new SandeshRxQueue(recv_task_id_, recv_task_inst,
            &Sandesh::ProcessRecv));
}

void Sandesh::InitClient(EventManager *evm, Endpoint server) {
    // Create and initialize the client
    assert(client_ == NULL);
    client_ = new SandeshClient(evm, server);
    client_->Initiate();
}

static int32_t SandeshHttpCallback(SandeshRequest *rsnh) {
    return rsnh->Enqueue(Sandesh::recv_queue());
}

extern int PullSandeshGenStatsReq;
extern int PullSandeshUVE;
extern int PullSandeshTraceReq;

void Sandesh::Initialize(SandeshRole role,
                         const std::string module, 
                         const std::string source,
                         EventManager *evm,
                         unsigned short http_port,
                         SandeshContext *client_context) {
    PullSandeshGenStatsReq = 1;
    PullSandeshUVE = 1;
    PullSandeshTraceReq = 1;

    if (role_ != Invalid || role == Invalid) {
        return;
    }

    LOG(INFO, "SANDESH: ROLE   : " << SandeshRoleToString(role)); 
    LOG(INFO, "SANDESH: MODULE : " << module);
    LOG(INFO, "SANDESH: SOURCE : " << source);
    LOG(INFO, "SANDESH: HTTP SERVER PORT : " << http_port);

    role_           = role;
    module_         = module;
    source_         = source;
    client_context_ = client_context;
    event_manager_  = evm;

    InitReceive(Task::kTaskInstanceAny);
    http_port_ = SandeshHttp::Init(evm, module, http_port, &SandeshHttpCallback);
}

bool Sandesh::ConnectToCollector(const std::string collector_ip,
                                 int collector_port) {
    boost::system::error_code ec;
    address collector_addr = address::from_string(collector_ip, ec);
    if (ec) {
        LOG(ERROR, __func__ << ": Invalid collector address: " <<
                collector_ip << " Error: " << ec);
        return false;
    }

    LOG(INFO, "SANDESH: COLLECTOR : " << collector_ip);
    LOG(INFO, "SANDESH: COLLECTOR PORT : " << collector_port);

    tcp::endpoint collector(collector_addr, collector_port);
    InitClient(event_manager_, collector);
    return true;
}

static bool make_endpoint(TcpServer::Endpoint& ep,const std::string& epstr) {

    typedef boost::tokenizer<boost::char_separator<char> > tokenizer;
    boost::char_separator<char> sep(":");

    tokenizer tokens(epstr, sep);
    tokenizer::iterator it = tokens.begin();
    std::string sip(*it);
    ++it;
    std::string sport(*it);
    int port;
    stringToInteger(sport, port);
    boost::system::error_code ec;
    address addr = address::from_string(sip, ec);
    if (ec) {
        LOG(ERROR, __func__ << ": Invalid collector address: " <<
                sip << " Error: " << ec);
        return false;
    }
    ep = TcpServer::Endpoint(addr, port);
    return true;
}

void Sandesh::InitGenerator(const std::string module,
        const std::string source, EventManager *evm,
        unsigned short http_port,
        SandeshContext *client_context) {
    Initialize(Generator, module, source, evm,
            http_port, client_context);
}

bool Sandesh::InitGenerator(const std::string &module, 
                            const std::string &source,
                            EventManager *evm, 
                            unsigned short http_port,
                            CollectorSubFn csf,
                            const std::vector<std::string> collectors,
                            SandeshContext *client_context) {
    Initialize(Generator, module, source, evm,
               http_port, client_context);

    Endpoint primary = Endpoint();
    Endpoint secondary = Endpoint();

    if (collectors.size()!=0) {
        if (!make_endpoint(primary, collectors[0])) {
            return false;
        }
        if (collectors.size()>1) {
            if (!make_endpoint(secondary, collectors[1])) {
                return false;
            } 
        }
    }
    if (collectors.size()>2) {
        LOG(ERROR, __func__ << " Too many collectors");
        return false;
    }

    client_ = new SandeshClient(evm,
            primary, secondary, csf);
    client_->Initiate();
    return true;
}

void Sandesh::InitCollector(const std::string module, 
                            const std::string source,
                            EventManager *evm, 
                            const std::string collector_ip, int collector_port,
                            unsigned short http_port,
                            SandeshContext *client_context) {
    Initialize(Collector, module, source, evm,
               http_port, client_context);
    ConnectToCollector(collector_ip, collector_port);
}

void Sandesh::InitGeneratorTest(const std::string module, 
                                const std::string source,
                                EventManager *evm,
                                unsigned short http_port, 
                                SandeshContext *client_context) {
    Initialize(Test, module, source, evm, http_port,
               client_context);
}

void Sandesh::Uninit() {

    // Wait until all pending http session based tasks are cleaned up.
    long count = 60000;
    while (count--) {
        if (HttpSession::GetPendingTaskCount() == 0) break;
        usleep(1000);
    }
    SandeshHttp::Uninit();
    role_ = Invalid;
    if (recv_queue_.get() != NULL) {
        recv_queue_->Shutdown();
        recv_queue_.reset(NULL);
        assert(recv_task_id_ != -1);
        recv_task_id_ = -1;
    } else {
        assert(recv_task_id_ == -1);
    }
    if (client_ != NULL) {
        client_->Shutdown();
        while (client()->IsSession()) usleep(100);
        client_->ClearSessions();
        TcpServerManager::DeleteServer(client_);
        client_ = NULL;
    }
}

void Sandesh::SetLoggingParams(bool enable_local_log, std::string category,
        std::string level, bool enable_trace_print) {
    SetLocalLogging(enable_local_log);
    SetLoggingCategory(category);
    SetLoggingLevel(level);
    SetTracePrint(enable_trace_print);
}

void Sandesh::SetLoggingParams(bool enable_local_log, std::string category,
        SandeshLevel::type level, bool enable_trace_print) {
    SetLocalLogging(enable_local_log);
    SetLoggingCategory(category);
    SetLoggingLevel(level);
    SetTracePrint(enable_trace_print);
}

void Sandesh::SetLoggingLevel(std::string level) {
    SandeshLevel::type nlevel = StringToLevel(level);
    SetLoggingLevel(nlevel);
}

void Sandesh::SetLoggingLevel(SandeshLevel::type level) {
    if (logging_level_ != level) {
        LOG(INFO, "SANDESH: Logging: LEVEL: " << "[ " <<
                LevelToString(logging_level_) << " ] -> [ " <<
                LevelToString(level) << " ]");
        logging_level_ = level;
    }
}

void Sandesh::SetLoggingCategory(std::string category) {
    if (logging_category_ != category) {
        LOG(INFO, "SANDESH: Logging: CATEGORY: " <<
                (logging_category_.empty() ? "*" : logging_category_) << " -> " <<
                (category.empty() ? "*" : category));
        logging_category_ = category;
    }
}

void Sandesh::SetLocalLogging(bool enable_local_log) {
    if (enable_local_log_ != enable_local_log) {
        LOG(INFO, "SANDESH: Logging: " <<
                (enable_local_log_ ? "ENABLED" : "DISABLED") << " -> " <<
                (enable_local_log ? "ENABLED" : "DISABLED"));
        enable_local_log_ = enable_local_log;
    }
}

void Sandesh::SetTracePrint(bool enable_trace_print) {
    if (enable_trace_print_ != enable_trace_print) {
        LOG(INFO, "SANDESH: Trace: PRINT: " <<
                (enable_trace_print_ ? "ENABLED" : "DISABLED") << " -> " <<
                (enable_trace_print ? "ENABLED" : "DISABLED"));
        enable_trace_print_ = enable_trace_print;
    }
}

bool Sandesh::Enqueue(SandeshQueue *queue) {
    if (!queue) {
        LOG(ERROR, __func__ << ": SandeshQueue NULL");
        Log();
        Release();
        return false;
    }
    if (!queue->Enqueue(this)) {
        // XXX Change when WorkQueue implements bounded queues
        return true;
    }
    return true;
}

bool Sandesh::ProcessRecv(SandeshRequest *rsnh) {
    rsnh->HandleRequest();
    rsnh->Release();
    return true;
}

void SandeshRequest::Release() { self_.reset(); }

int32_t Sandesh::WriteBinary(u_int8_t *buf, u_int32_t buf_len,
        int *error) {
    int32_t xfer;
    boost::shared_ptr<TMemoryBuffer> btrans =
            boost::shared_ptr<TMemoryBuffer>(
                    new TMemoryBuffer(buf, buf_len));
    btrans->setWriteBuffer(buf, buf_len);
    boost::shared_ptr<TBinaryProtocol> prot =
            boost::shared_ptr<TBinaryProtocol>(new TBinaryProtocol(btrans));
    xfer = Write(prot);
    if (xfer < 0) {
        LOG(DEBUG, __func__ << "Write sandesh to " << buf_len <<
                " bytes FAILED" << std::endl);
        *error = EINVAL;
        return xfer;
    }
    return xfer;
}

int32_t Sandesh::ReadBinary(u_int8_t *buf, u_int32_t buf_len,
        int *error) {
    int32_t xfer = 0;
    boost::shared_ptr<TMemoryBuffer> btrans =
            boost::shared_ptr<TMemoryBuffer>(
                    new TMemoryBuffer(buf, buf_len));
    boost::shared_ptr<TBinaryProtocol> prot =
            boost::shared_ptr<TBinaryProtocol>(new TBinaryProtocol(btrans));
    xfer = Read(prot);
    if (xfer < 0) {
        LOG(DEBUG, __func__ << "Read sandesh from " << buf_len <<
                " bytes FAILED" << std::endl);
        *error = EINVAL;
        return xfer;
    }
    return xfer;
}

int32_t Sandesh::ReceiveBinaryMsgOne(u_int8_t *buf, u_int32_t buf_len,
        int *error, SandeshContext *client_context) {
    int32_t xfer;
    std::string sandesh_name;
    boost::shared_ptr<TMemoryBuffer> btrans =
            boost::shared_ptr<TMemoryBuffer>(
                    new TMemoryBuffer(buf, buf_len));
    boost::shared_ptr<TBinaryProtocol> prot =
            boost::shared_ptr<TBinaryProtocol>(new TBinaryProtocol(btrans));
    // Extract sandesh name
    xfer = prot->readSandeshBegin(sandesh_name);
    if (xfer < 0) {
        LOG(DEBUG, __func__ << "Read sandesh begin from " << buf_len <<
                " bytes FAILED" << std::endl);
        *error = EINVAL;
        return xfer;
    }
    // Create and process the sandesh
    Sandesh *sandesh = SandeshBaseFactory::CreateInstance(sandesh_name);
    if (sandesh == NULL) {
        LOG(DEBUG, __func__ << " Unknown sandesh:" <<
                sandesh_name << std::endl);
        *error = EINVAL;
        return -1;
    }
    // Reinitialize buffer and protocol
    btrans = boost::shared_ptr<TMemoryBuffer>(
                    new TMemoryBuffer(buf, buf_len));
    prot = boost::shared_ptr<TBinaryProtocol>(new TBinaryProtocol(btrans));
    xfer = sandesh->Read(prot);
    if (xfer < 0) {
        LOG(DEBUG, __func__ << " Decoding " << sandesh_name << " FAILED" <<
                std::endl);
        *error = EINVAL;
        return xfer;
    }
    SandeshBuffer * bsnh = dynamic_cast<SandeshBuffer *>(sandesh);
    if (bsnh) bsnh->Process(client_context);
    sandesh->Release();
    return xfer;
}

int32_t Sandesh::ReceiveBinaryMsg(u_int8_t *buf, u_int32_t buf_len,
        int *error, SandeshContext *client_context) {
    u_int32_t xfer = 0;
    int ret;
    while (xfer < buf_len) {
        ret = ReceiveBinaryMsgOne(buf + xfer, buf_len - xfer, error,
                                  client_context);
        if (ret < 0) {
            LOG(DEBUG, __func__ << "Read sandesh from " << buf_len <<
                    " bytes at offset " << xfer << " FAILED (" <<
                    error << ")");
            return ret;
        }
        xfer += ret;
    }
    return xfer;
}

bool Sandesh::HandleTest() {
    // Handle unit test scenario
    if (IsUnitTest() || IsLevelUT()) {
        if (IsLoggingAllowed()) {
            Log();
        }
        Release();
        return true;
    }
    return false;
}

bool Sandesh::SendEnqueue() {
    if (!client_) {
        LOG(INFO, __func__ << " Cannot Send : No client");
        Log();
        Release();
        return false;        
    } 
    if (!client_->SendSandesh(this)) {
        LOG(INFO, __func__ << " Cannot Send : No session");
        Log();
        Release();
        return false;
    }
    return true;
}

bool Sandesh::Dispatch(SandeshConnection * sconn) {
    // Handle unit test
    if (HandleTest()) {
        return true;
    }
    // Sandesh client does not have a connection
    if (sconn) {
        return sconn->SendSandesh(this);
    } else {
        return SendEnqueue();
    }
}

bool SandeshResponse::Dispatch(SandeshConnection * sconn) {
    assert(sconn == NULL);
    if (context().find("http%") == 0) {
        SandeshHttp::Response(this, context());
        return true;
    }
    if (response_callback_) {
        response_callback_(this);
    }
    return Sandesh::Dispatch(sconn);
}

bool SandeshTrace::Dispatch(SandeshConnection * sconn) {
    assert(sconn == NULL);
    if (0 == context().find("http%")) {
        SandeshHttp::Response(this, context());
        return true;
    }
    return Sandesh::Dispatch();
}

bool SandeshUVE::Dispatch(SandeshConnection * sconn) {
    assert(sconn == NULL);
    if (0 == context().find("http%")) {
        SandeshHttp::Response(this, context());
        return true;
    }
    // Handle unit test
    if (HandleTest()) {
        return true;
    }
    if (client_) {
        if (!client_->SendSandeshUVE(this)) {
            LOG(ERROR, "SandeshClient UVE enqueue FAILED");
            Log();
            Release();
            return false;
        }
        return true;
    }
    LOG(ERROR, "Not Ready to Dispatch SandeshUVE");
    Log();
    Release();
    return false;
}

bool SandeshRequest::Enqueue(SandeshRxQueue *queue) {
    if (!queue) {
        LOG(ERROR, __func__ << ": SandeshRxQueue NULL");
        Log();
        Release();
        return false;
    }
    if (!queue->Enqueue(this)) {
        // XXX Change when WorkQueue implements bounded queues
        return true;
    }
    return true;
}

bool Sandesh::IsLevelUT() {
    return level_ >= SandeshLevel::UT_START &&
            level_ <= SandeshLevel::UT_END;
}

bool Sandesh::IsLoggingAllowed() {
    bool level_allowed = logging_level_ >= level_;
    bool category_allowed = !logging_category_.empty() ?
            logging_category_ == category_ : true;
    return level_allowed && category_allowed;
}

const char * Sandesh::LevelToString(SandeshLevel::type level) {
    std::map<int, const char*>::const_iterator it = _SandeshLevel_VALUES_TO_NAMES.find(level);
    if (it != _SandeshLevel_VALUES_TO_NAMES.end()) {
        return it->second;
    } else {
        return "UNKNOWN";
    }
}

SandeshLevel::type Sandesh::StringToLevel(std::string level) {
    std::map<int, const char*>::const_iterator it = _SandeshLevel_VALUES_TO_NAMES.begin();
    while (it != _SandeshLevel_VALUES_TO_NAMES.end()) {
        if (strncmp(level.c_str(), it->second, strlen(it->second)) == 0) {
            return static_cast<SandeshLevel::type>(it->first);
        }
        it++;
    }
    return SandeshLevel::INVALID;
}

void Sandesh::UpdateSandeshStats(const std::string& sandesh_name,
                                 uint32_t bytes, bool is_tx) {
    tbb::mutex::scoped_lock lock(stats_mutex_);

    boost::ptr_map<std::string, SandeshGenStatsElemCollection>& stats_map = 
        stats_.msgtype_stats_map_;
    boost::ptr_map<std::string, SandeshGenStatsElemCollection>::iterator it = 
        stats_map.find(sandesh_name);
    if (it == stats_map.end()) {
        std::string name(sandesh_name);
        it = (stats_map.insert(name, new SandeshGenStatsElemCollection)).first;
    }
    SandeshGenStatsElemCollection *stats_elem = it->second;

    if (is_tx) {
        stats_elem->messages_sent++;
        stats_elem->bytes_sent += bytes;
        stats_.total_sandesh_sent++;
        stats_.total_bytes_sent += bytes;
    } else {
        stats_elem->messages_received++;
        stats_elem->bytes_received += bytes;
        stats_.total_sandesh_received++;
        stats_.total_bytes_received += bytes;
    }
}

/*
 * Project: curve
 * File Created: Tuesday, 9th October 2018 5:17:04 pm
 * Author: tongguangxun
 * Copyright (c) 2018 NetEase
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <brpc/server.h>
#include <fiu-control.h>

#include <string>
#include <thread>   //NOLINT
#include <chrono>   //NOLINT
#include <mutex>    // NOLINT
#include <condition_variable>   //NOLINT

#include "src/client/config_info.h"
#include "test/client/fake/mock_schedule.h"
#include "src/client/io_tracker.h"
#include "src/client/splitor.h"
#include "test/client/fake/mockMDS.h"
#include "src/client/client_common.h"
#include "src/client/file_instance.h"
#include "src/client/metacache.h"
#include "src/client/iomanager4file.h"
#include "src/client/libcurve_file.h"
#include "src/client/client_config.h"
#include "src/client/mds_client.h"
#include "src/client/metacache_struct.h"
#include "test/client/fake/fakeMDS.h"

extern std::string metaserver_addr;
extern uint32_t chunk_size;
extern std::string configpath;

extern char* writebuffer;

using curve::client::UserInfo_t;
using curve::client::CopysetInfo_t;
using curve::client::SegmentInfo;
using curve::client::FInfo_t;
using curve::client::MDSClient;
using curve::client::ClientConfig;
using curve::client::FileInstance;
using curve::client::IOTracker;
using curve::client::MetaCache;
using curve::client::RequestContext;
using curve::client::IOManager4File;
using curve::client::LogicalPoolCopysetIDInfo_t;
using curve::client::FileMetric_t;

bool ioreadflag = false;
std::mutex readmtx;
std::condition_variable readcv;
bool iowriteflag = false;
std::mutex writemtx;
std::condition_variable writecv;

void readcallback(CurveAioContext* context) {
    LOG(INFO) << "aio call back here, errorcode = " << context->ret;
    ioreadflag = true;
    readcv.notify_one();
}

void writecallback(CurveAioContext* context) {
    LOG(INFO) << "aio call back here, errorcode = " << context->ret;
    iowriteflag = true;
    writecv.notify_one();
}

class IOTrackerSplitorTest : public ::testing::Test {
 public:
    void SetUp() {
        fiu_init(0);
        fopt.metaServerOpt.metaaddrvec.push_back("127.0.0.1:9104");
        fopt.metaServerOpt.rpcTimeoutMs = 500;
        fopt.metaServerOpt.rpcRetryTimes = 3;
        fopt.metaServerOpt.retryIntervalUs = 50000;
        fopt.loginfo.loglevel = 0;
        fopt.ioOpt.ioSplitOpt.ioSplitMaxSizeKB = 64;
        fopt.ioOpt.ioSenderOpt.enableAppliedIndexRead = 1;
        fopt.ioOpt.ioSenderOpt.rpcTimeoutMs = 1000;
        fopt.ioOpt.ioSenderOpt.rpcRetryTimes = 3;
        fopt.ioOpt.ioSenderOpt.failRequestOpt.opMaxRetry = 3;
        fopt.ioOpt.ioSenderOpt.failRequestOpt.opRetryIntervalUs = 500;
        fopt.ioOpt.metaCacheOpt.getLeaderRetry = 3;
        fopt.ioOpt.metaCacheOpt.retryIntervalUs = 500;
        fopt.ioOpt.reqSchdulerOpt.queueCapacity = 4096;
        fopt.ioOpt.reqSchdulerOpt.threadpoolSize = 2;
        fopt.ioOpt.reqSchdulerOpt.ioSenderOpt = fopt.ioOpt.ioSenderOpt;
        fopt.leaseOpt.refreshTimesPerLease = 4;

        fileinstance_ = new FileInstance();
        userinfo.owner = "userinfo";
        userinfo.password = "12345";

        mdsclient_.Initialize(fopt.metaServerOpt);
        fileinstance_->Initialize("/test", &mdsclient_, userinfo, fopt);
        InsertMetaCache();
    }

    void TearDown() {
        fileinstance_->UnInitialize();
        mdsclient_.UnInitialize();
        delete fileinstance_;
        LOG(INFO) << "DONE!";
        server.Stop(0);
        server.Join();
    }

    void InsertMetaCache() {
        if (server.AddService(&curvefsservice,
                            brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
            LOG(FATAL) << "Fail to add service";
        }
        if (server.AddService(&topologyservice,
                            brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
            LOG(FATAL) << "Fail to add service";
        }
        brpc::ServerOptions options;
        options.idle_timeout_sec = -1;
        if (server.Start(metaserver_addr.c_str(), &options) != 0) {
            LOG(ERROR) << "Fail to start Server";
        }

        /**
         * 1. set openfile response
         */
        ::curve::mds::OpenFileResponse* openresponse =
        new ::curve::mds::OpenFileResponse();
        ::curve::mds::ProtoSession* se = new ::curve::mds::ProtoSession;
        se->set_sessionid("1");
        se->set_createtime(12345);
        se->set_leasetime(10000000);
        se->set_sessionstatus(::curve::mds::SessionStatus::kSessionOK);

        ::curve::mds::FileInfo* fin = new ::curve::mds::FileInfo;
        fin->set_filename("1_userinfo_.txt");
        fin->set_id(1);
        fin->set_parentid(0);
        fin->set_filetype(curve::mds::FileType::INODE_PAGEFILE);
        fin->set_chunksize(4 * 1024 * 1024);
        fin->set_length(1 * 1024 * 1024 * 1024ul);
        fin->set_ctime(12345678);
        fin->set_seqnum(0);
        fin->set_segmentsize(1 * 1024 * 1024 * 1024ul);

        openresponse->set_statuscode(::curve::mds::StatusCode::kOK);
        openresponse->set_allocated_protosession(se);
        openresponse->set_allocated_fileinfo(fin);
        FakeReturn* openfakeret = new FakeReturn(nullptr, static_cast<void*>(openresponse));      // NOLINT
        curvefsservice.SetOpenFile(openfakeret);
        // open will set the finfo for file instance
        fileinstance_->Open("1_userinfo_.txt", userinfo);

        /**
         * 2. 设置GetOrAllocateSegmentresponse
         */
        curve::mds::GetOrAllocateSegmentResponse* response =
            new curve::mds::GetOrAllocateSegmentResponse();
        curve::mds::PageFileSegment* pfs = new curve::mds::PageFileSegment;

        response->set_statuscode(::curve::mds::StatusCode::kOK);
        response->set_allocated_pagefilesegment(pfs);
        response->mutable_pagefilesegment()->
            set_logicalpoolid(1234);
        response->mutable_pagefilesegment()->
            set_segmentsize(1 * 1024 * 1024 * 1024);
        response->mutable_pagefilesegment()->
            set_chunksize(4 * 1024 * 1024);
        response->mutable_pagefilesegment()->
            set_startoffset(0);

        for (int i = 0; i < 256; i ++) {
            auto chunk = response->mutable_pagefilesegment()->add_chunks();
            chunk->set_copysetid(i);
            chunk->set_chunkid(i);
        }
        FakeReturn* fakeret = new FakeReturn(nullptr,
                    static_cast<void*>(response));
        curvefsservice.SetGetOrAllocateSegmentFakeReturn(fakeret);

        // 3. set refresh response
        curve::mds::FileInfo * info = new curve::mds::FileInfo;
        info->set_filename("1_userinfo_.txt");
        info->set_seqnum(2);
        info->set_id(1);
        info->set_parentid(0);
        info->set_filetype(curve::mds::FileType::INODE_PAGEFILE);
        info->set_chunksize(4 * 1024 * 1024);
        info->set_length(4 * 1024 * 1024 * 1024ul);
        info->set_ctime(12345678);

        ::curve::mds::ReFreshSessionResponse* refreshresp =
            new ::curve::mds::ReFreshSessionResponse;
        refreshresp->set_statuscode(::curve::mds::StatusCode::kOK);
        refreshresp->set_sessionid("1234");
        refreshresp->set_allocated_fileinfo(info);
        FakeReturn* refreshfakeret
        = new FakeReturn(nullptr, static_cast<void*>(refreshresp));
        curvefsservice.SetRefreshSession(refreshfakeret, nullptr);

        /**
         * 4. 设置topology返回值
         */
        ::curve::mds::topology::GetChunkServerListInCopySetsResponse* response_1
        = new ::curve::mds::topology::GetChunkServerListInCopySetsResponse;
        response_1->set_statuscode(0);
        uint64_t chunkserveridc = 1;
        for (int i = 0; i < 256; i ++) {
            auto csinfo = response_1->add_csinfo();
            csinfo->set_copysetid(i);

            for (int j = 0; j < 3; j++) {
                auto cslocs = csinfo->add_cslocs();
                cslocs->set_chunkserverid(chunkserveridc++);
                cslocs->set_hostip("127.0.0.1");
                cslocs->set_port(9104);
            }
        }
        FakeReturn* faktopologyeret = new FakeReturn(nullptr,
                    static_cast<void*>(response_1));
        topologyservice.SetFakeReturn(faktopologyeret);

        curve::client::MetaCache* mc = fileinstance_->GetIOManager4File()->
                                                            GetMetaCache();
        curve::client::FInfo_t fi;
        fi.userinfo = userinfo;
        fi.chunksize   = 4 * 1024 * 1024;
        fi.segmentsize = 1 * 1024 * 1024 * 1024ul;
        SegmentInfo sinfo;
        LogicalPoolCopysetIDInfo_t lpcsIDInfo;
        mdsclient_.GetOrAllocateSegment(true, 0, &fi, &sinfo);
        int count = 0;
        for (auto iter : sinfo.chunkvec) {
            uint64_t index = (sinfo.startoffset + count*fi.chunksize )
                            / fi.chunksize;
            mc->UpdateChunkInfoByIndex(index, iter);
            ++count;
        }

        std::vector<CopysetInfo_t> cpinfoVec;
        mdsclient_.GetServerList(lpcsIDInfo.lpid,
                                 lpcsIDInfo.cpidVec, &cpinfoVec);

        for (auto iter : cpinfoVec) {
            mc->UpdateCopysetInfo(lpcsIDInfo.lpid, iter.cpid_, iter);
        }
    }

    UserInfo_t userinfo;
    MDSClient mdsclient_;
    FileServiceOption_t fopt;
    curve::client::ClientConfig cc;
    FileInstance*    fileinstance_;
    brpc::Server server;
    FakeMDSCurveFSService curvefsservice;
    FakeTopologyService topologyservice;
};

TEST_F(IOTrackerSplitorTest, AsyncStartRead) {
    MockRequestScheduler* mockschuler = new MockRequestScheduler;
    mockschuler->DelegateToFake();

    curve::client::IOManager4File* iomana = fileinstance_->GetIOManager4File();
    MetaCache* mc = fileinstance_->GetIOManager4File()->GetMetaCache();
    iomana->SetRequestScheduler(mockschuler);

    CurveAioContext aioctx;
    aioctx.offset = 4 * 1024 * 1024 - 4 * 1024;
    aioctx.length = 4 * 1024 * 1024 + 8 * 1024;
    aioctx.ret = LIBCURVE_ERROR::OK;
    aioctx.cb = readcallback;
    aioctx.buf = new char[aioctx.length];
    aioctx.op = LIBCURVE_OP::LIBCURVE_OP_READ;

    ioreadflag = false;
    char* data = static_cast<char*>(aioctx.buf);
    iomana->AioRead(&aioctx, &mdsclient_);

    {
        std::unique_lock<std::mutex> lk(readmtx);
        readcv.wait(lk, []()->bool{return ioreadflag;});
    }
    LOG(ERROR) << "address = " << &data;
    ASSERT_EQ('a', data[0]);
    ASSERT_EQ('a', data[4 * 1024 - 1]);
    ASSERT_EQ('b', data[4 * 1024]);
    ASSERT_EQ('e', data[4 * 1024 + chunk_size - 1]);
    ASSERT_EQ('f', data[4 * 1024 + chunk_size]);
    ASSERT_EQ('f', data[aioctx.length - 1]);

    delete[] data;
}

TEST_F(IOTrackerSplitorTest, AsyncStartWrite) {
    MockRequestScheduler* mockschuler = new MockRequestScheduler;
    mockschuler->DelegateToFake();

    curve::client::IOManager4File* iomana = fileinstance_->GetIOManager4File();
    MetaCache* mc = fileinstance_->GetIOManager4File()->GetMetaCache();
    iomana->SetRequestScheduler(mockschuler);

    CurveAioContext aioctx;
    aioctx.offset = 4 * 1024 * 1024 - 4 * 1024;
    aioctx.length = 4 * 1024 * 1024 + 8 * 1024;
    aioctx.ret = LIBCURVE_ERROR::OK;
    aioctx.cb = writecallback;
    aioctx.buf = new char[aioctx.length];
    aioctx.op = LIBCURVE_OP::LIBCURVE_OP_WRITE;

    char* data = static_cast<char*>(aioctx.buf);

    memset(data, 'a', 4 * 1024);
    memset(data + 4 * 1024, 'b', chunk_size);
    memset(data + 4 * 1024 + chunk_size, 'c', 4 * 1024);
    FInfo_t fi;
    fi.chunksize = 4 * 1024 * 1024;
    fi.segmentsize = 1 * 1024 * 1024 * 1024ul;
    iowriteflag = false;
    iomana->AioWrite(&aioctx, &mdsclient_);

    {
        std::unique_lock<std::mutex> lk(writemtx);
        writecv.wait(lk, []()->bool{return iowriteflag;});
    }

    ASSERT_EQ('a', writebuffer[0]);
    ASSERT_EQ('a', writebuffer[4 * 1024 - 1]);
    ASSERT_EQ('b', writebuffer[4 * 1024]);
    ASSERT_EQ('b', writebuffer[4 * 1024 + chunk_size - 1]);
    ASSERT_EQ('c', writebuffer[4 * 1024 + chunk_size]);
    ASSERT_EQ('c', writebuffer[aioctx.length - 1]);

    delete[] data;
}

TEST_F(IOTrackerSplitorTest, StartRead) {
    MockRequestScheduler* mockschuler = new MockRequestScheduler;
    mockschuler->DelegateToFake();

    curve::client::IOManager4File* iomana = fileinstance_->GetIOManager4File();
    MetaCache* mc = fileinstance_->GetIOManager4File()->GetMetaCache();
    iomana->SetRequestScheduler(mockschuler);

    uint64_t offset = 4 * 1024 * 1024 - 4 * 1024;
    uint64_t length = 4 * 1024 * 1024 + 8 * 1024;
    char* data = new char[length];

    auto threadfunc = [&]() {
        iomana->Read(data, offset, length, &mdsclient_);
    };
    std::thread process(threadfunc);

    if (process.joinable()) {
        process.join();
    }

    ASSERT_EQ('a', data[0]);
    ASSERT_EQ('a', data[4 * 1024 - 1]);
    ASSERT_EQ('b', data[4 * 1024]);
    ASSERT_EQ('e', data[4 * 1024 + chunk_size - 1]);
    ASSERT_EQ('f', data[4 * 1024 + chunk_size]);
    ASSERT_EQ('f', data[length - 1]);

    delete[] data;
}

TEST_F(IOTrackerSplitorTest, StartWrite) {
    MockRequestScheduler* mockschuler = new MockRequestScheduler;
    mockschuler->DelegateToFake();

    curve::client::IOManager4File* iomana = fileinstance_->GetIOManager4File();
    MetaCache* mc = fileinstance_->GetIOManager4File()->GetMetaCache();
    iomana->SetRequestScheduler(mockschuler);

    uint64_t offset = 4 * 1024 * 1024 - 4 * 1024;
    uint64_t length = 4 * 1024 * 1024 + 8 * 1024;
    char* buf = new char[length];

    memset(buf, 'a', 4 * 1024);
    memset(buf + 4 * 1024, 'b', chunk_size);
    memset(buf + 4 * 1024 + chunk_size, 'c', 4 * 1024);

    auto threadfunc = [&]() {
        iomana->Write(buf, offset, length, &mdsclient_);
    };
    std::thread process(threadfunc);

    if (process.joinable()) {
        process.join();
    }

    ASSERT_EQ('a', writebuffer[0]);
    ASSERT_EQ('a', writebuffer[4 * 1024 - 1]);
    ASSERT_EQ('b', writebuffer[4 * 1024]);
    ASSERT_EQ('b', writebuffer[4 * 1024 + chunk_size - 1]);
    ASSERT_EQ('c', writebuffer[4 * 1024 + chunk_size]);
    ASSERT_EQ('c', writebuffer[length - 1]);

    delete[] buf;
}

TEST_F(IOTrackerSplitorTest, ManagerAsyncStartRead) {
    MockRequestScheduler* mockschuler = new MockRequestScheduler;
    mockschuler->DelegateToFake();

    auto ioctxmana = fileinstance_->GetIOManager4File();
    ioctxmana->SetRequestScheduler(mockschuler);
    CurveAioContext* aioctx = new CurveAioContext;
    aioctx->offset = 4 * 1024 * 1024 - 4 * 1024;
    aioctx->length = 4 * 1024 * 1024 + 8 * 1024;
    aioctx->ret = LIBCURVE_ERROR::OK;
    aioctx->cb = readcallback;
    aioctx->buf = new char[aioctx->length];
    aioctx->op = LIBCURVE_OP::LIBCURVE_OP_READ;

    ioreadflag = false;
    char* data = static_cast<char*>(aioctx->buf);
    ioctxmana->AioRead(aioctx, &mdsclient_);

    {
        std::unique_lock<std::mutex> lk(readmtx);
        readcv.wait(lk, []()->bool{return ioreadflag;});
    }
    ASSERT_EQ('a', data[0]);
    ASSERT_EQ('a', data[4 * 1024 - 1]);
    ASSERT_EQ('b', data[4 * 1024]);
    ASSERT_EQ('e', data[4 * 1024 + chunk_size - 1]);
    ASSERT_EQ('f', data[4 * 1024 + chunk_size]);
    ASSERT_EQ('f', data[aioctx->length - 1]);
}

TEST_F(IOTrackerSplitorTest, ManagerAsyncStartWrite) {
    MockRequestScheduler* mockschuler = new MockRequestScheduler;
    mockschuler->DelegateToFake();

    MetaCache* mc = fileinstance_->GetIOManager4File()->GetMetaCache();
    auto ioctxmana = fileinstance_->GetIOManager4File();
    ioctxmana->SetRequestScheduler(mockschuler);

    CurveAioContext* aioctx = new CurveAioContext;
    aioctx->offset = 4 * 1024 * 1024 - 4 * 1024;
    aioctx->length = 4 * 1024 * 1024 + 8 * 1024;
    aioctx->ret = LIBCURVE_ERROR::OK;
    aioctx->cb = writecallback;
    aioctx->buf = new char[aioctx->length];
    aioctx->op = LIBCURVE_OP::LIBCURVE_OP_WRITE;

    char* data = static_cast<char*>(aioctx->buf);

    memset(data, 'a', 4 * 1024);
    memset(data + 4 * 1024, 'b', chunk_size);
    memset(data + 4 * 1024 + chunk_size, 'c', 4 * 1024);

    iowriteflag = false;
    ioctxmana->AioWrite(aioctx, &mdsclient_);

    {
        std::unique_lock<std::mutex> lk(writemtx);
        writecv.wait(lk, []()->bool{return iowriteflag;});
    }

    ASSERT_EQ('a', writebuffer[0]);
    ASSERT_EQ('a', writebuffer[4 * 1024 - 1]);
    ASSERT_EQ('b', writebuffer[4 * 1024]);
    ASSERT_EQ('b', writebuffer[4 * 1024 + chunk_size - 1]);
    ASSERT_EQ('c', writebuffer[4 * 1024 + chunk_size]);
    ASSERT_EQ('c', writebuffer[aioctx->length - 1]);
}


TEST_F(IOTrackerSplitorTest, ManagerAsyncStartWriteReadGetSegmentFail) {
    MockRequestScheduler* mockschuler = new MockRequestScheduler;
    mockschuler->DelegateToFake();

    curve::mds::GetOrAllocateSegmentResponse* response =
                new curve::mds::GetOrAllocateSegmentResponse();
    brpc::Controller* controller1 = new brpc::Controller;
    controller1->SetFailed(-1, "error");
    FakeReturn* fakeret = new FakeReturn(controller1,
                static_cast<void*>(response));
    curvefsservice.SetGetOrAllocateSegmentFakeReturn(fakeret);

    MetaCache* mc = fileinstance_->GetIOManager4File()->GetMetaCache();
    auto ioctxmana = fileinstance_->GetIOManager4File();
    ioctxmana->SetRequestScheduler(mockschuler);
    ioctxmana->SetIOOpt(fopt.ioOpt);

    CurveAioContext* aioctx = new CurveAioContext;
    aioctx->offset = 10*1024*1024*1024ul;
    aioctx->length = chunk_size + 8 * 1024;
    aioctx->ret = LIBCURVE_ERROR::OK;
    aioctx->cb = writecallback;
    aioctx->buf = new char[aioctx->length];
    aioctx->op = LIBCURVE_OP::LIBCURVE_OP_WRITE;

    char* data = static_cast<char*>(aioctx->buf);

    memset(data, 'a', 4 * 1024);
    memset(data + 4 * 1024, 'b', chunk_size);
    memset(data + 4 * 1024 + chunk_size, 'c', 4 * 1024);

    // 设置mds一侧get segment接口返回失败，底层task thread层会一直重试，
    // 但是不会阻塞上层继续向下发送IO请求
    int reqcount = 32;
    auto threadFunc1 = [&]() {
        while (reqcount > 0) {
            fileinstance_->AioWrite(aioctx);
            reqcount--;
        }
    };

    std::thread t1(threadFunc1);
    std::thread t2(threadFunc1);
    t1.join();
    t2.join();
}

TEST_F(IOTrackerSplitorTest, ManagerAsyncStartWriteReadGetServerlistFail) {
    MockRequestScheduler* mockschuler = new MockRequestScheduler;
    mockschuler->DelegateToFake();

    ::curve::mds::topology::GetChunkServerListInCopySetsResponse* response
        = new ::curve::mds::topology::GetChunkServerListInCopySetsResponse;
    brpc::Controller* controller1 = new brpc::Controller;
    controller1->SetFailed(-1, "error");
    FakeReturn* fakeret = new FakeReturn(controller1,
                static_cast<void*>(response));
    topologyservice.SetFakeReturn(fakeret);

    MetaCache* mc = fileinstance_->GetIOManager4File()->GetMetaCache();
    auto ioctxmana = fileinstance_->GetIOManager4File();
    ioctxmana->SetRequestScheduler(mockschuler);
    ioctxmana->SetIOOpt(fopt.ioOpt);

    // offset 10*1024*1024*1024ul 不在metacache里
    // client回去mds拿segment和serverlist
    CurveAioContext* aioctx = new CurveAioContext;
    aioctx->offset = 10*1024*1024*1024ul;
    aioctx->length = chunk_size + 8 * 1024;
    aioctx->ret = LIBCURVE_ERROR::OK;
    aioctx->cb = writecallback;
    aioctx->buf = new char[aioctx->length];
    aioctx->op = LIBCURVE_OP::LIBCURVE_OP_WRITE;

    char* data = static_cast<char*>(aioctx->buf);

    memset(data, 'a', 4 * 1024);
    memset(data + 4 * 1024, 'b', chunk_size);
    memset(data + 4 * 1024 + chunk_size, 'c', 4 * 1024);

    // 设置mds一侧get server list接口返回失败，底层task thread层会一直重试
    // 但是不会阻塞，上层继续向下发送IO请求
    int reqcount = 32;
    auto threadFunc1 = [&]() {
        while (reqcount > 0) {
            fileinstance_->AioWrite(aioctx);
            reqcount--;
        }
    };

    std::thread t1(threadFunc1);
    std::thread t2(threadFunc1);
    t1.join();
    t2.join();
}

TEST_F(IOTrackerSplitorTest, ManagerStartRead) {
    MockRequestScheduler* mockschuler = new MockRequestScheduler;
    mockschuler->DelegateToFake();

    MetaCache* mc = fileinstance_->GetIOManager4File()->GetMetaCache();
    auto ioctxmana = fileinstance_->GetIOManager4File();
    ioctxmana->SetRequestScheduler(mockschuler);

    uint64_t offset = 4 * 1024 * 1024 - 4 * 1024;
    uint64_t length = 4 * 1024 * 1024 + 8 * 1024;
    char* data = new char[length];

    auto threadfunc = [&]() {
        ASSERT_EQ(length, ioctxmana->Read(data,
                                        offset,
                                        length,
                                        &mdsclient_));
    };
    std::thread process(threadfunc);

    if (process.joinable()) {
        process.join();
    }

    ASSERT_EQ('a', data[0]);
    ASSERT_EQ('a', data[4 * 1024 - 1]);
    ASSERT_EQ('b', data[4 * 1024]);
    ASSERT_EQ('e', data[4 * 1024 + chunk_size - 1]);
    ASSERT_EQ('f', data[4 * 1024 + chunk_size]);
    ASSERT_EQ('f', data[length - 1]);
    delete[] data;
}

TEST_F(IOTrackerSplitorTest, ManagerStartWrite) {
    MockRequestScheduler* mockschuler = new MockRequestScheduler;
    mockschuler->DelegateToFake();

    MetaCache* mc = fileinstance_->GetIOManager4File()->GetMetaCache();
    auto ioctxmana = fileinstance_->GetIOManager4File();
    ioctxmana->SetRequestScheduler(mockschuler);

    uint64_t offset = 4 * 1024 * 1024 - 4 * 1024;
    uint64_t length = 4 * 1024 * 1024 + 8 * 1024;
    char* buf = new char[length];

    memset(buf, 'a', 4 * 1024);
    memset(buf + 4 * 1024, 'b', chunk_size);
    memset(buf + 4 * 1024 + chunk_size, 'c', 4 * 1024);

    auto threadfunc = [&]() {
        ioctxmana->Write(buf,
                        offset,
                        length,
                        &mdsclient_);
    };
    std::thread process(threadfunc);

    if (process.joinable()) {
        process.join();
    }

    ASSERT_EQ('a', writebuffer[0]);
    ASSERT_EQ('a', writebuffer[4 * 1024 - 1]);
    ASSERT_EQ('b', writebuffer[4 * 1024]);
    ASSERT_EQ('b', writebuffer[4 * 1024 + chunk_size - 1]);
    ASSERT_EQ('c', writebuffer[4 * 1024 + chunk_size]);
    ASSERT_EQ('c', writebuffer[length - 1]);

    delete[] buf;
}

TEST_F(IOTrackerSplitorTest, ExceptionTest_TEST) {
    MockRequestScheduler* mockschuler = new MockRequestScheduler;
    mockschuler->DelegateToFake();
    FInfo_t fi;
    fi.chunksize = 4 * 1024 * 1024;
    fi.segmentsize = 1 * 1024 * 1024 * 1024ul;
    auto fileserv = new FileInstance();

    UserInfo_t rootuserinfo;
    rootuserinfo.owner = "root";
    rootuserinfo.password = "root_password";

    ASSERT_TRUE(fileserv->Initialize("/test", &mdsclient_, rootuserinfo, fopt));
    ASSERT_EQ(LIBCURVE_ERROR::OK, fileserv->Open("1_userinfo_.txt", userinfo));
    curve::client::IOManager4File* iomana = fileserv->GetIOManager4File();
    MetaCache* mc = fileserv->GetIOManager4File()->GetMetaCache();

    IOTracker* iotracker = new IOTracker(iomana, mc, mockschuler);

    ASSERT_NE(nullptr, iotracker);
    uint64_t offset = 4 * 1024 * 1024 - 4 * 1024;
    uint64_t length = 4 * 1024 * 1024 + 8 * 1024;
    char* buf = new char[length];

    auto waitfunc = [&]() {
        int retlen = iotracker->Wait();
        ASSERT_EQ(-1 * LIBCURVE_ERROR::FAILED, retlen);
    };

    auto threadfunc = [&]() {
        iotracker->StartWrite(nullptr,
                            nullptr,
                            offset,
                            length,
                            &mdsclient_,
                            &fi);
    };
    std::thread process(threadfunc);
    std::thread waitthread(waitfunc);

    uint64_t off = 4 * 1024 * 1024 * 1024ul - 4 * 1024;
    uint64_t len = 4 * 1024 * 1024 + 8 * 1024;
    iomana->Write(buf, off, len, &mdsclient_);

    if (process.joinable()) {
        process.join();
    }
    if (waitthread.joinable()) {
        waitthread.join();
    }
    fileserv->UnInitialize();
    delete fileserv;
    delete mockschuler;
}

TEST_F(IOTrackerSplitorTest, BoundaryTEST) {
    MockRequestScheduler* mockschuler = new MockRequestScheduler;
    mockschuler->DelegateToFake();

    MetaCache* mc = fileinstance_->GetIOManager4File()->GetMetaCache();
    auto ioctxmana = fileinstance_->GetIOManager4File();
    ioctxmana->SetRequestScheduler(mockschuler);

    // this offset and length will make splitor split fail.
    // we set disk size = 1G.
    uint64_t offset = 1 * 1024 * 1024 * 1024 -
                     4 * 1024 * 1024 - 4 *1024;
    uint64_t length = 4 * 1024 * 1024 + 8 * 1024;
    char* buf = new char[length];

    memset(buf, 'a', 4 * 1024);
    memset(buf + 4 * 1024, 'b', chunk_size);
    memset(buf + 4 * 1024 + chunk_size, 'c', 4 * 1024);

    auto threadfunc = [&]() {
        ASSERT_EQ(-1 * LIBCURVE_ERROR::FAILED, ioctxmana->Write(buf,
                                                                offset,
                                                                length,
                                                                &mdsclient_));
    };
    std::thread process(threadfunc);

    if (process.joinable()) {
        process.join();
    }

    delete[] buf;
}

TEST_F(IOTrackerSplitorTest, largeIOTest) {
    MockRequestScheduler mockschuler;
    mockschuler.DelegateToFake();
    /**
     * this offset and length will make splitor split into two 8k IO.
     */
    uint64_t length = 2 * 64 * 1024;
    uint64_t offset = 4 * 1024 * 1024 - length;
    char* buf = new char[length];

    memset(buf, 'a', 64 * 1024);
    memset(buf + 64 * 1024, 'b', 64 * 1024);
    FInfo_t fi;
    fi.seqnum = 0;
    fi.chunksize = 4 * 1024 * 1024;
    fi.segmentsize = 1 * 1024 * 1024 * 1024ul;
    curve::client::IOManager4File* iomana = fileinstance_->GetIOManager4File();
    MetaCache* mc = fileinstance_->GetIOManager4File()->GetMetaCache();

    IOTracker* iotracker = new IOTracker(iomana, mc, &mockschuler);

    curve::client::ChunkIDInfo chinfo(1, 2, 3);
    mc->UpdateChunkInfoByIndex(0, chinfo);

    std::list<RequestContext*> reqlist;
    ASSERT_EQ(0, curve::client::Splitor::IO2ChunkRequests(iotracker, mc,
                                                            &reqlist,
                                                            buf,
                                                            offset,
                                                            length,
                                                            &mdsclient_,
                                                            &fi));
    ASSERT_EQ(2, reqlist.size());

    RequestContext* first = reqlist.front();
    reqlist.pop_front();
    RequestContext* second = reqlist.front();
    reqlist.pop_front();

    for (int i = 0; i < 64 * 1024; i++) {
        ASSERT_EQ(97, (char)(*(first->readBuffer_ + i)));
        ASSERT_EQ(98, (char)(*(second->readBuffer_ + i)));
    }

    ASSERT_EQ(1, first->idinfo_.cid_);
    ASSERT_EQ(3, first->idinfo_.cpid_);
    ASSERT_EQ(2, first->idinfo_.lpid_);
    ASSERT_EQ(4 * 1024 * 1024 - length, first->offset_);
    ASSERT_EQ(64 * 1024, first->rawlength_);
    ASSERT_EQ(0, first->seq_);
    ASSERT_EQ(0, first->appliedindex_);

    ASSERT_EQ(1, second->idinfo_.cid_);
    ASSERT_EQ(3, second->idinfo_.cpid_);
    ASSERT_EQ(2, second->idinfo_.lpid_);
    ASSERT_EQ(4 * 1024 * 1024 - 64 * 1024, second->offset_);
    ASSERT_EQ(64 * 1024, second->rawlength_);
    ASSERT_EQ(0, second->seq_);
    ASSERT_EQ(0, second->appliedindex_);
    delete[] buf;
}

TEST_F(IOTrackerSplitorTest, InvalidParam) {
    uint64_t length = 2 * 64 * 1024;
    uint64_t offset = 4 * 1024 * 1024 - length;
    char* buf = new char[length];
    MetaCache* mc = fileinstance_->GetIOManager4File()->GetMetaCache();
    std::list<RequestContext*> reqlist;
    FInfo_t fi;

    IOTracker* iotracker = new IOTracker(nullptr, nullptr, nullptr);
    curve::client::ChunkIDInfo cid(0, 0, 0);

    ASSERT_EQ(-1, curve::client::Splitor::IO2ChunkRequests(nullptr, mc,
                                        &reqlist,
                                        buf,
                                        offset,
                                        length,
                                        &mdsclient_,
                                        &fi));
    ASSERT_EQ(-1, curve::client::Splitor::SingleChunkIO2ChunkRequests(nullptr, mc,      // NOLINT
                                        &reqlist,
                                        cid,
                                        buf,
                                        offset,
                                        length,
                                        0));
    ASSERT_EQ(-1, curve::client::Splitor::IO2ChunkRequests(iotracker, nullptr,
                                        &reqlist,
                                        buf,
                                        offset,
                                        length,
                                        &mdsclient_,
                                        nullptr));
    ASSERT_EQ(-1, curve::client::Splitor::SingleChunkIO2ChunkRequests(iotracker, nullptr,   // NOLINT
                                        &reqlist,
                                        cid,
                                        buf,
                                        offset,
                                        length,
                                        0));
    ASSERT_EQ(-1, curve::client::Splitor::IO2ChunkRequests(iotracker, mc,
                                        &reqlist,
                                        buf,
                                        offset,
                                        length,
                                        &mdsclient_,
                                        nullptr));
    ASSERT_EQ(0, curve::client::Splitor::SingleChunkIO2ChunkRequests(iotracker, mc,        // NOLINT
                                        &reqlist,
                                        cid,
                                        buf,
                                        offset,
                                        length,
                                        0));
    ASSERT_EQ(-1, curve::client::Splitor::IO2ChunkRequests(iotracker, mc,
                                        nullptr,
                                        buf,
                                        offset,
                                        length,
                                        &mdsclient_,
                                        nullptr));
    ASSERT_EQ(-1, curve::client::Splitor::SingleChunkIO2ChunkRequests(iotracker, mc,        // NOLINT
                                        nullptr,
                                        cid,
                                        buf,
                                        offset,
                                        length,
                                        0));
    ASSERT_EQ(-1, curve::client::Splitor::IO2ChunkRequests(iotracker, mc,
                                        &reqlist,
                                        nullptr,
                                        offset,
                                        length,
                                        &mdsclient_,
                                        nullptr));
    ASSERT_EQ(-1, curve::client::Splitor::SingleChunkIO2ChunkRequests(iotracker, mc,        // NOLINT
                                        &reqlist,
                                        cid,
                                        nullptr,
                                        offset,
                                        length,
                                        0));

    delete iotracker;
    delete[] buf;
}

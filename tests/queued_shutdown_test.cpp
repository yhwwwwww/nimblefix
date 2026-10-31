#include <nimblefix/engine.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <thread>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nf=nimblefix;
using namespace std::chrono_literals;
namespace {
std::string wire(bool logon) {
    std::array<char,1024> bytes{};
    std::array<char,32> stamp{};
    const auto now=std::time(nullptr);
    std::tm utc{};::gmtime_r(&now,&utc);
    ::strftime(stamp.data(),stamp.size(),"%Y%m%d-%H:%M:%S",&utc);
    nf::SessionHeader header{"BUY","SELL",logon?1U:2U,stamp.data()};
    auto encoded=logon ? nf::encode_fix(nf::message<"A">().set<98>(0).set<108>(30),header,bytes)
        : nf::encode_fix(nf::message<"D">().set<11>("PENDING"),header,bytes);
    return encoded.ok()?std::string{bytes.data(),encoded.value()}:std::string{};
}
bool send_all(int socket,std::string_view bytes) {
    while(!bytes.empty()) {
        const auto sent=::send(socket,bytes.data(),bytes.size(),MSG_NOSIGNAL);
        if(sent<0&&errno==EINTR)continue;
        if(sent<=0)return false;
        bytes.remove_prefix(sent);
    }
    return true;
}
}
int main() {
    char pattern[]="/tmp/nimblefix-queued-shutdown-XXXXXX";
    const auto* directory=::mkdtemp(pattern);
    if(!directory)return 1;
    int failure=0;
    {
        nf::EngineConfig config;
        config.storage.directory=directory;
        config.logout_timeout_ms=100;
        config.max_pending_messages=12;
        config.max_pending_bytes=512U*1024U;
        config.write_stall_ms=5000;
        config.queued.worker_threads=1;
        auto opened=nf::Engine<nf::FlatSchema,nf::Queued>::create(config);
        if(!opened.ok())return 2;
        auto engine=std::move(opened).value();
        const std::string payload(50U*1024U,'X');
        std::atomic<bool> callback_returning{false};
        std::atomic<unsigned> accepted{0};
        if(!engine.on_message([&](nf::SessionContext& context,const nf::FlatMessageView&) {
            for(unsigned i=0;i<64;++i) {
                auto response=nf::message<"8">(nf::MessageLimits{8,52U*1024U}).set<11>(i).set<58>(payload);
                const auto sent=context.send(response);
                if(!sent.ok())return sent.status();
                ++accepted;
            }
            callback_returning.store(true,std::memory_order_release);
            return nf::Status::success();
        }).ok() || !engine.add_listener({.name="main",.endpoint={"127.0.0.1",0}}).ok() ||
           !engine.add_acceptor({.name="peer",.listener="main",.local_comp_id="SELL",.remote_comp_id="BUY"}).ok())return 3;
        nf::Status result;
        std::thread owner([&]{result=engine.run();});
        nf::Result<nf::TcpEndpoint> endpoint{nf::Status{nf::ErrorCode::need_more}};
        const auto deadline=std::chrono::steady_clock::now()+5s;
        while(!endpoint.ok()&&std::chrono::steady_clock::now()<deadline) {
            endpoint=engine.listener_endpoint("main");std::this_thread::sleep_for(1ms);
        }
        const int peer=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
        int receive_bytes=1024;
        ::setsockopt(peer,SOL_SOCKET,SO_RCVBUF,&receive_bytes,sizeof(receive_bytes));
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        address.sin_port=htons(endpoint.ok()?endpoint.value().port:0);
        if(peer<0||!endpoint.ok()||::connect(peer,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0 || !send_all(peer,wire(true)))failure=4;
        if(!failure) {
            std::string buffer;
            while(std::chrono::steady_clock::now()<deadline) {
                nf::FrameView frame;
                if(nf::inspect_frame(buffer,frame).ok())break;
                pollfd ready{peer,POLLIN,0};
                if(::poll(&ready,1,100)<=0)continue;
                std::array<char,512> bytes{};
                const auto size=::recv(peer,bytes.data(),bytes.size(),0);
                if(size<=0){failure=5;break;}
                buffer.append(bytes.data(),static_cast<std::size_t>(size));
            }
            if(!failure&&!send_all(peer,wire(false)))failure=6;
        }
        while(!failure&&!callback_returning.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(1ms);
        if(accepted!=64)failure=7;
        engine.request_stop();owner.join();
        if(peer>=0)::close(peer);
        if(result.code!=nf::ErrorCode::logout_timeout)failure=8;
    }
    std::filesystem::remove_all(directory);
    if(failure)std::cerr<<"queued bounded-shutdown test failed: "<<failure<<'\n';
    return failure;
}

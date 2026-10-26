#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <nimblefix/session.hpp>

#define CHECK(x) do { if(!(x)) { std::cerr<<"session crash boundary "<<__LINE__<<": " #x "\n"; std::exit(1); } } while(false)
namespace nf=nimblefix;
constexpr std::string_view wall="20261026-12:00:00.000";
struct Fault { bool armed{}; nf::JournalFaultPoint point{}; };
void kill_at(nf::JournalFaultPoint point,void* context) noexcept {
    auto& fault=*static_cast<Fault*>(context);
    if(fault.armed&&point==fault.point) ::kill(::getpid(),SIGKILL);
}
nf::WriteResult discard(void*,std::span<const char> bytes) noexcept { return {bytes.size(),false,{}}; }
nf::WriteResult capture(void* context,std::span<const char> bytes) noexcept {
    static_cast<std::vector<std::string>*>(context)->emplace_back(bytes.data(),bytes.size());
    return {bytes.size(),false,{}};
}
template<class Message> std::string peer_frame(const Message& message,std::uint64_t sequence) {
    std::array<char,2048> bytes;
    const auto encoded=nf::encode_fix44(message,{"BUY","SELL",sequence,wall},bytes);
    CHECK(encoded.ok());return {bytes.data(),encoded.value()};
}
nf::SessionConfig configuration(const char* directory,Fault* fault=nullptr) {
    nf::SessionConfig config;
    config.role=nf::SessionRole::acceptor;config.storage_directory=directory;
    config.session_key="FIX.4.4|local=SELL|remote=BUY";
    config.local_comp_id="SELL";config.remote_comp_id="BUY";
    if(fault) config.journal_test_hook={kill_at,fault};
    return config;
}
enum class Case { inbound, outbound, reply, delivery_before, delivery_after };

void crash_case(Case mode) {
    char pattern[]="/tmp/nimblefix-driver-crash-XXXXXX";
    const auto directory=mkdtemp(pattern);CHECK(directory);
    const auto child=fork();CHECK(child>=0);
    if(child==0) {
        Fault fault;
        nf::SessionCallbacks callbacks;
        callbacks.on_message=[&](auto& context,const auto&,const auto&) {
            if(mode==Case::reply) { fault.point=nf::JournalFaultPoint::postcommit;fault.armed=true; }
            if(mode==Case::reply||mode==Case::delivery_before||mode==Case::delivery_after) {
                auto response=nf::message<"8">().set<11>("CRASH-ORDER");
                const auto sent=context.send(response);if(!sent.ok()) return sent.status();
            }
            if(mode==Case::delivery_before||mode==Case::delivery_after) {
                fault.point=mode==Case::delivery_before?nf::JournalFaultPoint::precommit:nf::JournalFaultPoint::postcommit;
                fault.armed=true;
            }
            return nf::Status::success();
        };
        auto created=nf::SessionDriver::create(configuration(directory,&fault),std::move(callbacks));CHECK(created.ok());
        auto driver=std::move(created).value();CHECK(driver.connected(1,wall).ok());
        CHECK(driver.receive(peer_frame(nf::message<"A">().set<98>(0).set<108>(30),1),2,wall).ok());
        CHECK(driver.flush(discard,nullptr,3,65536).ok());CHECK(driver.state()==nf::SessionState::active);
        if(mode==Case::outbound) {
            fault.point=nf::JournalFaultPoint::postcommit;fault.armed=true;
            auto response=nf::message<"8">().set<11>("CRASH-ORDER");(void)driver.send(response,4,wall);
        } else {
            if(mode==Case::inbound) {fault.point=nf::JournalFaultPoint::postcommit;fault.armed=true;}
            (void)driver.receive(peer_frame(nf::message<"D">().set<11>("CRASH-ORDER"),2),4,wall);
        }
        _exit(3);
    }
    int status=0;while(waitpid(child,&status,0)<0) CHECK(errno==EINTR);
    CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
    std::filesystem::path journal_path;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(directory))
        if(entry.path().extension()==".nfxj") journal_path=entry.path();
    CHECK(!journal_path.empty());
    const auto config=configuration(directory);
    {
        const auto path=journal_path.string();
        auto opened=nf::Journal::open({.path=path,.session_key=config.session_key});CHECK(opened.ok());
        const auto& journal=opened.value();
        CHECK(journal.next_outbound_seq()==(mode==Case::inbound?2:3));
        CHECK(journal.next_inbound_seq()==(mode==Case::outbound?2:3));
        if(mode!=Case::outbound) {
            const nf::JournalRecord* input=nullptr;
            for(const auto& record:journal.records())if(record.kind==nf::RecordKind::inbound&&record.seq==2)input=&record;
            CHECK(input);CHECK(journal.has_delivery(input->id)==(mode==Case::delivery_after));
            if(mode!=Case::inbound) { const auto* output=journal.find_outbound(2);CHECK(output&&output->dependency==input->id); }
        } else CHECK(journal.find_outbound(2));
    }
    unsigned redelivered=0;
    nf::SessionCallbacks callbacks;
    callbacks.on_message=[&](auto& context,const auto& message,const auto& delivery) {
        CHECK(context.is_recovery()&&delivery.is_recovery);
        CHECK(message.find_unique(11).value().value=="CRASH-ORDER");++redelivered;
        if(mode==Case::reply||mode==Case::delivery_before) {
            auto response=nf::message<"8">().set<11>("CRASH-ORDER");
            return context.send(response).status();
        }
        return nf::Status::success();
    };
    {
        auto created=nf::SessionDriver::create(config,std::move(callbacks));CHECK(created.ok());
        auto driver=std::move(created).value();CHECK(driver.connected(10,wall).ok());
        const auto next=driver.next_inbound_seq();
        const auto resumed=driver.receive(peer_frame(nf::message<"A">().set<98>(0).set<108>(30),next),11,wall);
        if(!resumed.ok()) std::cerr<<"case="<<static_cast<int>(mode)<<" recovery: "<<resumed.message()<<" tag="<<resumed.tag<<" offset="<<resumed.offset<<'\n';
        CHECK(resumed.ok());
        std::vector<std::string> sent;
        CHECK(driver.flush(capture,&sent,12,65536).ok());
        CHECK(redelivered==((mode==Case::outbound||mode==Case::delivery_after)?0U:1U));
        if(mode!=Case::inbound) {
            CHECK(driver.receive(peer_frame(nf::message<"2">().set<7>(2).set<16>(2),driver.next_inbound_seq()),13,wall).ok());
            for(unsigned i=0;i<8&&driver.needs_write();++i) CHECK(driver.flush(capture,&sent,14,65536).ok());
            bool replayed=false;
            for(const auto& bytes:sent) {
                nf::FrameView frame;CHECK(nf::inspect_frame(bytes,frame).ok());
                nf::FlatMessageView view;std::array<nf::FieldView,32> scratch;CHECK(nf::parse_flat(frame,scratch,view).ok());
                if(view.find_unique(35).value().value=="8") {
                    CHECK(view.find_unique(34).value().value=="2");CHECK(view.find_unique(43).value().value=="Y");
                    CHECK(view.find_unique(122).value().value==wall);CHECK(view.find_unique(11).value().value=="CRASH-ORDER");replayed=true;
                }
            }
            CHECK(replayed);
        }
    }
    std::filesystem::remove_all(directory);
}
int main(){for(const auto mode:{Case::inbound,Case::outbound,Case::reply,Case::delivery_before,Case::delivery_after})crash_case(mode);std::cout<<"five real-driver SIGKILL boundaries and restart replay passed\n";}

#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>
#include <nimblefix/session.hpp>

namespace {
bool tracking = false;
std::size_t allocations = 0, sync_calls = 0;
constexpr std::string_view wall = "20261026-12:00:00.000";
void require(bool good) { if (!good) { std::cerr << "session benchmark failed\n"; std::exit(1); } }
struct Link { nimblefix::SessionDriver* target; std::uint64_t now; };
nimblefix::WriteResult deliver(void* context, std::span<const char> bytes) noexcept {
    auto& link = *static_cast<Link*>(context);
    const auto status = link.target->receive({bytes.data(),bytes.size()},link.now,wall);
    return {status.ok() ? bytes.size() : 0,false,status};
}
}
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t size) { if (tracking) ++allocations; return __real_malloc(size); }
extern "C" int __real_fsync(int);
extern "C" int __real_fdatasync(int);
extern "C" int __real_msync(void*,std::size_t,int);
extern "C" int __wrap_fsync(int fd) { if (tracking) ++sync_calls; return __real_fsync(fd); }
extern "C" int __wrap_fdatasync(int fd) { if (tracking) ++sync_calls; return __real_fdatasync(fd); }
extern "C" int __wrap_msync(void* address,std::size_t length,int flags) { if (tracking) ++sync_calls; return __real_msync(address,length,flags); }
void* operator new(std::size_t size) { if (auto* p=std::malloc(size?size:1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }

int main(int argc,char** argv) {
    std::size_t count=2000;
    if (argc>3) return 2;
    if(argc>=2) { const std::string_view text(argv[1]); auto r=std::from_chars(text.data(),text.data()+text.size(),count);
        if(r.ec!=std::errc{}||r.ptr!=text.data()+text.size()||count<100||count>10000) return 2; }
    const auto parent=std::filesystem::absolute(argc==3?argv[2]:"build/bench-state");
    std::filesystem::create_directories(parent);
    auto pattern=(parent/"nimblefix-session-bench-XXXXXX").string();
    auto* directory=mkdtemp(pattern.data()); require(directory!=nullptr);
    using Shape=nimblefix::Fields<nimblefix::Field<900001,nimblefix::FieldKind::signed_integer>>;
    nimblefix::SessionCallbacks sell_callbacks;
    std::size_t delivered_count=0;
    sell_callbacks.on_message=[](auto& context,const auto& view,const auto&) {
        auto field=view.find_unique(900001);
        if(!field.ok()) return field.status();
        int value=0; auto parsed=std::from_chars(field.value().value.data(),field.value().value.data()+field.value().value.size(),value);
        if(parsed.ec!=std::errc{}) return nimblefix::Status{nimblefix::ErrorCode::invalid_value};
        nimblefix::FixedMessage<"Z",Shape,0> reply;
        auto status=reply.set<900001>(value); if(!status.ok()) return status;
        return context.send(reply).status();
    };
    nimblefix::SessionCallbacks buy_callbacks;
    buy_callbacks.on_message=[&](auto&,const auto&,const auto&) { ++delivered_count; return nimblefix::Status::success(); };
    nimblefix::SessionConfig buy_config;
    buy_config.role=nimblefix::SessionRole::initiator; buy_config.local_comp_id="BUY";buy_config.remote_comp_id="SELL";
    buy_config.storage_directory=directory;buy_config.session_key="FIX.4.4|BUY|SELL";
    buy_config.journal_max_records=32768;
    auto sell_config=buy_config;sell_config.role=nimblefix::SessionRole::acceptor;
    sell_config.local_comp_id="SELL";sell_config.remote_comp_id="BUY";sell_config.session_key="FIX.4.4|SELL|BUY";
    {
        auto a=nimblefix::SessionDriver::create(buy_config,std::move(buy_callbacks)); require(a.ok());
        auto b=nimblefix::SessionDriver::create(sell_config,std::move(sell_callbacks)); require(b.ok());
        auto buy=std::move(a).value();auto sell=std::move(b).value();
        require(sell.connected(1,wall).ok());require(buy.connected(1,wall).ok());
        Link to_sell{&sell,1},to_buy{&buy,1};
        auto pump=[&] {
            for(unsigned i=0;i<16&&(buy.needs_write()||sell.needs_write());++i) {
                require(buy.flush(deliver,&to_sell,to_sell.now,65536).ok());
                require(sell.flush(deliver,&to_buy,to_buy.now,65536).ok());
            }
            require(!buy.needs_write()&&!sell.needs_write());
        };
        pump(); require(buy.state()==nimblefix::SessionState::active&&sell.state()==nimblefix::SessionState::active);
        nimblefix::FixedMessage<"Z",Shape,0> request;
        std::vector<std::uint64_t> samples(count);
        rusage before{},after{};
        for(std::size_t i=0;i<count+100;++i) {
            if(i==100)getrusage(RUSAGE_SELF,&before);
            to_sell.now=to_buy.now=i+2;
            require(request.set<900001>(static_cast<int>(i)).ok());
            allocations=sync_calls=0;tracking=i>=100;
            const auto start=std::chrono::steady_clock::now();
            require(buy.send(request,i+2,wall).ok());pump();
            const auto end=std::chrono::steady_clock::now();tracking=false;
            require(allocations==0&&sync_calls==0);
            if(i>=100) samples[i-100]=std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count();
        }
        require(delivered_count==count+100);
        getrusage(RUSAGE_SELF,&after);
        std::cerr<<"{\"minor_faults\":"<<after.ru_minflt-before.ru_minflt<<",\"major_faults\":"<<after.ru_majflt-before.ru_majflt
            <<",\"allocation_calls_per_operation\":0,\"sync_calls_per_operation\":0}\n";
        std::cout<<"iteration,round_trip_ns,allocation_calls,sync_calls\n";
        for(std::size_t i=0;i<count;++i) std::cout<<i<<','<<samples[i]<<",0,0\n";
    }
    std::filesystem::remove_all(directory);
}

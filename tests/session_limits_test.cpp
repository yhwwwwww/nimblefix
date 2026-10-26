#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <nimblefix/session.hpp>
#include <unistd.h>

#define CHECK(x) do { if(!(x)){std::cerr<<"session limits "<<__LINE__<<": " #x "\n";std::exit(1);} }while(false)
namespace nf=nimblefix;
constexpr std::string_view wall="20261027-00:00:00.000";
struct Directory { std::string path; Directory(){char p[]="/tmp/nimblefix-limits-XXXXXX";auto* d=mkdtemp(p);CHECK(d);path=d;} ~Directory(){std::filesystem::remove_all(path);} };
nf::WriteResult written(void*,std::span<const char> data)noexcept{return{data.size(),false,{}};}
nf::WriteResult blocked(void*,std::span<const char>)noexcept{return{0,true,{}};}
nf::SessionConfig config(const Directory& directory) {
    nf::SessionConfig c;c.role=nf::SessionRole::acceptor;c.storage_directory=directory.path;
    c.session_key="FIX.4.4/SELL/BUY";c.local_comp_id="SELL";c.remote_comp_id="BUY";
    c.max_frame_bytes=1024;c.max_pending_messages=10;c.admin_reserve_messages=2;
    c.max_pending_bytes=6144;c.admin_reserve_bytes=2048;c.receipt_capacity=4;
    c.write_stall_ms=10;c.logout_timeout_ms=5;return c;
}
void activate(nf::SessionDriver& driver) {
    CHECK(driver.connected(0,wall).ok());std::array<char,512> bytes;
    auto logon=nf::message<"A">().set<98>(0).set<108>(30);
    auto n=nf::encode_fix44(logon,{"BUY","SELL",1,wall},bytes);CHECK(n.ok());
    CHECK(driver.receive({bytes.data(),n.value()},1,wall).ok());
    CHECK(driver.flush(written,nullptr,1,65536).ok());CHECK(driver.can_send());
}
void messages_and_receipts() {
    Directory directory;unsigned capacity_events=0;
    nf::SessionCallbacks callbacks;callbacks.on_capacity=[&]{++capacity_events;};
    auto opened=nf::SessionDriver::create(config(directory),std::move(callbacks));CHECK(opened.ok());
    auto driver=std::move(opened).value();activate(driver);
    auto order=nf::message<"D">().set<11>("RETRY-OWNED");
    std::array<std::uint64_t,8> ids{};
    for(auto& id:ids){auto sent=driver.send(order,2,wall);CHECK(sent.ok());id=sent.value().id;}
    const auto before=driver.next_outbound_seq();
    CHECK(driver.send(order,2,wall).status().code==nf::ErrorCode::queue_full);
    CHECK(driver.next_outbound_seq()==before&&order.status().ok());
    CHECK(driver.receipt(ids.front()).status().code==nf::ErrorCode::receipt_expired);
    CHECK(driver.receipt(ids.back()).value().state==nf::ReceiptState::appended);
    CHECK(driver.flush(written,nullptr,3,65536).ok());
    CHECK(capacity_events==1&&driver.can_send());
    CHECK(driver.receipt(ids.back()).value().state==nf::ReceiptState::written);
    CHECK(driver.send(order,4,wall).ok());
}
void byte_budget_and_validation() {
    Directory directory;auto c=config(directory);c.max_pending_messages=100;c.max_pending_bytes=3072;
    auto opened=nf::SessionDriver::create(c);CHECK(opened.ok());auto driver=std::move(opened).value();activate(driver);
    auto bad=nf::message<"D">().set<11>(std::string(1500,'x'));
    const auto before=driver.next_outbound_seq();CHECK(!driver.send(bad,2,wall).ok());CHECK(driver.next_outbound_seq()==before);
    auto order=nf::message<"D">().set<11>(std::string(400,'x'));
    unsigned accepted=0;while(true){auto sent=driver.send(order,2,wall);if(!sent.ok()){CHECK(sent.status().code==nf::ErrorCode::queue_full);break;}++accepted;CHECK(accepted<10);}
    CHECK(accepted>=1&&accepted<10);CHECK(driver.next_outbound_seq()==before+accepted);
}
void stall_deadline_and_admin_reserve() {
    Directory directory;unsigned errors=0;nf::Status last;
    nf::SessionCallbacks callbacks;callbacks.on_error=[&](auto error){++errors;last=error;};
    auto opened=nf::SessionDriver::create(config(directory),std::move(callbacks));CHECK(opened.ok());auto driver=std::move(opened).value();activate(driver);
    auto order=nf::message<"D">().set<11>("BLOCKED");
    for(unsigned i=0;i<8;++i)CHECK(driver.send(order,2,wall).ok());
    CHECK(driver.flush(blocked,nullptr,3,65536).ok());
    CHECK(driver.tick(12,wall).ok()); // A reserved Logout can be appended despite business saturation.
    CHECK(driver.state()==nf::SessionState::logout_pending);
    CHECK(driver.pending_messages()>=9);
    CHECK(driver.tick(17,wall).code==nf::ErrorCode::write_stalled);
    CHECK(driver.should_close()&&errors==1&&last.code==nf::ErrorCode::write_stalled);
    CHECK(driver.disconnected({nf::ErrorCode::connection_closed},18).ok());
    CHECK(driver.connected(19,wall).ok());CHECK(driver.state()==nf::SessionState::awaiting_logon);
}
void delivery_store_failure_is_fatal_and_reported() {
    Directory directory;auto c=config(directory);c.journal_max_records=4;
    unsigned delivered=0,errors=0;nf::Status last;
    nf::SessionCallbacks callbacks;
    callbacks.on_message=[&](auto&,const auto&,const auto&){++delivered;return nf::Status::success();};
    callbacks.on_error=[&](nf::Status error){++errors;last=error;};
    auto opened=nf::SessionDriver::create(c,std::move(callbacks));CHECK(opened.ok());
    auto driver=std::move(opened).value();activate(driver);
    auto order=nf::message<"D">().set<11>("LAST-COMMITTED-INPUT");std::array<char,512> bytes;
    const auto encoded=nf::encode_fix44(order,{"BUY","SELL",2,wall},bytes);CHECK(encoded.ok());
    CHECK(driver.receive({bytes.data(),encoded.value()},2,wall).code==nf::ErrorCode::storage_full);
    CHECK(delivered==1&&errors==1&&last.code==nf::ErrorCode::storage_full);
    CHECK(driver.state()==nf::SessionState::failed&&driver.should_close());
    CHECK(driver.next_inbound_seq()==3); // Input commit survived; completion did not.
    CHECK(driver.disconnected({nf::ErrorCode::connection_closed},3).ok());
    CHECK(driver.status().code==nf::ErrorCode::storage_full&&driver.state()==nf::SessionState::failed);
    CHECK(driver.connected(4,wall).code==nf::ErrorCode::storage_full);
    CHECK(errors==1);
}
void outbound_store_failure_keeps_sequence_and_reports_error() {
    Directory directory;auto c=config(directory);c.journal_max_records=3;
    unsigned errors=0;nf::Status last;
    nf::SessionCallbacks callbacks;callbacks.on_error=[&](nf::Status error){++errors;last=error;};
    auto opened=nf::SessionDriver::create(c,std::move(callbacks));CHECK(opened.ok());
    auto driver=std::move(opened).value();activate(driver);
    auto order=nf::message<"D">().set<11>("NOT-ACCEPTED");
    const auto sequence=driver.next_outbound_seq();
    CHECK(driver.send(order,2,wall).status().code==nf::ErrorCode::storage_full);
    CHECK(driver.next_outbound_seq()==sequence&&order.status().ok());
    CHECK(driver.state()==nf::SessionState::failed&&driver.should_close());
    CHECK(errors==1&&last.code==nf::ErrorCode::storage_full);
    CHECK(driver.send(order,3,wall).status().code==nf::ErrorCode::storage_full&&errors==1);
}
void logout_timeout_is_reported() {
    Directory directory;unsigned errors=0;nf::Status last;
    nf::SessionCallbacks callbacks;callbacks.on_error=[&](nf::Status error){++errors;last=error;};
    auto opened=nf::SessionDriver::create(config(directory),std::move(callbacks));CHECK(opened.ok());
    auto driver=std::move(opened).value();activate(driver);
    CHECK(driver.logout("stop",2,wall).ok());CHECK(driver.flush(written,nullptr,3,65536).ok());
    CHECK(driver.tick(6,wall).ok());
    CHECK(driver.tick(7,wall).code==nf::ErrorCode::logout_timeout);
    CHECK(driver.state()==nf::SessionState::failed&&driver.should_close());
    CHECK(errors==1&&last.code==nf::ErrorCode::logout_timeout);
}
int main(){messages_and_receipts();byte_budget_and_validation();stall_deadline_and_admin_reserve();delivery_store_failure_is_fatal_and_reported();outbound_store_failure_keeps_sequence_and_reports_error();logout_timeout_is_reported();std::cout<<"bounded message/byte admission, receipts, low-water, store failure and stall deadlines passed\n";}

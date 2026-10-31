#include "../src/session/core.hpp"

#include <cstdlib>
#include <iostream>

namespace nf = nimblefix;
namespace core = nimblefix::detail;
#define CHECK(value) do { if (!(value)) { std::cerr << "protocol core: " << __LINE__ << ": " #value "\n"; std::exit(1); } } while (false)

int main() {
    core::SessionCore session{nf::SessionRole::acceptor,30'000,10'000,5000,7,9};
    auto result=session.process({.kind=core::CoreEventKind::reset_epoch});
    CHECK(result.status.ok()&&session.next_inbound()==1&&session.next_outbound()==1);
    CHECK(session.process({.kind=core::CoreEventKind::connected,.now_ms=0}).status.ok());
    CHECK(session.process({.kind=core::CoreEventKind::inbound_committed,.now_ms=1,.sequence=1,.first=30'000,.msg_type="A"}).status.ok());
    CHECK(session.process({.kind=core::CoreEventKind::outbound_appended,.now_ms=1,.sequence=1,.msg_type="A"}).status.ok());
    CHECK(session.process({.kind=core::CoreEventKind::logon_reply_appended,.now_ms=1}).status.ok());
    CHECK(session.state()==nf::SessionState::active);
    CHECK(session.process({.kind=core::CoreEventKind::reset_epoch}).status.code==nf::ErrorCode::sequence_reset_denied);
    CHECK(session.state()==nf::SessionState::active&&session.next_inbound()==2);
    result=session.process({.kind=core::CoreEventKind::inbound_candidate,.now_ms=2,.sequence=7,.msg_type="D"});
    CHECK(result.status.ok()&&result.inbound==core::InboundDisposition::gap&&result.action_count==1);
    CHECK(result.actions[0].kind==core::CoreActionKind::send_resend_request&&result.actions[0].first==2);
    result=session.process({.kind=core::CoreEventKind::sequence_reset_committed,.now_ms=3,.sequence=999,.next_sequence=10});
    CHECK(result.status.ok()&&session.next_inbound()==10&&session.next_outbound()==2);
    result=session.process({.kind=core::CoreEventKind::inbound_candidate,.now_ms=4,.sequence=10,.msg_type="D"});
    CHECK(result.status.ok()&&result.inbound==core::InboundDisposition::accept);
    CHECK(session.process({.kind=core::CoreEventKind::sequence_reset_committed,.now_ms=5,.next_sequence=10}).status.code==nf::ErrorCode::protocol_error);
    CHECK(session.state()==nf::SessionState::failed);
    std::cout<<"committed SequenceReset cursor, MsgSeqNum independence and explicit epoch policy passed\n";
}

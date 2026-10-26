#include <array>
#include <charconv>
#include <iostream>
#include <nimblefix/engine.hpp>

int main(int argc,char** argv) {
    if(argc!=3) return 2;
    unsigned port=0;const std::string_view text(argv[1]);
    auto parsed=std::from_chars(text.data(),text.data()+text.size(),port);
    if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()||!port||port>65535)return 2;
    auto created=nimblefix::Engine<>::create({.storage={.directory=argv[2]},.reconnect_delay_ms=20,.logout_timeout_ms=1000});
    if(!created.ok())return 1;
    auto engine=std::move(created).value();
    unsigned active=0,reports=0,errors=0;
    if(!engine.on_session_active([&](nimblefix::SessionContext& context) {
        ++active;std::array<char,16> id{};auto value=std::to_chars(id.data(),id.data()+id.size(),active);
        auto order=nimblefix::message<"D">().set<11>(std::string_view(id.data(),value.ptr));
        return context.send(order).status();
    }).ok())return 1;
    if(!engine.on_message([&](nimblefix::SessionContext&,const nimblefix::FlatMessageView& message) {
        auto id=message.find_unique(11);if(!id.ok())return id.status();
        ++reports;
        if(id.value().value!=std::to_string(reports))return nimblefix::Status{nimblefix::ErrorCode::invalid_value};
        if(reports==2)engine.request_stop();
        return nimblefix::Status::success();
    }).ok())return 1;
    if(!engine.on_error([&](const auto& error){++errors;std::cerr<<error.format()<<'\n';}).ok())return 1;
    if(!engine.add_initiator({.name="raw-peer",.sender_comp_id="BUY",.target_comp_id="SELL",.endpoint={"127.0.0.1",static_cast<std::uint16_t>(port)}}).ok())return 1;
    const auto result=engine.run();
    std::cout<<"{\"active\":"<<active<<",\"reports\":"<<reports<<",\"errors\":"<<errors<<"}\n";
    return result.ok()&&active==2&&reports==2?0:1;
}

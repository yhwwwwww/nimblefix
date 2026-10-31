#include <nimblefix/engine.hpp>
#include <nimblefix/fix44_sample.hpp>
#include <nimblefix/message.hpp>

#include <filesystem>
#include <string_view>

namespace nf = nimblefix;

namespace {

struct ForeignSchema {
    static constexpr std::string_view fingerprint = "foreign-schema-v1";
};

using ForeignBuilder = decltype(nf::message<"D">().set<11>("foreign").set<55>("AAPL"));

struct ForeignMessage {
    using schema_type = ForeignSchema;
    ForeignBuilder builder{nf::message<"D">().set<11>("foreign").set<55>("AAPL")};

    [[nodiscard]] nf::detail::MessageView view() const noexcept { return builder.view(); }
};

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() / "nimblefix-sender-schema-test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);

    {
        auto opened = nf::Engine<nf::fix44_sample::Schema>::create({.storage={.directory=directory}});
        if (!opened.ok()) return 6;
        auto engine = std::move(opened).value();
        nf::SessionSettings settings;
        settings.version = nf::FixVersion::fix42;
        if (!engine.add_initiator({.name="wrong-version",.sender_comp_id="BUY",.target_comp_id="SELL",
            .endpoint={"127.0.0.1",12345},.settings=settings}).ok()) return 7;
        if (engine.validate().code != nf::ErrorCode::unsupported_protocol) return 8;
    }
    const auto& descriptor=nf::fix44_sample::Schema::descriptor();
    if (!descriptor.matches_session(nf::FixVersion::fix44) ||
        !descriptor.matches_session(nf::FixVersion::fixt11,"6") ||
        descriptor.matches_session(nf::FixVersion::fixt11,"9")) return 9;
    nf::Sender<ForeignSchema> stale_sender;
    {
        auto opened = nf::Engine<nf::fix44_sample::Schema>::create({
            .storage = {.directory = directory}});
        if (!opened.ok()) return 1;
        auto engine = std::move(opened).value();
        if (!engine.add_listener({.name = "test", .endpoint = {"127.0.0.1", 0}}).ok()) return 2;
        auto session = engine.add_acceptor({.name = "peer", .listener = "test",
            .local_comp_id = "SELL", .remote_comp_id = "BUY"});
        if (!session.ok()) return 3;

        stale_sender = session.value().sender<ForeignSchema>();
        const auto mismatch = stale_sender.send(ForeignMessage{});
        if (mismatch.ok() || mismatch.status().code != nf::ErrorCode::schema_mismatch) return 4;
    }

    const auto stopped = stale_sender.send(ForeignMessage{});
    std::filesystem::remove_all(directory, ignored);
    return !stopped.ok() && stopped.status().code == nf::ErrorCode::engine_stopped ? 0 : 5;
}

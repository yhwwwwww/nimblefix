#include "builtin_schemas.hpp"
#include "config.hpp"
#include <nimblefix/journal.hpp>
#include <nimblefix/replay.hpp>
#include <nimblefix/telemetry.hpp>
#include <nimblefix/tls.hpp>

#if defined(NIMBLEFIX_ENABLE_XML)
#include <nimblefix/xml_schema.hpp>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
namespace nf = nimblefix;
namespace {
constexpr std::string_view version = "0.3.0";
static_assert(std::atomic<int>::is_always_lock_free);
std::atomic<int> interrupted{};
extern "C" void signal_handler(int signal) {
    interrupted.store(signal, std::memory_order_relaxed);
}

std::string quote(std::string_view text, bool binary = false) {
    std::string result{"\""};
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(c));
        } else if (c < 0x20 || (binary && c >= 0x7f)) {
            result.append("\\u00");
            result.push_back(hex[c >> 4]);
            result.push_back(hex[c & 15]);
        } else
            result.push_back(static_cast<char>(c));
    }
    result.push_back('"');
    return result;
}

struct Arguments {
    std::string command;
    std::string operation;
    std::filesystem::path config;
    std::filesystem::path directory;
    std::filesystem::path path;
    std::filesystem::path output;
    std::filesystem::path generator;
    std::vector<std::filesystem::path> xml;
    std::string name_space{"venue"};
    std::string session_key;
    std::uint32_t duration_ms{};
    std::uint32_t samples{10'000};
    std::string bench_case{"encode"};
    std::string replay_direction{"outbound"};
    std::string replay_timing{"full"};
    double replay_speed{1.0};
    std::uint64_t replay_seq_from{1};
    std::uint64_t replay_seq_to{0};
    std::uint32_t replay_epoch{0};
    std::string replay_endpoint;
    bool skip_admin{};
    bool json{};
    bool prometheus{};
    bool help{};
    bool print_version{};
    bool force{};
};

void help() {
    std::cout << "NimbleFIX " << version
              << "\n"
                 "Usage: nimblefix COMMAND [OPTIONS]\n"
                 "  init --directory PATH [--force]\n"
                 "  validate --config PATH [--json]\n"
                 "  run --config PATH [--duration-ms N] [--json]\n"
                 "  schema check|summary --xml PATH [--xml PATH] [--json]\n"
                 "  schema generate --xml PATH --namespace NAME --output HEADER [--generator PATH]\n"
                 "  inspect frame --path PATH [--json]\n"
                 "  inspect journal --path PATH --session-key KEY [--json]\n"
                 "  telemetry --path PATH [--json | --prometheus]\n"
                 "  replay --path PATH [--session-key KEY] [--direction inbound|outbound|both]\n"
                 "         [--timing full|original|scaled] [--speed FACTOR] [--seq-from N] [--seq-to M]\n"
                 "         [--epoch N] [--skip-admin] [--endpoint HOST:PORT] [--json]\n"
                 "  doctor [--config PATH] [--json]\n"
                 "  bench --case encode|parse [--samples N] [--json] (optional benchmark build)\n"
                 "  --help | --version\n";
}

bool parse(int argc, char **argv, Arguments &args) {
    if (argc == 1) {
        args.help = true;
        return true;
    }
    std::set<std::string_view> supplied;
    for (int i = 1; i < argc; ++i) {
        const std::string_view value{argv[i]};
        if (value == "--help" || value == "-h") {
            args.help = true;
            continue;
        }
        if (value == "--version") {
            args.print_version = true;
            continue;
        }
        if (value.starts_with("--")) supplied.insert(value);
        if (value == "--json") {
            args.json = true;
            continue;
        }
        if (value == "--prometheus") {
            args.prometheus = true;
            continue;
        }
        if (value == "--force") {
            args.force = true;
            continue;
        }
        if (value == "--skip-admin") {
            args.skip_admin = true;
            continue;
        }
        if (!value.starts_with('-') && args.command.empty()) {
            args.command = value;
            continue;
        }
        if (!value.starts_with('-') && args.operation.empty() &&
            (args.command == "schema" || args.command == "inspect")) {
            args.operation = value;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << value << '\n';
            return false;
        }
        const std::string_view next{argv[++i]};
        if (value == "--config")
            args.config = next;
        else if (value == "--directory")
            args.directory = next;
        else if (value == "--path")
            args.path = next;
        else if (value == "--output")
            args.output = next;
        else if (value == "--generator")
            args.generator = next;
        else if (value == "--xml")
            args.xml.emplace_back(next);
        else if (value == "--namespace")
            args.name_space = next;
        else if (value == "--session-key")
            args.session_key = next;
        else if (value == "--direction")
            args.replay_direction = next;
        else if (value == "--timing")
            args.replay_timing = next;
        else if (value == "--speed") {
            try {
                std::size_t idx = 0;
                const double val = std::stod(std::string{next}, &idx);
                if (idx != next.size() || !std::isfinite(val) || val <= 0.0) {
                    std::cerr << "--speed must be a positive number\n";
                    return false;
                }
                args.replay_speed = val;
            } catch (...) {
                std::cerr << "--speed must be a positive number\n";
                return false;
            }
        }
        else if (value == "--seq-from") {
            std::uint64_t number{};
            const auto result = std::from_chars(next.data(), next.data() + next.size(), number);
            if (result.ec != std::errc{} || result.ptr != next.data() + next.size() || number == 0)
                return false;
            args.replay_seq_from = number;
        }
        else if (value == "--seq-to") {
            std::uint64_t number{};
            const auto result = std::from_chars(next.data(), next.data() + next.size(), number);
            if (result.ec != std::errc{} || result.ptr != next.data() + next.size())
                return false;
            args.replay_seq_to = number;
        }
        else if (value == "--epoch") {
            std::uint32_t number{};
            const auto result = std::from_chars(next.data(), next.data() + next.size(), number);
            if (result.ec != std::errc{} || result.ptr != next.data() + next.size())
                return false;
            args.replay_epoch = number;
        }
        else if (value == "--endpoint")
            args.replay_endpoint = next;
        else if (value == "--case") args.bench_case = next;
        else if (value == "--samples") {
            std::uint32_t number{};
            const auto result = std::from_chars(next.data(), next.data() + next.size(), number);
            if (result.ec != std::errc{} || result.ptr != next.data() + next.size() || number == 0 || number > 1'000'000)
                return false;
            args.samples = number;
        }
        else if (value == "--duration-ms") {
            std::uint64_t number{};
            const auto result = std::from_chars(next.data(), next.data() + next.size(), number);
            if (result.ec != std::errc{} || result.ptr != next.data() + next.size() || number == 0 ||
                number > 86'400'000) {
                std::cerr << "--duration-ms must be an integer in 1..86400000\n";
                return false;
            }
            args.duration_ms = static_cast<std::uint32_t>(number);
        } else {
            std::cerr << "unknown option: " << value << '\n';
            return false;
        }
    }
    if (args.help || args.print_version)
        return true;
    std::set<std::string_view> allowed{"--json"};
    if (args.command == "init") allowed.insert({"--directory", "--force"});
    else if (args.command == "run") allowed.insert({"--config", "--duration-ms"});
    else if (args.command == "validate" || args.command == "doctor") allowed.insert("--config");
    else if (args.command == "inspect") allowed.insert({"--path", "--session-key"});
    else if (args.command == "telemetry") allowed.insert({"--path", "--prometheus"});
    else if (args.command == "replay") allowed.insert({
        "--path", "--session-key", "--direction", "--timing", "--speed",
        "--seq-from", "--seq-to", "--epoch", "--skip-admin", "--endpoint"});
    else if (args.command == "schema") {
        allowed.insert("--xml");
        if (args.operation == "generate") allowed.insert({"--output", "--namespace", "--generator"});
    } else if (args.command == "bench") allowed.insert({"--case", "--samples"});
    for (const auto option : supplied) if (!allowed.contains(option)) {
        std::cerr << "option " << option << " is not supported by this command\n";
        return false;
    }
    if (args.command == "bench") return args.bench_case == "encode" || args.bench_case == "parse";
    if (args.command == "init")
        return !args.directory.empty();
    if (args.command == "validate" || args.command == "run")
        return !args.config.empty();
    if (args.command == "doctor")
        return true;
    if (args.command == "inspect")
        return !args.path.empty() &&
               (args.operation == "frame" || (args.operation == "journal" && !args.session_key.empty()));
    if (args.command == "telemetry")
        return !args.path.empty();
    if (args.command == "replay") {
        const bool dir_ok = args.replay_direction == "inbound" ||
                            args.replay_direction == "outbound" ||
                            args.replay_direction == "both";
        const bool timing_ok = args.replay_timing == "full" ||
                               args.replay_timing == "original" ||
                               args.replay_timing == "scaled";
        return !args.path.empty() && dir_ok && timing_ok;
    }
    if (args.command == "schema")
        return !args.xml.empty() && (args.operation == "check" || args.operation == "summary" ||
                                     (args.operation == "generate" && !args.output.empty()));
    std::cerr << "unknown or incomplete command\n";
    return false;
}

int errors(const nf::ConfigLoadResult &loaded, bool json) {
    if (json)
        std::cerr << nf::diagnostics_json(loaded.diagnostics) << '\n';
    else
        for (const auto &error : loaded.diagnostics) {
            std::cerr << error.source.string() << ':' << error.line << ':' << error.column << ": "
                      << error.status.message() << " [" << error.field << "]\n"
                      << "  received: " << error.received << "\n  expected: " << error.expected
                      << "\n  fix: " << error.fix << '\n';
        }
    return 2;
}

int problem(const Arguments& args, nf::Status status, std::string_view explanation, int exit_code = 2) {
    if (args.json)
        std::cerr << "{\"operation\":" << quote(args.command) << ",\"code\":" << static_cast<unsigned>(status.code)
            << ",\"message\":" << quote(status.message()) << ",\"detail\":" << quote(explanation) << "}\n";
    else std::cerr << status.message() << ": " << explanation << '\n';
    return exit_code;
}

struct ApplicationSchema {
    std::shared_ptr<const void> owner;
    const nf::SchemaDescriptor* descriptor{};
    nf::WireHints hints{};
};

int validate_application_versions(const nf::ConfigDocument& configuration, const Arguments& args,
    const ApplicationSchema& schema) {
    if (!schema.descriptor) return 0;
    const auto valid = [&](const nf::SessionSettings& settings) {
        return schema.descriptor->matches_session(settings.version, settings.default_appl_ver_id);
    };
    for (const auto& session : configuration.initiators) if (!valid(session.settings))
        return problem(args, {nf::ErrorCode::unsupported_protocol, nf::tag::begin_string}, "dictionary version does not match initiator " + session.name);
    for (const auto& session : configuration.acceptors) if (!valid(session.settings))
        return problem(args, {nf::ErrorCode::unsupported_protocol, nf::tag::begin_string}, "dictionary version does not match acceptor " + session.name);
    return 0;
}

int load_application_schema(const nf::ConfigDocument& configuration, const Arguments& args, ApplicationSchema& result) {
    if (configuration.schema != "none") {
        const auto builtin = nf::cli_builtin::find_standard_schema(configuration.schema);
        if (builtin.descriptor == nullptr) {
            return problem(args, {nf::ErrorCode::unsupported},
                "this build does not contain the generated " + configuration.schema + " schema");
        }
        result.descriptor = builtin.descriptor;
        result.hints = builtin.hints;
    }
    if (configuration.dictionaries.empty()) return validate_application_versions(configuration, args, result);
#if defined(NIMBLEFIX_ENABLE_XML)
    std::vector<std::string> owned;
    std::vector<std::string_view> paths;
    owned.reserve(configuration.dictionaries.size());
    for (const auto& path : configuration.dictionaries) owned.push_back(path.string());
    for (const auto& path : owned) paths.push_back(path);
    auto schema = std::make_shared<nf::XmlSchema>();
    nf::XmlSchemaDiagnostic diagnostic;
    const auto loaded = nf::load_xml_schema(paths, *schema, diagnostic);
    if (!loaded.ok()) {
        if (args.json)
            std::cerr << "{\"code\":" << static_cast<unsigned>(loaded.code) << ",\"source\":" << quote(diagnostic.source)
                << ",\"line\":" << diagnostic.line << ",\"column\":" << diagnostic.column
                << ",\"message\":" << quote(diagnostic.message) << "}\n";
        else std::cerr << diagnostic.source << ':' << diagnostic.line << ':' << diagnostic.column << ": " << diagnostic.message << '\n';
        return 2;
    }
    if (result.descriptor != nullptr && !nf::check_schema_fingerprint(*result.descriptor, *schema).ok())
        return problem(args, {nf::ErrorCode::schema_mismatch},
            "XML fingerprint " + std::string{schema->fingerprint()} + " differs from generated fingerprint " +
            std::string{result.descriptor->fingerprint} + "; regenerate the C++ binding from the same XML");
    result.descriptor = &schema->descriptor();
    result.hints = schema->wire_hints();
    result.owner = std::move(schema);
    return validate_application_versions(configuration, args, result);
#else
    return problem(args, {nf::ErrorCode::unsupported}, "runtime XML requires the optional XML component");
#endif
}

int check_schema(const Arguments& args) {
    nf::ConfigDocument configuration;
    for (const auto& path : args.xml) configuration.dictionaries.push_back(std::filesystem::absolute(path));
    ApplicationSchema schema;
    const auto checked = load_application_schema(configuration, args, schema);
    if (checked != 0) return checked;
    const auto& descriptor = *schema.descriptor;
    if (args.json)
        std::cout << "{\"valid\":true,\"type\":" << quote(descriptor.type) << ",\"version\":" << quote(descriptor.version)
            << ",\"fingerprint\":" << quote(descriptor.fingerprint) << ",\"fields\":" << descriptor.fields.size()
            << ",\"messages\":" << descriptor.messages.size() << ",\"groups\":" << descriptor.groups.size() << "}\n";
    else std::cout << "XML schema valid: " << descriptor.type << '.' << descriptor.version << "; fields="
        << descriptor.fields.size() << " messages=" << descriptor.messages.size() << " groups=" << descriptor.groups.size()
        << " fingerprint=" << descriptor.fingerprint << '\n';
    return 0;
}

bool write_new(const std::filesystem::path &path, std::string_view data) {
    const auto descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return false;
    std::size_t written = 0;
    while (written < data.size()) {
        const auto count = ::write(descriptor, data.data() + written, data.size() - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            ::close(descriptor);
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return ::close(descriptor) == 0;
}

int init(const Arguments &args) {
    const auto directory = std::filesystem::absolute(args.directory);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::cerr << "cannot create configuration directory\n";
        return 1;
    }
    const auto acceptor = directory / "acceptor.toml";
    const auto initiator = directory / "initiator.toml";
    if (std::filesystem::exists(acceptor) || std::filesystem::exists(initiator)) {
        if (!args.force) {
            std::cerr << "configuration exists; use --force to replace these two files\n";
            return 2;
        }
        std::filesystem::remove(acceptor, error);
        if (error)
            return 1;
        std::filesystem::remove(initiator, error);
        if (error)
            return 1;
    }
    constexpr std::string_view sell =
        "version = 1\n[engine]\ndispatch = \"inline\"\nschema = \"none\"\n"
        "[engine.storage]\ndirectory = \"./state/sell\"\nconfirmation = \"appended\"\n"
        "[[listeners]]\nname = \"main\"\nendpoint = \"127.0.0.1:9876\"\n"
        "[[sessions]]\nname = \"buy-side\"\nrole = \"acceptor\"\nlistener = \"main\"\n"
        "transport = \"FIX.4.4\"\nlocal_comp_id = \"SELL\"\nremote_comp_id = \"BUY\"\n";
    constexpr std::string_view buy =
        "version = 1\n[engine]\ndispatch = \"inline\"\nschema = \"none\"\n"
        "[engine.storage]\ndirectory = \"./state/buy\"\nconfirmation = \"appended\"\n"
        "[[sessions]]\nname = \"venue-a\"\nrole = \"initiator\"\ntransport = \"FIX.4.4\"\n"
        "sender_comp_id = \"BUY\"\ntarget_comp_id = \"SELL\"\nendpoint = \"127.0.0.1:9876\"\n";
    if (!write_new(acceptor, sell) || !write_new(initiator, buy)) {
        std::cerr << "could not write both configuration files\n";
        return 1;
    }
    if (args.json)
        std::cout << "{\"acceptor\":" << quote(acceptor.string())
                  << ",\"initiator\":" << quote(initiator.string()) << "}\n";
    else
        std::cout << "Created " << acceptor << " and " << initiator << '\n';
    return 0;
}

std::filesystem::path find_generator(const Arguments &args) {
    if (!args.generator.empty())
        return std::filesystem::absolute(args.generator);
    std::array<char, 4096> buffer;
    const auto count = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (count <= 0 || static_cast<std::size_t>(count) >= buffer.size())
        return {};
    const auto directory =
        std::filesystem::path{std::string{buffer.data(), static_cast<std::size_t>(count)}}.parent_path();
    for (const auto &candidate : {directory / "nimblefix-schema-gen",
                                  directory.parent_path() / "bin/nimblefix-schema-gen"})
        if (std::filesystem::is_regular_file(candidate))
            return candidate;
    return {};
}

int generate_schema(const Arguments &args) {
    const auto generator = find_generator(args);
    if (generator.empty()) {
        std::cerr << "installed schema generator (nimblefix-schema-gen) is missing; set --generator PATH\n";
        return 1;
    }
    std::vector<std::string> values{generator.string(),
                                    "--namespace", args.name_space,
                                    "--output",    std::filesystem::absolute(args.output).string()};
    for (const auto &xml : args.xml)
        values.push_back(std::filesystem::absolute(xml).string());
    std::vector<char *> arguments;
    for (auto &value : values)
        arguments.push_back(value.data());
    arguments.push_back(nullptr);
    pid_t child{};
    const auto spawned = ::posix_spawn(&child, generator.c_str(), nullptr, nullptr, arguments.data(), environ);
    if (spawned != 0) {
        std::cerr << "could not start C++ schema generator\n";
        return 1;
    }
    int status{};
    while (::waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return 1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 2;
}

int benchmark(const Arguments& args) {
#if defined(NIMBLEFIX_ENABLE_BENCH)
    auto message = nf::message<"D">().set<11>("CLI-BENCH").set<55>("AAPL")
        .set<54>('1').set<38>(100).set<40>('2').set<44>(nf::Decimal{15025, 2});
    nf::SessionHeader header{"BUY", "SELL", 1, "20261031-12:00:00.000"};
    std::array<char, 2048> output{};
    auto encoded = nf::encode_fix(message.view(), header, output);
    if (!encoded.ok()) return problem(args, encoded.status(), "cannot prepare benchmark frame", 1);
    const std::string wire{output.data(), encoded.value()};
    std::array<nf::FieldView, 64> fields{};
    std::vector<std::uint64_t> samples(args.samples);
    std::uint64_t observed = 0;
    for (std::size_t iteration = 0; iteration < samples.size() + 1000; ++iteration) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        const auto begin = std::chrono::steady_clock::now();
        if (args.bench_case == "encode") {
            header.msg_seq_num = iteration + 1;
            encoded = nf::encode_fix(message.view(), header, output);
            if (!encoded.ok()) return problem(args, encoded.status(), "benchmark encoding failed", 1);
            observed += encoded.value() + static_cast<unsigned char>(output[0]);
        } else {
            nf::FrameView frame;
            nf::FlatMessageView flat;
            auto status = nf::inspect_frame(wire, frame);
            if (status.ok()) status = nf::parse_flat(frame, fields, flat);
            if (!status.ok()) return problem(args, status, "benchmark parsing failed", 1);
            observed += flat.fields().size();
        }
        std::atomic_signal_fence(std::memory_order_seq_cst);
        const auto end = std::chrono::steady_clock::now();
        if (iteration >= 1000)
            samples[iteration - 1000] = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    }
    std::sort(samples.begin(), samples.end());
    const auto quantile = [&](std::size_t percent) { return samples[(samples.size() - 1) * percent / 100]; };
    if (args.json)
        std::cout << "{\"boundary\":\"codec\",\"case\":" << quote(args.bench_case)
            << ",\"samples\":" << samples.size() << ",\"warmup\":1000,\"unit\":\"ns\",\"p50\":" << quantile(50)
            << ",\"p99\":" << quantile(99) << ",\"max\":" << samples.back() << ",\"checksum\":" << observed << "}\n";
    else std::cout << "codec " << args.bench_case << " samples=" << samples.size()
        << " p50=" << quantile(50) << "ns p99=" << quantile(99) << "ns max=" << samples.back() << "ns\n";
    return 0;
#else
    return problem(args, {nf::ErrorCode::unsupported}, "enable the optional benchmarks build component");
#endif
}

int inspect_frame(const Arguments &args) {
    std::error_code error;
    const auto size = std::filesystem::file_size(args.path, error);
    if (error || size > 64U * 1024U * 1024U) {
        std::cerr << "frame input must be a readable file at most 64 MiB\n";
        return 2;
    }
    std::ifstream input(args.path, std::ios::binary);
    std::string wire{std::istreambuf_iterator<char>{input}, {}};
    std::size_t consumed = 0, frames = 0;
    std::string json_output;
    if (args.json) {
        json_output.reserve(wire.size() * 2 + 2);
        json_output.push_back('[');
    }
    while (consumed < wire.size()) {
        nf::FrameView frame;
        const auto inspected = nf::inspect_frame(std::string_view{wire}.substr(consumed), frame);
        if (!inspected.ok()) {
            std::cerr << "invalid frame at byte " << consumed << ": " << inspected.message() << '\n';
            return 1;
        }
        std::vector<nf::FieldView> scratch(frame.bytes.size() / 3 + 1);
        nf::FlatMessageView message;
        const auto parsed = nf::parse_flat(frame, scratch, message);
        if (!parsed.ok()) {
            std::cerr << parsed.message() << '\n';
            return 1;
        }
        const auto type = message.find_unique(nf::tag::msg_type);
        const bool logon = type.ok() && type.value().value == nf::msg_type::logon;
        if (args.json) {
            if (frames != 0)
                json_output.push_back(',');
            json_output += "{\"offset\":" + std::to_string(consumed) +
                           ",\"bytes\":" + std::to_string(frame.consumed) + ",\"fields\":[";
        } else
            std::cout << "frame offset=" << consumed << " bytes=" << frame.consumed << '\n';
        std::size_t index = 0;
        for (const auto &field : message.fields()) {
            const bool hidden =
                logon && field.tag != nf::tag::begin_string && field.tag != nf::tag::body_length &&
                field.tag != nf::tag::check_sum && field.tag != nf::tag::msg_type &&
                field.tag != nf::tag::msg_seq_num && field.tag != nf::tag::sender_comp_id &&
                field.tag != nf::tag::target_comp_id && field.tag != nf::tag::sending_time &&
                field.tag != nf::tag::encrypt_method && field.tag != nf::tag::heart_bt_int;
            const std::string_view value = hidden ? std::string_view{"<redacted>"} : field.value;
            if (args.json) {
                if (index++ != 0)
                    json_output.push_back(',');
                json_output += "{\"tag\":" + std::to_string(field.tag) + ",\"value\":" + quote(value, true) + '}';
            } else
                std::cout << "  " << field.tag << '=' << (hidden ? "<redacted>" : quote(value, true)) << '\n';
        }
        if (args.json)
            json_output += "]}";
        consumed += frame.consumed;
        ++frames;
    }
    if (args.json) {
        json_output += "]\n";
        std::cout << json_output;
    }
    return 0;
}

int show_telemetry(const Arguments &args) {
    auto opened = nf::TelemetryReader::open(std::filesystem::absolute(args.path));
    if (!opened.ok()) {
        return problem(args, opened.status(), "cannot open mmap telemetry region", 1);
    }
    const auto &reader = opened.value();
    if (args.prometheus) {
        std::cout << reader.format_prometheus();
        return 0;
    }
    const auto snaps = reader.read_all();
    constexpr std::string_view state_names[]{
        "disconnected", "awaiting_logon", "active", "logout_pending", "closed", "failed"};
    if (args.json) {
        std::cout << "{\"pid\":" << reader.writer_pid() << ",\"sessions\":[";
        for (std::size_t i = 0; i < snaps.size(); ++i) {
            const auto &s = snaps[i];
            if (i != 0) std::cout << ',';
            const auto st_idx = static_cast<std::size_t>(s.state);
            const auto st_name = st_idx < 6 ? state_names[st_idx] : std::string_view{"unknown"};
            std::cout << "{\"name\":" << quote(s.name)
                      << ",\"local_comp_id\":" << quote(s.local_comp_id)
                      << ",\"remote_comp_id\":" << quote(s.remote_comp_id)
                      << ",\"state\":" << quote(st_name)
                      << ",\"next_inbound_seq\":" << s.next_inbound_seq
                      << ",\"next_outbound_seq\":" << s.next_outbound_seq
                      << ",\"inbound_frames\":" << s.inbound_frames
                      << ",\"outbound_frames\":" << s.outbound_frames
                      << ",\"business_deliveries\":" << s.business_deliveries
                      << ",\"inbound_bytes\":" << s.inbound_bytes
                      << ",\"outbound_bytes\":" << s.outbound_bytes
                      << ",\"errors\":" << s.error_count
                      << ",\"active_endpoint_index\":" << s.active_endpoint_index
                      << ",\"stages\":{";
            for (std::size_t st = 0; st < static_cast<std::size_t>(nf::TelemetryStage::count); ++st) {
                if (st != 0) std::cout << ',';
                const auto &h = s.stages[st];
                std::cout << quote(nf::telemetry_stage_name(static_cast<nf::TelemetryStage>(st)))
                          << ":{\"count\":" << h.count
                          << ",\"min_ns\":" << h.min_ns
                          << ",\"p50_ns\":" << h.p50_ns
                          << ",\"p90_ns\":" << h.p90_ns
                          << ",\"p99_ns\":" << h.p99_ns
                          << ",\"p999_ns\":" << h.p999_ns
                          << ",\"p9999_ns\":" << h.p9999_ns
                          << ",\"max_ns\":" << h.max_ns << '}';
            }
            std::cout << "}}";
        }
        std::cout << "]}\n";
    } else {
        std::cout << "Telemetry writer PID=" << reader.writer_pid()
                  << " sessions=" << snaps.size() << '\n';
        for (const auto &s : snaps) {
            const auto st_idx = static_cast<std::size_t>(s.state);
            const auto st_name = st_idx < 6 ? state_names[st_idx] : std::string_view{"unknown"};
            std::cout << "  session=" << s.name << " (" << s.local_comp_id << "->" << s.remote_comp_id
                      << ") state=" << st_name
                      << " in=" << s.inbound_frames << " out=" << s.outbound_frames
                      << " deliveries=" << s.business_deliveries << " errors=" << s.error_count << '\n';
            for (std::size_t st = 0; st < static_cast<std::size_t>(nf::TelemetryStage::count); ++st) {
                const auto &h = s.stages[st];
                if (h.count == 0) continue;
                std::cout << "    stage=" << nf::telemetry_stage_name(static_cast<nf::TelemetryStage>(st))
                          << " n=" << h.count
                          << " p50=" << h.p50_ns << "ns"
                          << " p90=" << h.p90_ns << "ns"
                          << " p99=" << h.p99_ns << "ns"
                          << " p99.9=" << h.p999_ns << "ns"
                          << " p99.99=" << h.p9999_ns << "ns"
                          << " max=" << h.max_ns << "ns\n";
            }
        }
    }
    return 0;
}

int replay_journal(const Arguments &args) {
    nf::ReplayOptions opts{};
    opts.journal_path = std::filesystem::absolute(args.path);
    opts.session_key = args.session_key;
    if (args.replay_direction == "inbound") opts.direction = nf::ReplayDirection::inbound;
    else if (args.replay_direction == "both") opts.direction = nf::ReplayDirection::both;
    else opts.direction = nf::ReplayDirection::outbound;

    if (args.replay_timing == "original") opts.timing = nf::ReplayTiming::original;
    else if (args.replay_timing == "scaled") opts.timing = nf::ReplayTiming::scaled;
    else opts.timing = nf::ReplayTiming::full_speed;
    opts.speed_multiplier = args.replay_speed;
    opts.seq_from = args.replay_seq_from;
    opts.seq_to = args.replay_seq_to;
    opts.epoch_filter = args.replay_epoch;
    opts.skip_admin = args.skip_admin;

    nf::Result<nf::ReplayStats> res{nf::Status{nf::ErrorCode::invalid_config}};
    if (!args.replay_endpoint.empty()) {
        std::string host;
        std::uint16_t port = 0;
        const std::string_view ep = args.replay_endpoint;
        const auto colon = ep.rfind(':');
        if (colon == std::string_view::npos || colon == 0 || colon + 1 >= ep.size()) {
            return problem(args, {nf::ErrorCode::invalid_config}, "endpoint must be HOST:PORT or [IPv6]:PORT");
        }
        std::string_view host_sv = ep.substr(0, colon);
        if (host_sv.size() >= 2 && host_sv.front() == '[' && host_sv.back() == ']') {
            host_sv = host_sv.substr(1, host_sv.size() - 2);
        }
        host.assign(host_sv);
        const auto port_sv = ep.substr(colon + 1);
        std::uint32_t port_num = 0;
        const auto pr = std::from_chars(port_sv.data(), port_sv.data() + port_sv.size(), port_num);
        if (pr.ec != std::errc{} || pr.ptr != port_sv.data() + port_sv.size() || port_num == 0 || port_num > 65535) {
            return problem(args, {nf::ErrorCode::invalid_config}, "invalid port in --endpoint");
        }
        port = static_cast<std::uint16_t>(port_num);
        res = nf::JournalReplayer::replay_to_tcp(opts, nf::TcpEndpoint{host, port});
    } else {
        res = nf::JournalReplayer::replay(opts, [](const nf::JournalRecord &, const nf::FlatMessageView *) noexcept {
            return nf::Status::success();
        });
    }

    if (!res.ok()) {
        return problem(args, res.status(), "journal replay failed", 1);
    }
    const auto &s = res.value();
    if (args.json) {
        std::cout << "{\"mode\":" << quote(args.replay_endpoint.empty() ? "offline" : "tcp")
                  << ",\"direction\":" << quote(args.replay_direction)
                  << ",\"timing\":" << quote(args.replay_timing)
                  << ",\"scanned_records\":" << s.scanned_records
                  << ",\"replayed_frames\":" << s.replayed_frames
                  << ",\"skipped_records\":" << s.skipped_records
                  << ",\"invalid_frames\":" << s.invalid_frames
                  << ",\"total_bytes\":" << s.total_bytes
                  << ",\"first_seq\":" << s.first_seq
                  << ",\"last_seq\":" << s.last_seq
                  << ",\"elapsed_ns\":" << s.elapsed_ns
                  << ",\"total_sleep_ns\":" << s.total_sleep_ns << "}\n";
    } else {
        std::cout << "Replayed " << s.replayed_frames << " frame(s) (" << s.total_bytes
                  << " bytes, seq " << s.first_seq << ".." << s.last_seq
                  << ", scanned=" << s.scanned_records << ", skipped=" << s.skipped_records
                  << ", elapsed=" << (s.elapsed_ns / 1000ULL) << "us)\n";
    }
    return 0;
}

int inspect_journal(const Arguments &args) {
    nf::JournalConfig config;
    const auto path = std::filesystem::absolute(args.path).string();
    config.path = path;
    config.session_key = args.session_key;
    const auto snapshot = nf::Journal::inspect_snapshot(config);
    if (!snapshot.diagnostic.ok()) {
        std::cerr << snapshot.diagnostic.message() << '\n';
        return 1;
    }
    if (args.json)
        std::cout << "{\"format_version\":" << snapshot.format_version
                  << ",\"segments\":" << snapshot.segment_count
                  << ",\"committed_records\":" << snapshot.committed_records
                  << ",\"used_bytes\":" << snapshot.used_bytes
                  << ",\"next_inbound_seq\":" << snapshot.next_inbound_seq
                  << ",\"next_outbound_seq\":" << snapshot.next_outbound_seq
                  << ",\"checkpoint_stale\":" << (snapshot.checkpoint_stale ? "true" : "false") << "}\n";
    else
        std::cout << "format=" << snapshot.format_version << " segments=" << snapshot.segment_count
                  << " records=" << snapshot.committed_records << " bytes=" << snapshot.used_bytes
                  << " next-in=" << snapshot.next_inbound_seq << " next-out=" << snapshot.next_outbound_seq
                  << " checkpoint=" << (snapshot.checkpoint_stale ? "rebuild needed" : "valid") << '\n';
    return 0;
}

template <class Schema, class Policy>
int run_engine(const nf::ConfigDocument &configuration, const Arguments &args, nf::EngineConfig runtime_config) {
    auto created = nf::Engine<Schema, Policy>::create(std::move(runtime_config));
    if (!created.ok()) {
        std::cerr << created.status().message() << '\n';
        return 1;
    }
    auto engine = std::move(created).value();
    std::atomic<std::uint64_t> active{}, received{}, error_count{};
    nf::Status message_handler;
    if constexpr (std::is_same_v<Schema, nf::FlatSchema>) {
        message_handler = engine.on_message([&](auto &, const nf::FlatMessageView &) {
            received.fetch_add(1, std::memory_order_relaxed);
            return nf::Status::success();
        });
    } else {
        Schema::for_each_message([&]<class Message>() {
            const auto* descriptor = Schema::find_message(Message::msg_type);
            if (descriptor == nullptr || descriptor->category != "app") return;
            const auto registered = engine.template on<Message>(
                [&](nf::SessionContext&, typename Message::view_type) {
                    received.fetch_add(1, std::memory_order_relaxed);
                    return nf::Status::success();
                });
            if (!registered.ok() && message_handler.ok()) message_handler = registered;
        });
    }
    auto active_handler = engine.on_session_active([&](auto &) {
        active.fetch_add(1, std::memory_order_relaxed);
        return nf::Status::success();
    });
    auto error_handler = engine.on_error([&](const nf::ErrorEvent &event) {
        error_count.fetch_add(1, std::memory_order_relaxed);
        if (args.json)
            std::cerr << "{\"event\":\"error\",\"session\":" << quote(event.session_name)
                      << ",\"operation\":" << quote(event.operation) << ",\"code\":" << static_cast<unsigned>(event.status.code)
                      << ",\"message\":" << quote(event.status.message()) << "}\n";
        else
            std::cerr << event.session_name << ": " << event.status.message() << '\n';
    });
    if (!message_handler.ok() || !active_handler.ok() || !error_handler.ok())
        return 1;
    for (const auto &config : configuration.listeners)
        if (!engine.add_listener(config).ok())
            return 2;
    for (const auto &config : configuration.initiators)
        if (!engine.add_initiator(config).ok())
            return 2;
    for (const auto &config : configuration.acceptors)
        if (!engine.add_acceptor(config).ok())
            return 2;
    const auto valid = engine.validate();
    if (!valid.ok()) {
        std::cerr << valid.message() << '\n';
        return 2;
    }
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)::sigaction(SIGINT, &sa, nullptr);
    (void)::sigaction(SIGTERM, &sa, nullptr);
    std::atomic<bool> done{};
    nf::Status status;
    std::thread owner([&] {
        status = engine.run();
        done.store(true, std::memory_order_release);
    });
    struct ThreadJoinGuard {
        std::thread& t;
        nf::Engine<Schema, Policy>& eng;
        ~ThreadJoinGuard() {
            if (t.joinable()) {
                eng.request_stop();
                t.join();
            }
        }
    } owner_guard{owner, engine};
    const auto start = std::chrono::steady_clock::now();
    std::set<std::string> published_listeners;
    std::map<std::string, nf::SessionState> displayed_states;
    std::uint64_t displayed_active = 0;
    std::uint64_t displayed_messages = 0;
    while (!done.load(std::memory_order_acquire) && interrupted.load(std::memory_order_relaxed) == 0) {
        for (const auto& listener : configuration.listeners) {
            if (published_listeners.contains(listener.name)) continue;
            const auto endpoint = engine.listener_endpoint(listener.name);
            if (endpoint.ok()) {
                if (args.json)
                    std::cout << "{\"event\":\"listening\",\"listener\":" << quote(listener.name)
                        << ",\"host\":" << quote(endpoint.value().host) << ",\"port\":" << endpoint.value().port << "}\n";
                else
                    std::cout << "LISTENING " << listener.name << ' ' << endpoint.value().host << ':' << endpoint.value().port << '\n';
                std::cout.flush();
                published_listeners.insert(listener.name);
            }
        }
        const auto publish_state = [&](std::string_view name) {
            const auto current = engine.snapshot(name);
            if (!current.ok()) return;
            const auto& snapshot = current.value();
            const auto previous = displayed_states.find(snapshot.name);
            if (previous != displayed_states.end() && previous->second == snapshot.state) return;
            displayed_states[snapshot.name] = snapshot.state;
            constexpr std::string_view names[]{"disconnected", "awaiting_logon", "active", "logout_pending", "closed", "failed"};
            const auto state = names[static_cast<std::size_t>(snapshot.state)];
            if (args.json) std::cout << "{\"event\":\"session\",\"session\":" << quote(snapshot.name)
                << ",\"state\":" << quote(state) << ",\"generation\":" << snapshot.generation
                << ",\"next_inbound_seq\":" << snapshot.next_inbound_seq
                << ",\"next_outbound_seq\":" << snapshot.next_outbound_seq << "}\n";
            else std::cout << snapshot.name << ": " << state << " generation=" << snapshot.generation << '\n';
            std::cout.flush();
        };
        for (const auto& session : configuration.initiators) publish_state(session.name);
        for (const auto& session : configuration.acceptors) publish_state(session.name);
        const auto current = active.load(std::memory_order_relaxed);
        if (current != displayed_active) {
            if (args.json)
                std::cout << "{\"event\":\"active\",\"activations\":" << current << "}\n";
            else
                std::cout << "FIX session active (activations=" << current << ")\n";
            displayed_active = current;
            std::cout.flush();
        }
        const auto messages = received.load(std::memory_order_relaxed);
        if (messages != displayed_messages) {
            if (args.json) std::cout << "{\"event\":\"message\",\"business_messages\":" << messages << "}\n";
            else std::cout << "Business messages received=" << messages << '\n';
            displayed_messages = messages;
            std::cout.flush();
        }
        if (args.duration_ms != 0 &&
            std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(args.duration_ms))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    engine.request_stop();
    owner.join();
    if (args.json)
        std::cout << "{\"event\":\"stopped\",\"code\":" << static_cast<unsigned>(status.code)
                  << ",\"activations\":" << active.load() << ",\"business_messages\":" << received.load()
                  << ",\"error_events\":" << error_count.load() << "}\n";
    else
        std::cout << "Stopped: " << status.message() << "; business messages=" << received.load() << '\n';
    const auto signal = interrupted.load(std::memory_order_relaxed);
    return signal != 0 ? 128 + signal : status.ok() ? 0 : 1;
}

int run(const nf::ConfigDocument &configuration, const Arguments &args) {
    ApplicationSchema application;
    const auto checked = load_application_schema(configuration, args, application);
    if (checked != 0) return checked;
    nf::ConfigDocument effective_doc = configuration;
    auto engine_config = effective_doc.engine;
    engine_config.wire_hints = application.hints;
    if (application.descriptor != nullptr) {
        for (auto& s : effective_doc.initiators) {
            if (s.settings.schema == nullptr) {
                s.settings.schema = application.descriptor;
                s.settings.schema_wire_hints = application.hints;
            }
        }
        for (auto& s : effective_doc.acceptors) {
            if (s.settings.schema == nullptr) {
                s.settings.schema = application.descriptor;
                s.settings.schema_wire_hints = application.hints;
            }
        }
    }
    const bool queued = effective_doc.dispatch == nf::ConfigDispatch::queued_callbacks;
    return queued ? run_engine<nf::FlatSchema, nf::Queued>(effective_doc, args, std::move(engine_config))
                  : run_engine<nf::FlatSchema, nf::Inline>(effective_doc, args, std::move(engine_config));
}

int doctor(const Arguments &args) {
#if defined(NIMBLEFIX_ENABLE_XML)
    constexpr bool xml = true;
#else
    constexpr bool xml = false;
#endif
    const bool std_schemas = nf::cli_builtin::has_standard_schemas();
#if defined(NIMBLEFIX_ENABLE_BENCH)
    constexpr bool bench = true;
#else
    constexpr bool bench = false;
#endif
    if (!args.config.empty()) {
        const auto loaded = nf::load_config(args.config);
        if (!loaded.ok())
            return errors(loaded, args.json);
        ApplicationSchema schema;
        const auto checked = load_application_schema(*loaded.document, args, schema);
        if (checked != 0) return checked;
    }
    if (args.json)
        std::cout << "{\"version\":" << quote(version)
                  << ",\"tls\":" << (nf::tls_available() ? "true" : "false")
                  << ",\"io_uring\":" << (nf::io_uring_available() ? "true" : "false")
                  << ",\"xml\":" << (xml ? "true" : "false") << ",\"fix44\":" << (std_schemas ? "true" : "false")
                  << ",\"schemas\":" << (std_schemas ? "[\"fix42\",\"fix43\",\"fix44\",\"fix50sp2\"]" : "[]")
                  << ",\"benchmarks\":" << (bench ? "true" : "false")
                  << ",\"journal_format\":2,\"configuration_version\":1,\"source_api\":\"0.3.x\""
                  << ",\"confirmation\":\"appended\",\"dispatch\":[\"inline\",\"queued\"],\"io_backends\":[\"epoll\",\"busy_poll\""
                  << (nf::io_uring_available() ? ",\"io_uring\"" : "")
                  << "],\"platform\":\"Linux\"}\n";
    else
        std::cout << "NimbleFIX " << version << "\nTLS: " << (nf::tls_available() ? "OpenSSL" : "not built")
                  << "\nio_uring: " << (nf::io_uring_available() ? "liburing" : "not built")
                  << "\nXML: " << (xml ? "libxml2" : "not built")
                  << "\nSchemas: " << (std_schemas ? "fix42, fix43, fix44, fix50sp2" : "not built")
                  << "\nBenchmarks: " << (bench ? "built" : "not built") << "\nDispatch: inline, queued\nStore: mmap appended, v2\n";
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    try {
        Arguments args;
        if (!parse(argc, argv, args)) {
            help();
            return 2;
        }
        if (args.help) {
            help();
            return 0;
        }
        if (args.print_version) {
            std::cout << "NimbleFIX " << version << '\n';
            return 0;
        }
        if (args.command == "bench") return benchmark(args);
        if (args.command == "init")
            return init(args);
        if (args.command == "doctor")
            return doctor(args);
        if (args.command == "schema" && args.operation == "generate")
            return generate_schema(args);
        if (args.command == "inspect")
            return args.operation == "frame" ? inspect_frame(args) : inspect_journal(args);
        if (args.command == "telemetry") return show_telemetry(args);
        if (args.command == "replay") return replay_journal(args);
        if (args.command == "schema") return check_schema(args);
        const auto loaded = nf::load_config(args.config);
        if (!loaded.ok())
            return errors(loaded, args.json);
        if (args.command == "validate") {
            ApplicationSchema schema;
            const auto checked = load_application_schema(*loaded.document, args, schema);
            if (checked != 0) return checked;
            if (args.json)
                std::cout << "{\"valid\":true}\n";
            else
                std::cout << "Configuration valid (offline)\n";
            return 0;
        }
        if (args.command == "run")
            return run(*loaded.document, args);
        return 2;
    } catch (const std::exception &) {
        std::cerr << "operation failed; check paths, file permissions and resource limits\n";
        return 1;
    } catch (...) {
        std::cerr << "operation failed\n";
        return 1;
    }
}

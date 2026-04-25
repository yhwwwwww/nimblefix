# NimbleFIX Public API Guide

NimbleFIX exports two header trees:

- `include/public/nimblefix/`: supported external API
- `include/internal/nimblefix/`: repository-private implementation headers

External applications should add only `include/public/` to the include path.

## First Includes

Most applications start with these headers:

- `nimblefix/runtime/application.h`
- `nimblefix/runtime/config.h`
- `nimblefix/runtime/engine.h`
- `nimblefix/runtime/live_initiator.h`
- `nimblefix/runtime/live_acceptor.h`

Bring in these additional headers only when you need them:

- `nimblefix/session/session_handle.h`
- `nimblefix/session/session_send_envelope.h`
- `nimblefix/message/message_builder.h`
- `nimblefix/message/message_view.h`
- `nimblefix/profile/profile_loader.h`
- `nimblefix/store/memory_store.h`
- `nimblefix/store/mmap_store.h`
- `nimblefix/store/durable_batch_store.h`

## Lifecycle Contract

`Engine::Boot()` is the normal entry point. It validates `EngineConfig`, loads profiles from `profile_artifacts` and `profile_dictionaries`, optionally loads matching contract sidecars from `profile_contracts`, registers static counterparties, and makes `config()`, `profiles()`, `runtime()`, `FindCounterpartyConfig()`, and `FindListenerConfig()` available on success.

`Engine::LoadProfiles()` exists for tooling and tests that need profile loading without a booted runtime. Typical applications do not need to call it directly because `Boot()` already does.

The expected startup order is:

1. Fill `EngineConfig`.
2. Call `ValidateEngineConfig()` if you want an explicit preflight step.
3. Call `Engine::Boot()`.
4. Construct `LiveInitiator` or `LiveAcceptor` with the booted `Engine`.
5. Open sessions or listeners.
6. Call `Run()`.

## Minimal Initiator Walkthrough

This is the shortest normal path from config to a running initiator:

```cpp
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/message/message_builder.h"
#include "nimblefix/runtime/application.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/live_initiator.h"

class BuySideApp final : public nimble::runtime::ApplicationCallbacks {
public:
	auto OnSessionEvent(const nimble::runtime::RuntimeEvent& event) -> nimble::base::Status override {
		if (event.session_event != nimble::runtime::SessionEventKind::kActive) {
			return nimble::base::Status::Ok();
		}

		auto startup_probe = nimble::message::MessageBuilder{nimble::codec::msg_types::kTestRequest}
										 .set_string(nimble::codec::tags::kTestReqID, "startup-probe")
										 .build();
		return event.handle.SendTake(std::move(startup_probe));
	}
};

nimble::runtime::EngineConfig config;
config.profile_artifacts.push_back("fix44.nfa");
config.profile_contracts.push_back("fix44-contract.nfct");
config.counterparties.push_back(
	nimble::runtime::CounterpartyConfigBuilder::Initiator(
		"buy-side",
		1001U,
		nimble::session::SessionKey{ .sender_comp_id = "BUY1", .target_comp_id = "SELL1" },
		4400U,
		nimble::session::TransportVersion::kFix44)
		.contract_service_subsets({"order-entry"})
		.reconnect()
		.build());

auto validation = nimble::runtime::ValidateEngineConfig(config);
if (!validation.ok()) {
	return validation;
}

nimble::runtime::Engine engine;
auto boot = engine.Boot(config);
if (!boot.ok()) {
	return boot;
}

auto app = std::make_shared<BuySideApp>();
nimble::runtime::LiveInitiator initiator(&engine, { .application = app });

auto open = initiator.OpenSession(1001U, "127.0.0.1", 9876);
if (!open.ok()) {
	return open;
}

return initiator.Run();
```

Notes:

- `OpenSession()` may block until the TCP dial succeeds or times out.
- `OpenSessionAsync()` defers the dial onto the runtime worker loop if the caller cannot block.
- `SessionKey` is always written from the local engine's perspective. For initiators, `sender_comp_id` is your local `49` and `target_comp_id` is the remote `56`.
- `profile_contracts` is optional. Use it only for Orchestra-derived `.nfct` sidecars; `.nfa` remains the structural dictionary artifact and `.nfct` remains cold-path behavior metadata.
- `contract_service_subsets` is also optional. If you set it, the named subsets must exist in the loaded contract sidecar for the selected `profile_id`.
- `nimblefix/codec/fix_tags.h` exposes named FIX tag constants and common `MsgType` constants; public examples should prefer those names over raw values such as `35`, `112`, or `"1"`.

## Session Events And Send Boundaries

`ApplicationCallbacks` uses three session lifecycle events:

- `kBound`: Logon matched and the session is attached to a runtime worker, but replay or recovery may still be in progress.
- `kActive`: Recovery is complete and business traffic may start.
- `kClosed`: Transport is closed. Do not send from this event.

For first-time integrations, treat `kActive` as the safe point to send the first application message.

Send API rules:

- `SessionHandle::SendCopy()` / `SendTake()` / encoded owned variants use the session command queue and require a single producer thread per handle.
- `SessionHandle::SendInlineBorrowed()` / encoded inline-borrowed variants are valid only from direct runtime inline callbacks such as `OnSessionEvent`, `OnAdminMessage`, and `OnAppMessage`.
- Queue-drained application threads must use owned send variants, not inline-borrowed sends.

## Minimum Required Config

For a static initiator counterparty, the minimum meaningful fields are:

- `CounterpartyConfig::name`
- `session.session_id`
- `session.key.sender_comp_id`
- `session.key.target_comp_id`
- `session.profile_id`
- `transport_profile` / builder transport version
- `session.is_initiator = true`

For a static acceptor counterparty, the minimum meaningful fields are:

- `CounterpartyConfig::name`
- `session.session_id`
- `session.key.sender_comp_id` as the local acceptor CompID
- `session.key.target_comp_id` as the expected remote initiator CompID
- `session.profile_id`
- `session.is_initiator = false`

Additional conditional requirements:

- FIXT.1.1 sessions require `default_appl_ver_id`.
- `StoreMode::kMmap` and `StoreMode::kDurableBatch` require `store_path`.
- `RecoveryMode::kWarmRestart` requires a persistent store mode.
- `EngineConfig::listeners` is required before `LiveAcceptor::OpenListeners()`.

`CounterpartyConfigBuilder` and `ListenerConfigBuilder` exist to reduce boilerplate for the common cases above.

## Orchestra Contract Sidecars

When you import FIX Orchestra, keep the layering explicit:

- `.nfa` or `.nfd` still define only structure: fields, messages, groups, and validation tables used by the codec.
- `.nfct` sidecars define only behavior: conditional field rules, enum/code constraints, role-direction restrictions, service subsets, flow edges, and importer warnings.
- `EngineConfig::profile_contracts` loads sidecars on cold paths.
- `CounterpartyConfig::contract_service_subsets` selects which sidecar service subsets apply to one deployed session.

Unsupported Orchestra semantics are surfaced as importer warnings and contract introspection output. NimbleFIX does not parse Orchestra XML or interpret unsupported rules on the steady-state per-message hot path.

## Optional TLS Transport

TLS support has two separate switches:

- Build support: compile with `NIMBLEFIX_ENABLE_TLS=ON` in CMake or `--nimblefix_enable_tls=true` in xmake. This links OpenSSL and makes TLS code available.
- Runtime enablement: set `TlsClientConfig::enabled` or `TlsServerConfig::enabled`. Leaving those flags false keeps the connection on plain TCP even in a TLS-capable binary.

If the binary was built without TLS support, `ValidateEngineConfig()` rejects any runtime config with `enabled=true`, and direct TLS connection creation returns a clear error. It never silently falls back to plain TCP.

Initiator TLS is configured on the counterparty:

```cpp
nimble::runtime::TlsClientConfig tls;
tls.enabled = true;
tls.server_name = "fix.example.com";       // SNI
tls.expected_peer_name = "fix.example.com";
tls.ca_file = "/etc/nimblefix/ca.pem";
tls.min_version = nimble::runtime::TlsProtocolVersion::kTls12;

config.counterparties.push_back(
	nimble::runtime::CounterpartyConfigBuilder::Initiator(
		"buy-side-tls",
		1001U,
		nimble::session::SessionKey{ .sender_comp_id = "BUY1", .target_comp_id = "SELL1" },
		4400U)
		.tls_client(std::move(tls))
		.build());
```

Acceptor TLS is configured on the listener because TLS is negotiated before FIX Logon identifies a session:

```cpp
nimble::runtime::TlsServerConfig tls;
tls.enabled = true;
tls.certificate_chain_file = "/etc/nimblefix/server-chain.pem";
tls.private_key_file = "/etc/nimblefix/server-key.pem";
tls.ca_file = "/etc/nimblefix/client-ca.pem";       // Required for mTLS verification.
tls.verify_peer = true;
tls.require_client_certificate = true;

config.listeners.push_back(
	nimble::runtime::ListenerConfigBuilder::Named("tls-main")
		.bind("0.0.0.0", 9877)
		.tls_server(std::move(tls))
		.build());
```

For acceptor sessions, `CounterpartyConfig::acceptor_transport_security` can require a session to bind only over TLS or only over plain TCP. The runtime checks this against the actual accepted connection after Logon matching:

```cpp
config.counterparties.push_back(
	nimble::runtime::CounterpartyConfigBuilder::Acceptor(
		"sell-side-sensitive",
		2001U,
		nimble::session::SessionKey{ .sender_comp_id = "SELL1", .target_comp_id = "BUY1" },
		4400U)
		.acceptor_transport_security(nimble::runtime::TransportSecurityRequirement::kTlsOnly)
		.build());
```

TLS does not participate in `SessionKey`, worker routing, store keys, FIX sequence numbers, or profile selection. `verify_peer=false` is available for controlled test environments, but production configurations should keep peer verification enabled and provide CA material.

## Minimal Static Acceptor Walkthrough

```cpp
#include "nimblefix/runtime/application.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/live_acceptor.h"

class SellSideApp final : public nimble::runtime::ApplicationCallbacks {
public:
	auto OnSessionEvent(const nimble::runtime::RuntimeEvent& event) -> nimble::base::Status override {
		if (event.session_event == nimble::runtime::SessionEventKind::kActive) {
			// Session is live; start any business-level initialization here.
		}
		return nimble::base::Status::Ok();
	}
};

nimble::runtime::EngineConfig config;
config.profile_artifacts.push_back("fix44.nfa");
config.listeners.push_back(
	nimble::runtime::ListenerConfigBuilder::Named("main").bind("0.0.0.0", 9876).build());
config.counterparties.push_back(
	nimble::runtime::CounterpartyConfigBuilder::Acceptor(
		"sell-side",
		2001U,
		nimble::session::SessionKey{ .sender_comp_id = "SELL1", .target_comp_id = "BUY1" },
		4400U,
		nimble::session::TransportVersion::kFix44)
		.store(nimble::runtime::StoreMode::kDurableBatch, "/var/lib/nimblefix/sell-side")
		.build());

nimble::runtime::Engine engine;
auto boot = engine.Boot(config);
if (!boot.ok()) {
	return boot;
}

auto app = std::make_shared<SellSideApp>();
nimble::runtime::LiveAcceptor acceptor(&engine, { .application = app });

auto open = acceptor.OpenListeners("main");
if (!open.ok()) {
	return open;
}

return acceptor.Run();
```

## Dynamic Acceptor Walkthrough

Unknown inbound Logons are not dynamic by default. The rules are:

- `accept_unknown_sessions = false`: unknown Logons are rejected and `SessionFactory` is ignored.
- `accept_unknown_sessions = true` and no factory installed: unknown Logons are still rejected.
- `accept_unknown_sessions = true` and a factory installed: static counterparties match first, then the factory is called for unknown Logons.

Example:

```cpp
nimble::runtime::EngineConfig config;
config.profile_artifacts.push_back("fix44.nfa");
config.listeners.push_back(
	nimble::runtime::ListenerConfigBuilder::Named("main").bind("0.0.0.0", 9876).build());
config.accept_unknown_sessions = true;

nimble::runtime::Engine engine;
auto boot = engine.Boot(config);
if (!boot.ok()) {
	return boot;
}

engine.SetSessionFactory(
	[](const nimble::session::SessionKey& key) -> nimble::base::Result<nimble::runtime::CounterpartyConfig> {
		return nimble::runtime::CounterpartyConfigBuilder::Acceptor(
						 "dynamic-" + key.target_comp_id,
						 0U,
						 key,
						 4400U,
						 nimble::session::TransportVersion::kFix44)
			.validation_policy(nimble::session::ValidationPolicy::Permissive())
			.build();
	});
```

Dynamic onboarding contract:

- The factory input `SessionKey` is normalized to the local engine's perspective.
- `key.sender_comp_id` is the local acceptor CompID from inbound `TargetCompID(56)`.
- `key.target_comp_id` is the remote initiator CompID from inbound `SenderCompID(49)`.
- `listener` name, local port, and remote address are not part of the callback.
- Returning `session.session_id == 0` asks `Engine` to auto-assign a dynamic id from `kFirstDynamicSessionId` upward.
- `WhitelistSessionFactory::Allow(begin_string, local_sender_comp_id, template)` matches the same local-perspective key.

If you need routing decisions based on listener name, bound port, or remote address, the current public `SessionFactory` API does not expose that context. Use static counterparties or an external routing layer.

## SessionSendEnvelope And Header Tags

`SessionSendEnvelope` carries per-message session-managed header fields:

- `sender_sub_id` -> `SenderSubID(50)`
- `target_sub_id` -> `TargetSubID(57)`

Use the envelope when those values belong in the FIX session header. Do not rely on message body fields to populate `50/57` in the session header path.

## Support Headers

These headers remain public because public signatures depend on them or advanced integrations use them directly:

- `nimblefix/base/result.h`
- `nimblefix/base/status.h`
- `nimblefix/runtime/metrics.h`
- `nimblefix/runtime/profile_registry.h`
- `nimblefix/runtime/sharded_runtime.h`
- `nimblefix/runtime/trace.h`
- `nimblefix/session/encoded_application_message.h`
- `nimblefix/session/session_snapshot.h`
- `nimblefix/session/transport_profile.h`
- `nimblefix/session/validation_policy.h`

Everything under `include/internal/nimblefix/` remains repository-private.

## Do Not Do This

- Do not share one `SessionHandle` across multiple producer threads for sends. The runtime send path is single-producer.
- Do not default to `SendInlineBorrowed()` unless you are in a direct runtime inline callback and the borrowed payload lifetime is unquestionably safe.
- Do not assume `kBound` means the session is ready for business traffic. Wait for `kActive` unless you intentionally need pre-activation behavior.
- Do not call `Engine::LoadProfiles()` and then treat `Boot()` as a no-op. `Boot()` reloads profiles and resets runtime state.
- Do not assume `accept_unknown_sessions = true` is enough by itself. Unknown Logons still need a `SessionFactory`.
- Do not assume queue-decoupled handlers can use inline-borrowed sends. They cannot.
- Do not assume multi-listener dynamic routing can branch on listener name or port from `SessionFactory`; that context is not exposed today.

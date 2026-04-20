#include <catch2/catch_test_macros.hpp>

#include "nimblefix/base/status.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/session/session_key.h"

TEST_CASE("nimble namespace smoke", "[nimble][namespace]")
{
  nimble::base::Status status = nimble::base::Status::Ok();
  REQUIRE(status.ok());

  nimble::runtime::Engine engine;
  (void)engine;

  namespace nb = nimble;

  nb::session::SessionKey key{ "FIX.4.4", "BUY", "SELL" };
  REQUIRE(key.begin_string == "FIX.4.4");
  REQUIRE(key.sender_comp_id == "BUY");
  REQUIRE(key.target_comp_id == "SELL");
}
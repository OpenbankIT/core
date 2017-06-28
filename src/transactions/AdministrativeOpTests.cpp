// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "ledger/LedgerManager.h"
#include "main/Config.h"
#include "util/Timer.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "TxTests.h"
#include "ledger/LedgerDelta.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

// Merging when you are holding credit
// Merging when others are holding your credit
// Merging and then trying to set options in same ledger
// Merging with outstanding 0 balance trust lines
// Merging with outstanding offers
// Merge when you have outstanding data entries
TEST_CASE("administrative operation", "[tx][admin]")
{
    Config cfg(getTestConfig());

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    app.start();
    upgradeToCurrentLedgerVersion(app);

    // set up world
    SecretKey root = getRoot(app.getNetworkID());
    SecretKey a1 = getAccount("A");
	auto rootSeq = getAccountSeqNum(root, app) + 1;
	auto signer = Signer(a1.getPublicKey(), 100, SIGNER_ADMIN);
	applySetOptions(app, root, rootSeq++, nullptr, nullptr, nullptr, nullptr, &signer, nullptr);
	std::string data = "LongRandomData(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)(V) (;,,;) (V)";

	SECTION("success")
	{
		applyAdminOp(app, root, a1, data, rootSeq++);
	}
	SECTION("Empty data")
	{
		std::string emptyData = "";
		applyAdminOp(app, root, a1, emptyData, rootSeq++, ADMINISTRATIVE_MALFORMED);
	}
	SECTION("Source is not bank")
	{
		applyCreateAccountTx(app, root, a1, rootSeq++, 0);
		auto a1Seq = getAccountSeqNum(a1, app) + 1;
		applyAdminOp(app, a1, a1, data, a1Seq++, ADMINISTRATIVE_NOT_AUTHORIZED);
	}
	SECTION("Only admin can sign")
	{
		auto b1 = getAccount("b1");
		// emission
		signer = Signer(b1.getPublicKey(), 100, SIGNER_EMISSION);
		applySetOptions(app, root, rootSeq++, nullptr, nullptr, nullptr, nullptr, &signer, nullptr);
		applyAdminOp(app, root, b1, data, rootSeq++, ADMINISTRATIVE_NOT_AUTHORIZED);
		//general
		signer = Signer(b1.getPublicKey(), 100, SIGNER_EMISSION);
		applySetOptions(app, root, rootSeq++, nullptr, nullptr, nullptr, nullptr, &signer, nullptr);
		applyAdminOp(app, root, b1, data, rootSeq++, ADMINISTRATIVE_NOT_AUTHORIZED);
	}
}

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
#include "crypto/SHA.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

// Merging when you are holding credit
// Merging when others are holding your credit
// Merging and then trying to set options in same ledger
// Merging with outstanding 0 balance trust lines
// Merging with outstanding offers
// Merge when you have outstanding data entries
TEST_CASE("Payment reversal", "[tx][payment_reversal]")
{
	Config cfg = getTestConfig();
	auto commissionPass = "(VVV)(^,,,^)(VVV)";
	auto commissionAccount = SecretKey::fromSeed(sha256(commissionPass));
	cfg.BANK_COMMISSION_KEY = commissionAccount.getPublicKey();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    app.start();
    upgradeToCurrentLedgerVersion(app);

	const int64 paymentAmount = 1000;
	const int64 commissionAmount = 100;

    // set up world
    SecretKey root = getRoot(app.getNetworkID());
	auto rootSeq = getAccountSeqNum(root, app) + 1;

	SecretKey sk = getAccount("admin_signer");
	auto signer = Signer(sk.getPublicKey(), 100, SIGNER_ADMIN);
	applySetOptions(app, root, rootSeq++, nullptr, nullptr, nullptr, nullptr, &signer, nullptr);
	auto commissionSeq = getAccountSeqNum(commissionAccount, app) + 1;
	applySetOptions(app, commissionAccount, commissionSeq++, nullptr, nullptr, nullptr, nullptr, &signer, nullptr);

	// create an account
	auto agent = getAccount("agent");
	applyCreateAccountTx(app, root, agent, rootSeq++, 0, &sk, CREATE_ACCOUNT_SUCCESS, AccountType::ACCOUNT_SETTLEMENT_AGENT);
	auto agentSeq = getAccountSeqNum(agent, app) + 1;

	auto account = getAccount("account");
	applyCreateAccountTx(app, root, account, rootSeq++, 0, &sk, CREATE_ACCOUNT_SUCCESS, AccountType::ACCOUNT_REGISTERED_USER);
	auto accountSeq = getAccountSeqNum(account, app) + 1;

	auto usd = makeAsset(root, "USD");
	// send some assets to agent
	applyChangeTrust(app, agent, root, agentSeq++, "USD", INT64_MAX);
	OperationFee fee;
	fee.type(OperationFeeType::opFEE_CHARGED);
	fee.fee().asset = usd;
	fee.fee().flatFee.activate() = commissionAmount;
	fee.fee().amountToCharge = commissionAmount;
	applyCreditPaymentTx(app, root, agent, usd, rootSeq++, paymentAmount, &sk, &fee);
	auto agentLine = loadTrustLine(agent, usd, app, true);
	REQUIRE(agentLine->getBalance() == paymentAmount - commissionAmount);
	auto commissionLine = loadTrustLine(commissionAccount, usd, app, true);
	REQUIRE(commissionLine->getBalance() == commissionAmount);

	int64 paymentID = rand();

	SECTION("Malformed payment reversal")
	{
		SECTION("Invalid amount")
		{
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, usd, -paymentAmount, commissionAmount, PAYMENT_REVERSAL_MALFORMED);
		}
		SECTION("Invalid commission amount")
		{
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, usd, paymentAmount, -commissionAmount, PAYMENT_REVERSAL_MALFORMED);
		}
		SECTION("Invalid asset")
		{
			Asset native;
			native.type(ASSET_TYPE_NATIVE);
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, native, paymentAmount, commissionAmount, PAYMENT_REVERSAL_MALFORMED);
		}
	}
	SECTION("Commission")
	{
		SECTION("Commission trust line does not exists")
		{
			auto eur = makeAsset(root, "EUR");
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, eur, paymentAmount, commissionAmount, PAYMENT_REVERSAL_COMMISSION_UNDERFUNDED);
		}
		SECTION("Commission account is underfunded")
		{
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, usd, paymentAmount, commissionAmount + 1, PAYMENT_REVERSAL_COMMISSION_UNDERFUNDED);
		}
	}
	SECTION("Source")
	{
		SECTION("Trust line does not exists")
		{
			auto eur = makeAsset(root, "EUR");
			applyChangeTrust(app, commissionAccount, root, commissionSeq++, "EUR", INT64_MAX, CHANGE_TRUST_SUCCESS, &sk);
			applyCreditPaymentTx(app, root, commissionAccount, eur, rootSeq++, commissionAmount, &sk, nullptr);
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, eur, paymentAmount, commissionAmount, PAYMENT_REVERSAL_SRC_NO_TRUST);

		}
		SECTION("Source line underfunded")
		{
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, usd, paymentAmount + 1, commissionAmount, PAYMENT_REVERSAL_UNDERFUNDED);
		}
	}
	SECTION("Payment sender")
	{
		SECTION("Does not exists")
		{
			auto randomAccount = SecretKey::random();
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, randomAccount, usd, paymentAmount, commissionAmount, PAYMENT_REVERSAL_NO_PAYMENT_SENDER);
		}
		SECTION("Line is full")
		{
			applyChangeTrust(app, account, root, accountSeq++, "USD", paymentAmount - 1);
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, usd, paymentAmount, commissionAmount, PAYMENT_REVERSAL_PAYMENT_SENDER_LINE_FULL);
		}
	}
	SECTION("Success")
	{
		// check prebalances
		commissionLine = loadTrustLine(commissionAccount, usd, app, true);
		REQUIRE(commissionLine->getBalance() == commissionAmount);
		agentLine = loadTrustLine(agent, usd, app, true);
		REQUIRE(agentLine->getBalance() == paymentAmount - commissionAmount);
		applyChangeTrust(app, account, root, accountSeq++, "USD", INT64_MAX);
		auto accountLine = loadTrustLine(account, usd, app, true);
		REQUIRE(accountLine->getBalance() == 0);
		// apply reversal
		applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, usd, paymentAmount, commissionAmount);
		// check postbalances
		commissionLine = loadTrustLine(commissionAccount, usd, app, true);
		REQUIRE(commissionLine->getBalance() == 0);
		agentLine = loadTrustLine(agent, usd, app, true);
		REQUIRE(agentLine->getBalance() == 0);
		accountLine = loadTrustLine(account, usd, app, true);
		REQUIRE(accountLine->getBalance() == paymentAmount);
		SECTION("Already reversed")
		{
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, usd, paymentAmount, commissionAmount, PAYMENT_REVERSAL_ALREADY_REVERSED);
		}
	}
}

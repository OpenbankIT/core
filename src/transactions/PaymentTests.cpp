// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "main/Application.h"
#include "util/Timer.h"
#include "main/Config.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "TxTests.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerDelta.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "crypto/SHA.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

// *XLM Payment
// *Credit Payment
// XLM -> Credit Payment
// Credit -> XLM Payment
// Credit -> XLM -> Credit Payment
// Credit -> Credit -> Credit -> Credit Payment
// path payment where there isn't enough in the path
// path payment with a transfer rate
TEST_CASE("payment", "[tx][payment]")
{
    Config cfg = getTestConfig();
	auto commissionPass = "(V)(^,,,^)(V)";
	auto commissionSeed = SecretKey::fromSeed(sha256(commissionPass));
	cfg.BANK_COMMISSION_KEY = commissionSeed.getPublicKey();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    app.start();

    // set up world
    SecretKey root = getRoot(app.getNetworkID());
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");

    SecretKey sk = getAccount("admin_signer");

    const int64_t paymentAmount = 100;
    auto signer = Signer(sk.getPublicKey(), 100, SIGNER_ADMIN);
    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;
    applySetOptions(app, root, rootSeq++, nullptr, nullptr, nullptr, nullptr, &signer, nullptr);
    
    // create an account
    applyCreateAccountTx(app, root, a1, rootSeq++, 0, &sk);

    SequenceNumber a1Seq = getAccountSeqNum(a1, app) + 1;

    const int64_t morePayment = paymentAmount / 2;

    const int64_t assetMultiplier = 10000000;

    int64_t trustLineLimit = INT64_MAX;

    int64_t trustLineStartingBalance = 20000 * assetMultiplier;

    Asset idrCur = makeAsset(root, "IDR");
    Asset usdCur = makeAsset(root, "USD");
	Asset eurCur = makeAsset(root, "EUR");

    AccountFrame::pointer a1Account, rootAccount;
    rootAccount = loadAccount(root, app);
    a1Account = loadAccount(a1, app);
    REQUIRE(rootAccount->getMasterWeight() == 1);
    REQUIRE(rootAccount->getHighThreshold() == 0);
    REQUIRE(rootAccount->getLowThreshold() == 0);
    REQUIRE(rootAccount->getMediumThreshold() == 0);
    REQUIRE(a1Account->getMasterWeight() == 1);
    REQUIRE(a1Account->getHighThreshold() == 0);
    REQUIRE(a1Account->getLowThreshold() == 0);
    REQUIRE(a1Account->getMediumThreshold() == 0);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
	SECTION("payment") 
	{
		auto newAccount = getAccount("newAccount");
		OperationFee fee;
		fee.type(OperationFeeType::opFEE_CHARGED);
		fee.fee().amountToCharge = paymentAmount / 2;
		fee.fee().asset = idrCur;
		SECTION("invalid asset")
		{
			auto a1Asset = makeAsset(a1, "AAA");
			fee.fee().asset = a1Asset;
			applyCreditPaymentTx(app, a1, newAccount, a1Asset, a1Seq++, paymentAmount, nullptr, &fee, PAYMENT_MALFORMED);
		}
		// create an account
		applyCreateAccountTx(app, root, b1, rootSeq++, 0, &sk);
		applyChangeTrust(app, a1, root, a1Seq++, "IDR", INT64_MAX);
		applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++, paymentAmount, &sk);
        
        
        //TODO: due to txBAD_AUTH tx result we don't have a transaction result, and can't check using existing methods
//        SECTION("using root key to sign regular tx")
//        {
//            applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++, paymentAmount, nullptr, nullptr, ???txBAD_AUTH???);
//        }
        
        SECTION("balance confirmations")
        {
            TrustFrame::pointer line;
            line = loadTrustLine(a1, idrCur, app);
            REQUIRE(line->getBalance() == paymentAmount);
            
            line = loadTrustLine(root, idrCur, app);
            REQUIRE(line->getBalance() == -paymentAmount);
            
        }
        
		auto b1Seq = getAccountSeqNum(b1, app) + 1;
		applyChangeTrust(app, b1, root, b1Seq++, "IDR", INT64_MAX);

		SECTION("underfunded")
		{
			applyCreditPaymentTx(app, a1, b1, idrCur, a1Seq++, paymentAmount*2, nullptr, nullptr, PAYMENT_UNDERFUNDED);
		}
		SECTION("PAYMENT_SRC_NO_TRUST") 
		{
			applyChangeTrust(app, b1, root, b1Seq++, "USD", INT64_MAX);
			applyCreditPaymentTx(app, a1, b1, usdCur, a1Seq++, paymentAmount, nullptr, nullptr, PAYMENT_SRC_NO_TRUST);
		}
//		SECTION("PAYMENT_NO_TRUST")
//		{
//			applyCreateAccountTx(app, root, newAccount, rootSeq++, 0, &sk);
//			applyCreditPaymentTx(app, a1, newAccount, idrCur, a1Seq++, paymentAmount, nullptr, nullptr, PAYMENT_NO_TRUST);
//		}
		SECTION("PAYMENT_LINE_FULL")
		{
			applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++, INT64_MAX, &sk, &fee, PAYMENT_LINE_FULL);
		}
		
	}
    SECTION("Create account")
    {
        SECTION("Success")
        {
            applyCreateAccountTx(app, root, b1, rootSeq++, 0, &sk);
            SECTION("Account already exists")
            {
                applyCreateAccountTx(app, root, b1, rootSeq++, 0, &sk, CREATE_ACCOUNT_ALREADY_EXIST);
            }
        }
		SECTION("Scratch card")
		{
			auto distr = getAccount("distr");
			applyCreateAccountTx(app, root, distr, rootSeq++, 0, &sk, CREATE_ACCOUNT_SUCCESS, ACCOUNT_DISTRIBUTION_AGENT);
			auto distrSeq = getAccountSeqNum(distr, app) + 1;
			SECTION("Only can ACCOUNT_DISTRIBUTION_AGENT create account")
			{
				auto account = SecretKey::random();
				applyCreateAccountTx(app, root, account, rootSeq++, 100, &sk, CREATE_ACCOUNT_WRONG_TYPE, ACCOUNT_SCRATCH_CARD, &usdCur);
			}
			SECTION("Invalid amount")
			{
				auto account = SecretKey::random();
				applyCreateAccountTx(app, distr, account, distrSeq++, 0, nullptr, CREATE_ACCOUNT_MALFORMED, ACCOUNT_SCRATCH_CARD);
			}
			SECTION("Invalid asset")
			{
				auto account = SecretKey::random();
				auto invalidCur = makeAsset(distr, "USD");
				applyCreateAccountTx(app, distr, account, distrSeq++, 100, nullptr, CREATE_ACCOUNT_MALFORMED, ACCOUNT_SCRATCH_CARD, &invalidCur);
			}
			SECTION("Success")
			{
				int64 amount = 100000;
				auto account = SecretKey::random();
                applyChangeTrust(app, distr, root, distrSeq++, "USD", INT64_MAX);
                applyCreditPaymentTx(app, root, distr, usdCur, rootSeq++, 200+amount, &sk);
				applyCreateAccountTx(app, distr, account, distrSeq++, amount, nullptr, CREATE_ACCOUNT_SUCCESS, ACCOUNT_SCRATCH_CARD, &usdCur);
				auto loadedAccount = loadAccount(account, app);
				REQUIRE(loadedAccount);
				auto accountLine = loadTrustLine(account, usdCur, app, true);
				REQUIRE(accountLine);
				REQUIRE(accountLine->getBalance() == amount);
				SECTION("Can't send more to scratch card")
				{
					applyCreditPaymentTx(app, root, account, usdCur, rootSeq++, 100, &sk, nullptr, PAYMENT_NO_DESTINATION);
				}
				SECTION("Can spend, can't deposit")
				{
					auto accountSeq = getAccountSeqNum(account, app) + 1;
					applyCreditPaymentTx(app, account, root, usdCur, accountSeq++, amount / 2);
					accountLine = loadTrustLine(account, usdCur, app, true);
					REQUIRE(accountLine);
					REQUIRE(accountLine->getBalance() == amount / 2);
					// can't deposit
					applyCreditPaymentTx(app, root, account, usdCur, rootSeq++, 100, &sk, nullptr, PAYMENT_NO_DESTINATION);
				}
			}
		}
    }

    SECTION("issuer large amounts")
    {
        applyChangeTrust(app, a1, root, a1Seq++, "IDR", INT64_MAX);
        applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++,
                             INT64_MAX, &sk);
        TrustFrame::pointer line;
        line = loadTrustLine(a1, idrCur, app);
        REQUIRE(line->getBalance() == INT64_MAX);

        // send it all back
        applyCreditPaymentTx(app, a1, root, idrCur, a1Seq++, INT64_MAX);
        line = loadTrustLine(a1, idrCur, app);
        REQUIRE(line->getBalance() == 0);

        std::vector<TrustFrame::pointer> gwLines;
        TrustFrame::loadLines(root.getPublicKey(), gwLines,
                              app.getDatabase());
        REQUIRE(gwLines.size() == 1);
    }
    SECTION("authorize flag")
    {
        uint32_t setFlags = AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG;

        applySetOptions(app, root, rootSeq++, nullptr, &setFlags,
                        nullptr, nullptr, nullptr, nullptr);

        applyChangeTrust(app, a1, root, a1Seq++, "IDR", trustLineLimit);

        applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++,
                             trustLineStartingBalance, &sk, nullptr, PAYMENT_NOT_AUTHORIZED);

        applyAllowTrust(app, root, a1, rootSeq++, "IDR", true, &sk);

        applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++,
                             trustLineStartingBalance, &sk);

        // send it all back
        applyAllowTrust(app, root, a1, rootSeq++, "IDR", false, &sk);

        applyCreditPaymentTx(app, a1, root, idrCur, a1Seq++,
                             trustLineStartingBalance, nullptr, nullptr,
                             PAYMENT_SRC_NOT_AUTHORIZED);

        applyAllowTrust(app, root, a1, rootSeq++, "IDR", true, &sk);

        applyCreditPaymentTx(app, a1, root, idrCur, a1Seq++,
                             trustLineStartingBalance);
    }
	SECTION("Invalid issuer") {
		applyChangeTrust(app, a1, b1, a1Seq++, "USD", INT64_MAX, CHANGE_TRUST_MALFORMED);
		auto invalidCur = makeAsset(b1, "USD");
		applyCreditPaymentTx(app, a1, b1, invalidCur, a1Seq++, 123, nullptr, nullptr, PAYMENT_MALFORMED);
	}
    SECTION("payment through path")
    {
        SECTION("send EUR with path (not enough offers)")
        {
			applyChangeTrust(app, a1, root, a1Seq++, "EUR", trustLineLimit);
            applyPathPaymentTx(app, root, a1, idrCur, morePayment * 10,
                               eurCur, morePayment, rootSeq++, &sk, nullptr,
                               PATH_PAYMENT_TOO_FEW_OFFERS);
        }

        // setup a1
        applyChangeTrust(app, a1, root, a1Seq++, "USD", trustLineLimit);
        applyChangeTrust(app, a1, root, a1Seq++, "IDR", trustLineLimit);

        applyCreditPaymentTx(app, root, a1, usdCur, rootSeq++,
                             trustLineStartingBalance, &sk);

        // add a couple offers in the order book

        OfferFrame::pointer offer;

        const Price usdPriceOffer(2, 1);

        applyCreateAccountTx(app, root, b1, rootSeq++, 0, &sk);
        SequenceNumber b1Seq = getAccountSeqNum(b1, app) + 1;
        applyChangeTrust(app, b1, root, b1Seq++, "USD", trustLineLimit);
        applyChangeTrust(app, b1, root, b1Seq++, "IDR", trustLineLimit);

        applyCreditPaymentTx(app, root, b1, idrCur, rootSeq++,
                             trustLineStartingBalance, &sk);

        uint64_t offerB1 =
            applyCreateOffer(app, delta, 0, b1, idrCur, usdCur, usdPriceOffer,
                             100 * assetMultiplier, b1Seq++);

        // setup "c1"
        SecretKey c1 = getAccount("C");

        applyCreateAccountTx(app, root, c1, rootSeq++, 0, &sk);
        SequenceNumber c1Seq = getAccountSeqNum(c1, app) + 1;

        applyChangeTrust(app, c1, root, c1Seq++, "USD", trustLineLimit);
        applyChangeTrust(app, c1, root, c1Seq++, "IDR", trustLineLimit);

        applyCreditPaymentTx(app, root, c1, idrCur, rootSeq++,
                             trustLineStartingBalance, &sk);

        // offer is sell 100 IDR for 150 USD ; buy USD @ 1.5 = sell IRD @ 0.66
        uint64_t offerC1 =
            applyCreateOffer(app, delta, 0, c1, idrCur, usdCur, Price(3, 2),
                             100 * assetMultiplier, c1Seq++);

        // at this point:
        // a1 holds (0, IDR) (trustLineStartingBalance, USD)
        // b1 holds (trustLineStartingBalance, IDR) (0, USD)
        // c1 holds (trustLineStartingBalance, IDR) (0, USD)
        SECTION("send with path (over sendmax)")
        {
            // A1: try to send 100 IDR to B1
            // using 149 USD

            auto res = applyPathPaymentTx(
                app, a1, b1, usdCur, 149 * assetMultiplier, idrCur,
                100 * assetMultiplier, a1Seq++, nullptr, nullptr, PATH_PAYMENT_OVER_SENDMAX);
        }

        SECTION("send with path (success)")
        {
            // A1: try to send 125 IDR to B1 using USD
            // should cost 150 (C's offer taken entirely) +
            //  50 (1/4 of B's offer)=200 USD

            auto res = applyPathPaymentTx(
                app, a1, b1, usdCur, 250 * assetMultiplier, idrCur,
                125 * assetMultiplier, a1Seq++, nullptr, nullptr, PATH_PAYMENT_SUCCESS);

            auto const& multi = res.success();

            REQUIRE(multi.offers.size() == 2);

            TrustFrame::pointer line;

            // C1
            // offer was taken
            REQUIRE(multi.offers[0].offerID == offerC1);
            REQUIRE(!loadOffer(c1, offerC1, app, false));
            line = loadTrustLine(c1, idrCur, app);
            checkAmounts(line->getBalance(),
                         trustLineStartingBalance - 100 * assetMultiplier);
            line = loadTrustLine(c1, usdCur, app);
            checkAmounts(line->getBalance(), 150 * assetMultiplier);

            // B1
            auto const& b1Res = multi.offers[1];
            REQUIRE(b1Res.offerID == offerB1);
            offer = loadOffer(b1, offerB1, app);
            OfferEntry const& oe = offer->getOffer();
            REQUIRE(b1Res.sellerID == b1.getPublicKey());
            checkAmounts(b1Res.amountSold, 25 * assetMultiplier);
            checkAmounts(oe.amount, 75 * assetMultiplier);
            line = loadTrustLine(b1, idrCur, app);
            // 125 where sent, 25 were consumed by B's offer
            checkAmounts(line->getBalance(), trustLineStartingBalance +
                                                 (125 - 25) * assetMultiplier);
            line = loadTrustLine(b1, usdCur, app);
            checkAmounts(line->getBalance(), 50 * assetMultiplier);

            // A1
            line = loadTrustLine(a1, idrCur, app);
            checkAmounts(line->getBalance(), 0);
            line = loadTrustLine(a1, usdCur, app);
            checkAmounts(line->getBalance(),
                         trustLineStartingBalance - 200 * assetMultiplier);
        }

        SECTION("send with path (takes own offer)")
        {
            // raise A1's balance by what we're trying to send
            applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++, 100 * assetMultiplier, &sk);

            // offer is sell 100 USD for 100 IDR
            applyCreateOffer(app, delta, 0, a1, usdCur, idrCur, Price(1, 1),
                             100 * assetMultiplier, a1Seq++);

            // A1: try to send 100 USD to B1 using IDR

            applyPathPaymentTx(app, a1, b1, idrCur, 100 * assetMultiplier,
                               usdCur, 100 * assetMultiplier, a1Seq++, nullptr, nullptr,
                               PATH_PAYMENT_OFFER_CROSS_SELF);
        }

        SECTION("send with path (offer participant reaching limit)")
        {
            // make it such that C can only receive 120 USD (4/5th of offerC)
            applyChangeTrust(app, c1, root, c1Seq++, "USD",
                             120 * assetMultiplier);

            // A1: try to send 105 IDR to B1 using USD
            // cost 120 (C's offer maxed out at 4/5th of published amount)
            //  50 (1/4 of B's offer)=170 USD

            auto res = applyPathPaymentTx(
                app, a1, b1, usdCur, 400 * assetMultiplier, idrCur,
                105 * assetMultiplier, a1Seq++, nullptr, nullptr, PATH_PAYMENT_SUCCESS);

            auto& multi = res.success();

            REQUIRE(multi.offers.size() == 2);

            TrustFrame::pointer line;

            // C1
            // offer was taken
            REQUIRE(multi.offers[0].offerID == offerC1);
            REQUIRE(!loadOffer(c1, offerC1, app, false));
            line = loadTrustLine(c1, idrCur, app);
            checkAmounts(line->getBalance(),
                         trustLineStartingBalance - 80 * assetMultiplier);
            line = loadTrustLine(c1, usdCur, app);
            checkAmounts(line->getBalance(), line->getTrustLine().limit);

            // B1
            auto const& b1Res = multi.offers[1];
            REQUIRE(b1Res.offerID == offerB1);
            offer = loadOffer(b1, offerB1, app);
            OfferEntry const& oe = offer->getOffer();
            REQUIRE(b1Res.sellerID == b1.getPublicKey());
            checkAmounts(b1Res.amountSold, 25 * assetMultiplier);
            checkAmounts(oe.amount, 75 * assetMultiplier);
            line = loadTrustLine(b1, idrCur, app);
            // 105 where sent, 25 were consumed by B's offer
            checkAmounts(line->getBalance(), trustLineStartingBalance +
                                                 (105 - 25) * assetMultiplier);
            line = loadTrustLine(b1, usdCur, app);
            checkAmounts(line->getBalance(), 50 * assetMultiplier);

            // A1
            line = loadTrustLine(a1, idrCur, app);
            checkAmounts(line->getBalance(), 0);
            line = loadTrustLine(a1, usdCur, app);
            checkAmounts(line->getBalance(),
                         trustLineStartingBalance - 170 * assetMultiplier);
        }
        SECTION("missing trust line")
        {
            // modify C's trustlines to invalidate C's offer
            // * C's offer should be deleted
            // sell 100 IDR for 200 USD
            // * B's offer 25 IDR by 50 USD

            auto checkBalances = [&]()
            {
                auto res = applyPathPaymentTx(
                    app, a1, b1, usdCur, 200 * assetMultiplier, idrCur,
                    25 * assetMultiplier, a1Seq++, nullptr, nullptr, PATH_PAYMENT_SUCCESS);

                auto& multi = res.success();

                REQUIRE(multi.offers.size() == 2);

                TrustFrame::pointer line;

                // C1
                // offer was deleted
                REQUIRE(multi.offers[0].offerID == offerC1);
                REQUIRE(multi.offers[0].amountSold == 0);
                REQUIRE(multi.offers[0].amountBought == 0);
                REQUIRE(!loadOffer(c1, offerC1, app, false));

                // B1
                auto const& b1Res = multi.offers[1];
                REQUIRE(b1Res.offerID == offerB1);
                offer = loadOffer(b1, offerB1, app);
                OfferEntry const& oe = offer->getOffer();
                REQUIRE(b1Res.sellerID == b1.getPublicKey());
                checkAmounts(b1Res.amountSold, 25 * assetMultiplier);
                checkAmounts(oe.amount, 75 * assetMultiplier);
                line = loadTrustLine(b1, idrCur, app);
                // As B was the sole participant in the exchange, the IDR
                // balance should not have changed
                checkAmounts(line->getBalance(), trustLineStartingBalance);
                line = loadTrustLine(b1, usdCur, app);
                // but 25 USD cost 50 USD to send
                checkAmounts(line->getBalance(), 50 * assetMultiplier);

                // A1
                line = loadTrustLine(a1, idrCur, app);
                checkAmounts(line->getBalance(), 0);
                line = loadTrustLine(a1, usdCur, app);
                checkAmounts(line->getBalance(),
                             trustLineStartingBalance - 50 * assetMultiplier);
            };

            SECTION("deleted selling line")
            {
                applyCreditPaymentTx(app, c1, root, idrCur, c1Seq++,
                                     trustLineStartingBalance);

                applyChangeTrust(app, c1, root, c1Seq++, "IDR", 0);

                checkBalances();
            }

            SECTION("deleted buying line")
            {
                applyChangeTrust(app, c1, root, c1Seq++, "USD", 0);
                checkBalances();
            }
        }
    }
	SECTION("fee") {
		applyCreateAccountTx(app, root, b1, rootSeq++, 0, &sk);
		auto b1Seq = getAccountSeqNum(b1, app) + 1;
		applyChangeTrust(app, a1, root, a1Seq++, "USD", INT64_MAX);
		applyChangeTrust(app, b1, root, b1Seq++, "USD", INT64_MAX);
		applyCreditPaymentTx(app, root, a1, usdCur, rootSeq++, paymentAmount, &sk);

		SECTION("invalid fee") {
			SECTION("fee is not set") {
				auto paymentFrame = createCreditPaymentTx(app.getNetworkID(), a1, b1, usdCur, a1Seq++, paymentAmount);
				TransactionEnvelope env = paymentFrame->getEnvelope();
				env.operationFees.clear();
				auto payment = std::make_shared<TransactionFrame>(app.getNetworkID(), env);
				applyCheck(payment, delta, app);
				REQUIRE(payment->getResultCode() == txINTERNAL_ERROR);
			}
			SECTION("Fee is too big") {
				auto paymentFrame = createCreditPaymentTx(app.getNetworkID(), a1, b1, usdCur, a1Seq++, paymentAmount);
				TransactionEnvelope env = paymentFrame->getEnvelope();
				OperationFee fee;
				fee.type(OperationFeeType::opFEE_NONE);
				env.operationFees.push_back(fee);
				auto payment = std::make_shared<TransactionFrame>(app.getNetworkID(), env);
				applyCheck(payment, delta, app);
				REQUIRE(payment->getResultCode() == txINTERNAL_ERROR);
			}
			SECTION("fee - invalid asset") {
				OperationFee fee;
				fee.type(OperationFeeType::opFEE_CHARGED);
				fee.fee().amountToCharge = paymentAmount / 2;
				applyCreditPaymentTx(app, a1, b1, usdCur, a1Seq++, paymentAmount, nullptr, &fee, PAYMENT_MALFORMED);
			}
			SECTION("negative fee") {
				OperationFee fee;
				fee.type(OperationFeeType::opFEE_CHARGED);
				fee.fee().amountToCharge = -2;
				applyCreditPaymentTx(app, a1, b1, usdCur, a1Seq++, paymentAmount, nullptr, &fee, PAYMENT_MALFORMED);
			}
			SECTION("fee greater than amount") {
				OperationFee fee;
				fee.type(OperationFeeType::opFEE_CHARGED);
				fee.fee().amountToCharge = paymentAmount * 2;
				applyCreditPaymentTx(app, a1, b1, usdCur, a1Seq++, paymentAmount, nullptr, &fee, PAYMENT_MALFORMED);
			}
			SECTION("issuer large amounts")
			{
				applyChangeTrust(app, a1, root, a1Seq++, "EUR", INT64_MAX);
				auto paymentAmount = INT64_MAX;
				OperationFee fee;
				fee.type(OperationFeeType::opFEE_CHARGED);
				fee.fee().amountToCharge = paymentAmount / 10;
				fee.fee().asset = eurCur;
				applyCreditPaymentTx(app, root, a1, eurCur, rootSeq++, INT64_MAX, &sk, &fee);
				auto line = loadTrustLine(a1, eurCur, app);
				auto balance = INT64_MAX - fee.fee().amountToCharge;
				REQUIRE(line->getBalance() == balance);

				auto commissionLine = loadTrustLine(commissionSeed, eurCur, app);
				REQUIRE(commissionLine);
				REQUIRE(commissionLine->getBalance() == fee.fee().amountToCharge);

				// send it all back
				applyCreditPaymentTx(app, a1, root, eurCur, a1Seq++, balance);
				line = loadTrustLine(a1, eurCur, app);
				REQUIRE(line->getBalance() == 0);
			}
			SECTION("success path payment") {
				applyChangeTrust(app, a1, root, a1Seq++, "IDR", INT64_MAX);
				applyChangeTrust(app, b1, root, b1Seq++, "IDR", INT64_MAX);
				applyCreditPaymentTx(app, root, b1, idrCur, rootSeq++, paymentAmount, &sk);
				applyCreateOffer(app, delta, 0, b1, idrCur, usdCur, Price(1, 1), paymentAmount, b1Seq++);

				auto a1Line = loadTrustLine(a1, usdCur, app, false);
				auto oldBalance = a1Line->getBalance();
				OperationFee fee;
				fee.type(OperationFeeType::opFEE_CHARGED);
				fee.fee().amountToCharge = paymentAmount / 2;
				fee.fee().asset = idrCur;
				applyPathPaymentTx(app, a1, b1, usdCur, paymentAmount, idrCur, paymentAmount, a1Seq++, nullptr, &fee);
				
				auto b1Line = loadTrustLine(b1, idrCur, app);
				REQUIRE(b1Line->getBalance() == paymentAmount - fee.fee().amountToCharge);

				a1Line = loadTrustLine(a1, usdCur, app, false);
				REQUIRE((a1Line->getBalance() == (oldBalance - paymentAmount)));

				auto commLine = loadTrustLine(commissionSeed, idrCur, app);
				REQUIRE((commLine->getBalance() == fee.fee().amountToCharge));
			}
			SECTION("success account creation") {
				auto anonUser = getAccount("anonUser");
				auto aUAH = makeAsset(root, "AUAH");
				auto oldBalance = 0;
				OperationFee fee;
				fee.type(OperationFeeType::opFEE_CHARGED);
				fee.fee().amountToCharge = paymentAmount / 2;
				fee.fee().asset = aUAH;
				applyCreditPaymentTx(app, root, anonUser, aUAH, rootSeq++, paymentAmount, &sk, &fee);

				auto anonAccount = loadAccount(anonUser, app);
				REQUIRE(anonAccount->getAccount().accountType == ACCOUNT_ANONYMOUS_USER);

				auto anonLine = loadTrustLine(anonUser, aUAH, app);
				REQUIRE(anonLine->getBalance() == paymentAmount - fee.fee().amountToCharge);

				auto commLine = loadTrustLine(commissionSeed, aUAH, app);
				REQUIRE((commLine->getBalance() == fee.fee().amountToCharge));

				auto anonSeq = getAccountSeqNum(anonUser, app) + 1;
				applyCreditPaymentTx(app, anonUser, root, aUAH, anonSeq++, paymentAmount - fee.fee().amountToCharge, nullptr);
			}
		}
	}
}

TEST_CASE("single create account SQL", "[singlesql][paymentsql][hide]")
{
    Config::TestDbMode mode = Config::TESTDB_ON_DISK_SQLITE;
#ifdef USE_POSTGRES
    if (!force_sqlite)
        mode = Config::TESTDB_POSTGRESQL;
#endif

    VirtualClock clock;
    Application::pointer app =
        Application::create(clock, getTestConfig(0, mode));
    app->start();

    SecretKey root = getRoot(app->getNetworkID());
    SecretKey a1 = getAccount("A");
    int64_t txfee = app->getLedgerManager().getTxFee();
    const int64_t paymentAmount =
        app->getLedgerManager().getMinBalance(1) + txfee * 10;

    SequenceNumber rootSeq = getAccountSeqNum(root, *app) + 1;

    {
        auto ctx = app->getDatabase().captureAndLogSQL("createAccount");
        applyCreateAccountTx(*app, root, a1, rootSeq++, paymentAmount);
    }
}

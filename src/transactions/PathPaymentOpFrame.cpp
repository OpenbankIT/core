// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/PathPaymentOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/OfferFrame.h"
#include "database/Database.h"
#include "OfferExchange.h"
#include <algorithm>

#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "main/Application.h"

namespace stellar
{

using namespace std;
using xdr::operator==;

PathPaymentOpFrame::PathPaymentOpFrame(Operation const& op,
                                       OperationResult& res,
                                       OperationFee* fee,
                                       TransactionFrame& parentTx,
                                       bool isCreate)
    : OperationFrame(op, res, fee, parentTx)
    , mPathPayment(mOperation.body.pathPaymentOp())
    , mIsCreate(isCreate)
{
}

TrustFrame::pointer PathPaymentOpFrame::getCommissionDest(LedgerManager const& ledgerManager, LedgerDelta& delta, Database& db,
	AccountFrame::pointer commissionDest, Asset& asset) {
	
	auto commissionDestLine = TrustFrame::loadTrustLine(commissionDest->getID(), asset, db, &delta);

	if (!commissionDestLine)
	{
		//this trust line doesn't exist, lets add it
		commissionDestLine = std::make_shared<TrustFrame>();
		auto& tl = commissionDestLine->getTrustLine();
		tl.accountID = commissionDest->getID();
		tl.asset = asset;
		tl.limit = INT64_MAX;
		tl.balance = 0;
		auto issuer = AccountFrame::loadAccount(delta, getIssuer(asset), db);
		assert(!!issuer);
		commissionDestLine->setAuthorized(!issuer->isAuthRequired());

		if (!commissionDest->addNumEntries(1, ledgerManager))
		{
			return nullptr;
		}

		commissionDest->storeChange(delta, db);
		commissionDestLine->storeAdd(delta, db);
	}

	return commissionDestLine;
}

AccountFrame::pointer
PathPaymentOpFrame::createDestination(Application& app, LedgerManager& ledgerManager, LedgerDelta& delta) {
	// build a createAccountOp
	Operation op;
	op.sourceAccount = mOperation.sourceAccount;
	op.body.type(CREATE_ACCOUNT);
	CreateAccountOp& caOp = op.body.createAccountOp();
	caOp.destination = mPathPayment.destination;
	caOp.body.accountType(ACCOUNT_ANONYMOUS_USER);

	OperationResult opRes;
	opRes.code(opINNER);
	opRes.tr().type(CREATE_ACCOUNT);

	//no need to take fee twice
	OperationFee fee;
	fee.type(OperationFeeType::opFEE_NONE);

	CreateAccountOpFrame createAccount(op, opRes, &fee, mParentTx);
	createAccount.setSourceAccountPtr(mSourceAccount);

	// create account
	if (!createAccount.doCheckValid(app) ||
		!createAccount.doApply(app, delta, ledgerManager))
	{
		if (createAccount.getResultCode() != opINNER)
		{
			throw std::runtime_error("Unexpected error code from createAccount");
		}
		switch (CreateAccountOpFrame::getInnerCode(createAccount.getResult()))
		{
		case CREATE_ACCOUNT_UNDERFUNDED:
		case CREATE_ACCOUNT_LOW_RESERVE:
		case CREATE_ACCOUNT_NOT_AUTHORIZED_TYPE:
			return nullptr;
		case CREATE_ACCOUNT_MALFORMED:
			app.getMetrics().NewMeter({ "op-path-payment", "failure", "malformed-create-account-op" },
				"operation").Mark();
			throw std::runtime_error("Failed to create account - create account op is malformed");
		case CREATE_ACCOUNT_ALREADY_EXIST:
			app.getMetrics().NewMeter({ "op-path-payment", "failure", "already-exists-create-account-op" },
				"operation").Mark();
			throw std::runtime_error("Failed to create account - already exists");
		case CREATE_ACCOUNT_WRONG_TYPE:
			app.getMetrics().NewMeter({ "op-path-payment", "failure", "wrong-type-create-account-op" },
				"operation").Mark();
			throw std::runtime_error("Failed to create account - wrong type");
		default:
			throw std::runtime_error("Unexpected error code from createAccount");
		}
	}
	return createAccount.getDestAccount();
}

bool
PathPaymentOpFrame::doApply(Application& app,
                            LedgerDelta& delta, LedgerManager& ledgerManager)
{

    Database& db = ledgerManager.getDatabase();

    innerResult().code(PATH_PAYMENT_SUCCESS);
    // tracks the last amount that was traded
	int64_t curBCommission = 0;
	if (mFee->type() == OperationFeeType::opFEE_CHARGED) {
		curBCommission = mFee->fee().amountToCharge;
	}
    int64_t curBReceived = mPathPayment.destAmount - curBCommission;
    Asset curB = mPathPayment.destAsset;

    // update balances, walks backwards

    // build the full path to the destination, starting with sendAsset
    std::vector<Asset> fullPath;
    fullPath.emplace_back(mPathPayment.sendAsset);
    fullPath.insert(fullPath.end(), mPathPayment.path.begin(),
                    mPathPayment.path.end());

    
    AccountFrame::pointer destination;
	AccountFrame::pointer commissionDestination;

	commissionDestination = AccountFrame::loadAccount(delta, app.getConfig().BANK_COMMISSION_KEY, db);
	assert(!!commissionDestination);


    destination =
    AccountFrame::loadAccount(delta, mPathPayment.destination, db);
    
    if (!destination)
    {
        destination = createDestination(app, ledgerManager, delta);
        bool destinationCreated = !!destination;
        // if destination was created and asset is not native - create trust line
        if (destinationCreated && mPathPayment.destAsset.type() != ASSET_TYPE_NATIVE)
        {
            auto line = OperationFrame::createTrustLine(app, ledgerManager, delta, mParentTx, destination, mPathPayment.destAsset);
            destinationCreated = !!line;
        }
        if (!destinationCreated)
        {
            app.getMetrics().NewMeter({ "op-path-payment", "failure", "no-destination" },
                                      "operation").Mark();
            innerResult().code(PATH_PAYMENT_NO_DESTINATION);
            return false;
        }
    }

    // update last balance in the chain
    if (curB.type() == ASSET_TYPE_NATIVE)
    {
        if (destination->getAccount().accountType == ACCOUNT_SCRATCH_CARD)
        {
            app.getMetrics().NewMeter({ "op-path-payment", "failure", "destination-scratch-card" },
                                      "operation").Mark();
            innerResult().code(PATH_PAYMENT_NO_DESTINATION);
            return false;
        }

        destination->getAccount().balance += curBReceived;
		commissionDestination->getAccount().balance += curBCommission;
        destination->storeChange(delta, db);
		commissionDestination->storeChange(delta, db);
    }
    else
    {
        TrustFrame::pointer destLine;

        auto tlI = TrustFrame::loadTrustLineIssuer(mPathPayment.destination,
                                                   curB, db, delta);
        if (!tlI.second)
        {
            app.getMetrics().NewMeter({"op-path-payment", "failure", "no-issuer"},
                                      "operation").Mark();
            innerResult().code(PATH_PAYMENT_NO_ISSUER);
            innerResult().noIssuer() = curB;
            return false;
        }
        destLine = tlI.first;
        
        if(!destLine)
        {
            destLine = OperationFrame::createTrustLine(app, ledgerManager, delta, mParentTx, destination, mPathPayment.destAsset);   
        }

//        if (!destLine)
//        {
//            app.getMetrics().NewMeter({"op-path-payment", "failure", "no-trust"},
//                             "operation").Mark();
//            innerResult().code(PATH_PAYMENT_NO_TRUST);
//            return false;
//        }

        if (!destLine->isAuthorized())
        {
            app.getMetrics().NewMeter({"op-path-payment", "failure", "not-authorized"},
                             "operation").Mark();
            innerResult().code(PATH_PAYMENT_NOT_AUTHORIZED);
            return false;
        }

        if (destination->getAccount().accountType == ACCOUNT_SCRATCH_CARD && !mIsCreate)
        {
            app.getMetrics().NewMeter({ "op-path-payment", "failure", "destination-scratch-card" },
                                      "operation").Mark();
            innerResult().code(PATH_PAYMENT_NO_DESTINATION);
            return false;
        }

        if (!destLine->addBalance(curBReceived))
        {
            app.getMetrics().NewMeter({"op-path-payment", "failure", "line-full"},
                             "operation").Mark();
            innerResult().code(PATH_PAYMENT_LINE_FULL);
            return false;
        }

		TrustFrame::pointer commissionDestLine = getCommissionDest(ledgerManager, delta, db, commissionDestination, curB);

		if (!commissionDestLine) {
			app.getMetrics().NewMeter({ "op-path-payment", "failure", "comission-dest-low-reserve" },
				"operation").Mark();
			innerResult().code(PATH_PAYMENT_NO_DESTINATION);
			return false;
		}

        if (!commissionDestLine->addBalance(curBCommission)){
            app.getMetrics().NewMeter({"op-path-payment", "failure", "commission-line-full"},
                                      "operation").Mark();
            innerResult().code(PATH_PAYMENT_LINE_FULL);
            return false;
        }

        commissionDestLine->storeChange(delta, db);
        destLine->storeChange(delta, db);
    }

    innerResult().success().last =
        SimplePaymentResult(mPathPayment.destination, curB, curBReceived);

	auto curBNeedToSend = mPathPayment.destAmount;
	
    // now, walk the path backwards
    for (int i = (int)fullPath.size() - 1; i >= 0; i--)
    {
        int64_t curASent, actualCurBReceived;
        Asset const& curA = fullPath[i];

        if (curA == curB)
        {
            continue;
        }

        if (curA.type() != ASSET_TYPE_NATIVE)
        {
            if (!AccountFrame::loadAccount(delta, getIssuer(curA), db))
            {
                app.getMetrics().NewMeter({"op-path-payment", "failure", "no-issuer"},
                                 "operation").Mark();
                innerResult().code(PATH_PAYMENT_NO_ISSUER);
                innerResult().noIssuer() = curA;
                return false;
            }
        }

        OfferExchange oe(delta, ledgerManager);

        // curA -> curB
        medida::MetricsRegistry& metrics = app.getMetrics();
        OfferExchange::ConvertResult r = oe.convertWithOffers(
            curA, INT64_MAX, curASent, curB, curBNeedToSend, actualCurBReceived,
            [this, &metrics](OfferFrame const& o)
            {
                if (o.getSellerID() == getSourceID())
                {
                    // we are crossing our own offer, potentially invalidating
                    // mSourceAccount (balance or numSubEntries)
                    metrics.NewMeter({"op-path-payment", "failure",
                                      "offer-cross-self"},
                                     "operation").Mark();
                    innerResult().code(PATH_PAYMENT_OFFER_CROSS_SELF);
                    return OfferExchange::eStop;
                }
                return OfferExchange::eKeep;
            });
        switch (r)
        {
        case OfferExchange::eFilterStop:
            return false;
        case OfferExchange::eOK:
            if (curBNeedToSend == actualCurBReceived)
            {
                break;
            }
        // fall through
        case OfferExchange::ePartial:
            app.getMetrics().NewMeter({"op-path-payment", "failure", "too-few-offers"},
                             "operation").Mark();
            innerResult().code(PATH_PAYMENT_TOO_FEW_OFFERS);
            return false;
        }
        assert(curBNeedToSend == actualCurBReceived);
		curBNeedToSend = curASent; // next round, we need to send enough
        curB = curA;

        // add offers that got taken on the way
        // insert in front to match the path's order
        auto& offers = innerResult().success().offers;
        offers.insert(offers.begin(), oe.getOfferTrail().begin(),
                      oe.getOfferTrail().end());
    }

    // last step: we've reached the first account in the chain, update its
    // balance

    int64_t curBSent;

    curBSent = curBNeedToSend;

    if (curBSent > mPathPayment.sendMax)
    { // make sure not over the max
        app.getMetrics().NewMeter({"op-path-payment", "failure", "over-send-max"},
                         "operation").Mark();
        innerResult().code(PATH_PAYMENT_OVER_SENDMAX);
        return false;
    }

    if (curB.type() == ASSET_TYPE_NATIVE)
    {
        int64_t minBalance = mSourceAccount->getMinimumBalance(ledgerManager);

        if ((mSourceAccount->getAccount().balance - curBSent) < minBalance)
        { // they don't have enough to send
            app.getMetrics().NewMeter({"op-path-payment", "failure", "underfunded"},
                             "operation").Mark();
            innerResult().code(PATH_PAYMENT_UNDERFUNDED);
            return false;
        }

        mSourceAccount->getAccount().balance -= curBSent;
        mSourceAccount->storeChange(delta, db);
    }
    else
    {
        TrustFrame::pointer sourceLineFrame;
        auto tlI =
        TrustFrame::loadTrustLineIssuer(getSourceID(), curB, db, delta);
        if (!tlI.second)
        {
            app.getMetrics().NewMeter({"op-path-payment", "failure", "no-issuer"},
                                      "operation").Mark();
            innerResult().code(PATH_PAYMENT_NO_ISSUER);
            innerResult().noIssuer() = curB;
            return false;
        }
        bool sourceLineExists = !!tlI.first;
        if (!sourceLineExists)
        {
            if (getSourceID() == getIssuer(curB))
            {
                auto line = OperationFrame::createTrustLine(app, ledgerManager, delta, mParentTx, mSourceAccount, curB);
                sourceLineExists = !!line;
                sourceLineFrame = line;
            }
        }
        else
        {
            sourceLineFrame = tlI.first;
        }

        if (!sourceLineFrame)
        {
            app.getMetrics().NewMeter({"op-path-payment", "failure", "src-no-trust"},
                             "operation").Mark();
            innerResult().code(PATH_PAYMENT_SRC_NO_TRUST);
            return false;
        }

        if (!sourceLineFrame->isAuthorized())
        {
            app.getMetrics().NewMeter(
                        {"op-path-payment", "failure", "src-not-authorized"},
                        "operation").Mark();
            innerResult().code(PATH_PAYMENT_SRC_NOT_AUTHORIZED);
            return false;
        }

        if (!sourceLineFrame->addBalance(-curBSent))
        {
            app.getMetrics().NewMeter({"op-path-payment", "failure", "underfunded"},
                             "operation").Mark();
            innerResult().code(PATH_PAYMENT_UNDERFUNDED);
            return false;
        }

        sourceLineFrame->storeChange(delta, db);
    }

    app.getMetrics().NewMeter({"op-path-payment", "success", "apply"}, "operation")
        .Mark();

    return true;
}

bool
PathPaymentOpFrame::doCheckValid(Application& app)
{
	// Fee can't be nullptr
	assert(!!mFee);
	int64 commission = 0;
	if (mFee->type() != OperationFeeType::opFEE_NONE) {

		if (!(mFee->fee().asset == mPathPayment.destAsset)) {
			app.getMetrics().NewMeter({ "op-path-payment", "failure", "fee-invalid-asset" },
				"operation").Mark();
			innerResult().code(PATH_PAYMENT_MALFORMED);
			return false;
		}

		if (mFee->fee().amountToCharge < 0) {
			app.getMetrics().NewMeter({ "op-path-payment", "failure", "fee-invalid-amount" },
				"operation").Mark();
			innerResult().code(PATH_PAYMENT_MALFORMED);
			return false;
		}

		commission = mFee->fee().amountToCharge;
	}
    if (mPathPayment.destAmount - commission <= 0 || mPathPayment.sendMax <= 0)
    {
        app.getMetrics().NewMeter({"op-path-payment", "invalid", "malformed-amounts"},
                         "operation").Mark();
        innerResult().code(PATH_PAYMENT_MALFORMED);
        return false;
    }

	auto const& issuer = app.getIssuer();
    if (!isAssetValid(issuer, mPathPayment.sendAsset) ||
        !isAssetValid(issuer, mPathPayment.destAsset))
    {
        app.getMetrics().NewMeter({"op-path-payment", "invalid", "malformed-currencies"},
                         "operation").Mark();
        innerResult().code(PATH_PAYMENT_MALFORMED);
        return false;
    }
    auto const& p = mPathPayment.path;
	if (!std::all_of(p.begin(), p.end(), [issuer](Asset asset) {return isAssetValid(issuer, asset);}))
    {
        app.getMetrics().NewMeter({"op-path-payment", "invalid", "malformed-currencies"},
                         "operation").Mark();
        innerResult().code(PATH_PAYMENT_MALFORMED);
        return false;
    }
    return true;
}
}

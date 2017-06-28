// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/PaymentReversalOpFrame.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/TrustFrame.h"
#include "ledger/ReversedPaymentFrame.h"
#include "database/Database.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "main/Application.h"
#include <algorithm>

namespace stellar
{

using namespace std;
using xdr::operator==;

PaymentReversalOpFrame::PaymentReversalOpFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                               TransactionFrame& parentTx)
    : OperationFrame(op, res, fee, parentTx), mPaymentReversal(mOperation.body.paymentReversalOp())
{
}

bool PaymentReversalOpFrame::checkAllowed() {
	return true;
}

bool PaymentReversalOpFrame::checkAlreadyReversed(LedgerDelta& delta, Database& db) {
	// check if already reversed
	LedgerKey key;
	key.type(REVERSED_PAYMENT);
	key.reversedPayment().rID = mPaymentReversal.paymentID;
	auto alreadyReversed = ReversedPaymentFrame::exists(db, key);
	if (alreadyReversed) {
		return false;
	}

	auto reversedPayment = make_shared<ReversedPaymentFrame>();
	reversedPayment->getReversedPayment().rID = mPaymentReversal.paymentID;
	reversedPayment->storeAdd(delta, db);
	return true;
}

bool
PaymentReversalOpFrame::doApply(Application& app, LedgerDelta& delta,
                        LedgerManager& ledgerManager)
{
	if (!checkAllowed()) {
		app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "not-allowed" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_NOT_ALLOWED);
		return false;
	}

	Database& db = ledgerManager.getDatabase();

	if (!checkAlreadyReversed(delta, db)) {
		app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "already-reversed" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_ALREADY_REVERSED);
		return false;
	}

	// Handle commission

	auto commissionDestLine = TrustFrame::loadTrustLine(app.getConfig().BANK_COMMISSION_KEY, mPaymentReversal.asset, db, &delta);
	if (!commissionDestLine || !commissionDestLine->addBalance(-mPaymentReversal.commissionAmount)) {
		app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "comission-dest-low-reserve" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_COMMISSION_UNDERFUNDED);
		return false;
	}

	commissionDestLine->storeChange(delta, db);

	// Handle payment reversal source
	TrustFrame::pointer sourceLineFrame;
	auto issuerTrustLine = TrustFrame::loadTrustLineIssuer(getSourceID(), mPaymentReversal.asset, db, delta);
	if (!issuerTrustLine.second)
	{
		app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "no-issuer" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_NO_ISSUER);
		return false;
	}

	bool sourceLineExists = !!issuerTrustLine.first;
	if (!sourceLineExists)
	{
		if (getSourceID() == getIssuer(mPaymentReversal.asset))
		{
			auto line = OperationFrame::createTrustLine(app, ledgerManager, delta, mParentTx, mSourceAccount, mPaymentReversal.asset);
			sourceLineExists = !!line;
			sourceLineFrame = line;
		}
	}
	else
	{
		sourceLineFrame = issuerTrustLine.first;
	}

	if (!sourceLineExists)
	{
		app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "src-no-trust" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_SRC_NO_TRUST);
		return false;
	}

	if (!sourceLineFrame->isAuthorized())
	{
		app.getMetrics().NewMeter(
		{ "op-reversal-payment", "failure", "src-not-authorized" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_SRC_NOT_AUTHORIZED);
		return false;
	}

	int64 sourceRecieved = mPaymentReversal.amount - mPaymentReversal.commissionAmount;

	if (!sourceLineFrame->addBalance(-sourceRecieved))
	{
		app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "underfunded" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_UNDERFUNDED);
		return false;
	}

	sourceLineFrame->storeChange(delta, db);

	// Handle destination
	TrustFrame::pointer destLine;

	auto tlI = TrustFrame::loadTrustLineIssuer(mPaymentReversal.paymentSource, mPaymentReversal.asset, db, delta);
	if (!tlI.second)
	{
		app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "no-issuer" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_NO_ISSUER);
		return false;
	}

	destLine = tlI.first;

	if (!destLine)
	{
		auto destination = AccountFrame::loadAccount(delta, mPaymentReversal.paymentSource, db);
		if (!destination) {
			app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "no-payment-sender" },
				"operation").Mark();
			innerResult().code(PAYMENT_REVERSAL_NO_PAYMENT_SENDER);
			return false;
		}

		destLine = OperationFrame::createTrustLine(app, ledgerManager, delta, mParentTx, destination, mPaymentReversal.asset);
	}

	if (!destLine)
	{
		app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "no-payment-sender-trust" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_NO_PAYMENT_SENDER_TRUST);
		return false;
	}

	if (!destLine->isAuthorized())
	{
		app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "payment-sender-not-authorized" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_PAYMENT_SENDER_NOT_AUTHORIZED);
		return false;
	}

	if (!destLine->addBalance(mPaymentReversal.amount))
	{
		app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "payment-sender-line-full" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_PAYMENT_SENDER_LINE_FULL);
		return false;
	}

	destLine->storeChange(delta, db);

	innerResult().code(PAYMENT_REVERSAL_SUCCESS);
	return true;
}

bool
PaymentReversalOpFrame::doCheckValid(Application& app)
{
    if (mPaymentReversal.amount <= 0)
    {
        app.getMetrics().NewMeter({"op-reversal-payment", "invalid", "malformed-amount"},
                         "operation").Mark();
        innerResult().code(PAYMENT_REVERSAL_MALFORMED);
        return false;
    }
    
	if (mPaymentReversal.commissionAmount < 0)
	{
		app.getMetrics().NewMeter({ "op-reversal-payment", "invalid", "malformed-negative-commission" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_MALFORMED);
		return false;
	}

	if (!isAssetValid(app.getIssuer(), mPaymentReversal.asset) || mPaymentReversal.asset.type() == ASSET_TYPE_NATIVE)
	{
		app.getMetrics().NewMeter({ "op-reversal-payment", "invalid", "malformed-invalid-asset" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_MALFORMED);
		return false;
	}

    return true;
}
    
}

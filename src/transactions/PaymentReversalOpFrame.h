#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{

class PaymentReversalOpFrame : public OperationFrame
{
    PaymentReversalResult&
    innerResult()
    {
        return mResult.tr().paymentReversalResult();
    }
    PaymentReversalOp const& mPaymentReversal;

	bool checkAllowed();
	// Checks if payment have been already reversed. 
	// If not creates ReversedPayment instance
	// Returns false if already reversed
	bool checkAlreadyReversed(LedgerDelta& delta, Database& db);

  public:
    PaymentReversalOpFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                   TransactionFrame& parentTx);

    bool doApply(Application& app, LedgerDelta& delta,
                 LedgerManager& ledgerManager) override;
    bool doCheckValid(Application& app) override;

    static PaymentReversalResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().paymentReversalResult().code();
    }
};
}

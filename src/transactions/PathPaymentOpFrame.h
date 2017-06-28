#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"
#include "ledger/TrustFrame.h"

namespace stellar
{

class PathPaymentOpFrame : public OperationFrame
{
    PathPaymentResult&
    innerResult()
    {
        return mResult.tr().pathPaymentResult();
    }
    PathPaymentOp const& mPathPayment;
    bool mIsCreate;

	TrustFrame::pointer getCommissionDest(LedgerManager const& ledgerManager, LedgerDelta& delta, Database& db,
		AccountFrame::pointer commissionDest, Asset& asset);
	AccountFrame::pointer createDestination(Application& app, LedgerManager& ledgerManager, LedgerDelta& delta);

  public:
    PathPaymentOpFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                       TransactionFrame& parentTx, bool isCreate = false);

    bool doApply(Application& app, LedgerDelta& delta,
                 LedgerManager& ledgerManager) override;
    bool doCheckValid(Application& app) override;

    static PathPaymentResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().pathPaymentResult().code();
    }

};
}

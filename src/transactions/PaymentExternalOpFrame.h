#pragma once

// Copyright 2017 AtticLab and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{

class PaymentExternalOpFrame : public OperationFrame
{
    PaymentResult&
    innerResult()
    {
        return mResult.tr().paymentResult();
    }
    ExternalPaymentOp const& mPayment;

  public:
    PaymentExternalOpFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                   TransactionFrame& parentTx);

    bool doApply(Application& app, LedgerDelta& delta,
                 LedgerManager& ledgerManager) override;
    bool doCheckValid(Application& app) override;

    static PaymentResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().paymentResult().code();
    }
};
}

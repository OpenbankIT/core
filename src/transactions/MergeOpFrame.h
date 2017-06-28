#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{
class MergeOpFrame : public OperationFrame
{
    AccountMergeResult&
    innerResult()
    {
        return mResult.tr().accountMergeResult();
    }

    int32_t getNeededThreshold() const override;

  public:
    MergeOpFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                 TransactionFrame& parentTx);

    bool doApply(Application& app, LedgerDelta& delta,
                 LedgerManager& ledgerManager) override;
    bool doCheckValid(Application& app) override;
    bool checkValid(Application& app, LedgerDelta* delta = nullptr) override;
    void
    setSourceAccountPtr(AccountFrame::pointer sa)
    {
        mSourceAccount = sa;
    }
    static AccountMergeResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().accountMergeResult().code();
    }
};
}

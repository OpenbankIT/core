#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/OperationFrame.h"

namespace stellar
{

class CreateAccountOpFrame : public OperationFrame
{
    CreateAccountResult&
    innerResult()
    {
        return mResult.tr().createAccountResult();
    }
    CreateAccountOp const& mCreateAccount;

	AccountFrame::pointer mDestAccount;
    bool doApplyCreateScratch(Application& app, LedgerDelta& delta,
                                               LedgerManager& ledgerManager);
    

  public:
    CreateAccountOpFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                         TransactionFrame& parentTx);

    bool doApply(Application& app, LedgerDelta& delta,
                 LedgerManager& ledgerManager) override;
    bool doCheckValid(Application& app) override;

	AccountFrame::pointer getDestAccount() {
		return mDestAccount;
	}

    static CreateAccountResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().createAccountResult().code();
    }
};
}

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/MergeOpFrame.h"
#include "database/Database.h"
#include "ledger/TrustFrame.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

using namespace soci;

namespace stellar
{
using xdr::operator==;

MergeOpFrame::MergeOpFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                           TransactionFrame& parentTx)
    : OperationFrame(op, res, fee, parentTx)
{
}

int32_t
MergeOpFrame::getNeededThreshold() const
{
    return mSourceAccount->getHighThreshold();
}

// make sure the deleted Account hasn't issued credit
// make sure we aren't holding any credit
// make sure the we delete all the offers
// make sure the we delete all the trustlines
// move the XLM to the new account
bool
MergeOpFrame::doApply(Application& app, LedgerDelta& delta,
                      LedgerManager& ledgerManager)
{
    AccountFrame::pointer otherAccount;
    Database& db = ledgerManager.getDatabase();
    otherAccount =
        AccountFrame::loadAccount(delta, mOperation.body.destination(), db);

    if (!otherAccount)
    {
        app.getMetrics().NewMeter({"op-merge", "failure", "no-account"}, "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_NO_ACCOUNT);
        return false;
    }

    if (mSourceAccount->isImmutableAuth())
    {
        app.getMetrics().NewMeter({"op-merge", "failure", "static-auth"}, "operation")
            .Mark();
        innerResult().code(ACCOUNT_MERGE_IMMUTABLE_SET);
        return false;
    }

    auto const& sourceAccount = mSourceAccount->getAccount();
    if (sourceAccount.numSubEntries != sourceAccount.signers.size())
    {
        app.getMetrics().NewMeter({"op-merge", "failure", "has-sub-entries"},
                         "operation").Mark();
        innerResult().code(ACCOUNT_MERGE_HAS_SUB_ENTRIES);
        return false;
    }

    int64 sourceBalance = sourceAccount.balance;
    otherAccount->getAccount().balance += sourceBalance;
    otherAccount->storeChange(delta, db);
    mSourceAccount->storeDelete(delta, db);

    app.getMetrics().NewMeter({"op-merge", "success", "apply"}, "operation").Mark();
    innerResult().code(ACCOUNT_MERGE_SUCCESS);
    innerResult().sourceAccountBalance() = sourceBalance;
    return true;
}

bool
MergeOpFrame::checkValid(Application& app, LedgerDelta* delta)
{   
    bool forApply = (delta != nullptr);
    if (!loadAccount(delta, app.getDatabase()))
    {
        if (forApply || !mOperation.sourceAccount)
        {
            app.getMetrics()
                .NewMeter({"operation", "invalid", "no-account"}, "operation")
                .Mark();
            mResult.code(opNO_ACCOUNT);
            return false;
        }
        else
        {
            mSourceAccount =
                AccountFrame::makeAuthOnlyAccount(*mOperation.sourceAccount);
        }
    }
    auto bankKey = app.getConfig().BANK_MASTER_KEY;
    if (getSourceID() == bankKey)
    {
        bool isBank = false;
        for (auto& signer : mUsedSigners)
        {
            if (signer.signerType == SIGNER_ADMIN){
                isBank = true;
                break;
            }
        }
        if (!isBank)
        {
            app.getMetrics()
            .NewMeter({"operation", "invalid", "bad-auth"}, "operation")
            .Mark();
            mResult.code(opBAD_AUTH);
            return false;
        }
    }

    if (!forApply)
    {
        // safety: operations should not rely on ledger state as
        // previous operations may change it (can even create the account)
        mSourceAccount.reset();
    }

    mResult.code(opINNER);
    mResult.tr().type(mOperation.body.type());

    return doCheckValid(app);
}

bool
MergeOpFrame::doCheckValid(Application& app)
{
    if (!mOperation.sourceAccount)
    {
        app.getMetrics().NewMeter({"op-merge", "invalid", "malformed-self-merge"},
                         "operation").Mark();
        innerResult().code(ACCOUNT_MERGE_MALFORMED);
        return false;
    }
    // makes sure not merging into self
    if (*mOperation.sourceAccount == mOperation.body.destination())
    {
        app.getMetrics().NewMeter({"op-merge", "invalid", "malformed-self-merge"},
                         "operation").Mark();
        innerResult().code(ACCOUNT_MERGE_MALFORMED);
        return false;
    }
    if (*mOperation.sourceAccount == app.getConfig().BANK_MASTER_KEY)
    {
        app.getMetrics().NewMeter({"op-merge", "invalid", "bank-account-merge"},
                         "operation").Mark();
        innerResult().code(ACCOUNT_MERGE_MALFORMED);
        return false;
    }
    return true;
}
}

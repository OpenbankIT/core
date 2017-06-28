// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/PathPaymentOpFrame.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/TrustFrame.h"
#include "ledger/OfferFrame.h"
#include "database/Database.h"
#include "OfferExchange.h"
#include <algorithm>

#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

using namespace std;
using xdr::operator==;

CreateAccountOpFrame::CreateAccountOpFrame(Operation const& op,
                                           OperationResult& res,
                                           OperationFee* fee,
                                           TransactionFrame& parentTx)
    : OperationFrame(op, res, fee, parentTx)
    , mCreateAccount(mOperation.body.createAccountOp())
{
}

bool
CreateAccountOpFrame::doApply(Application& app,
                              LedgerDelta& delta, LedgerManager& ledgerManager)
{
    

    Database& db = ledgerManager.getDatabase();

    mDestAccount =
        AccountFrame::loadAccount(delta, mCreateAccount.destination, db);

    if (mCreateAccount.body.accountType() == ACCOUNT_SCRATCH_CARD && mSourceAccount->getAccount().accountType != ACCOUNT_DISTRIBUTION_AGENT){
        app.getMetrics().NewMeter({"op-create-scratchcard-account", "invalid",
                                   "malformed-source-type"},
                                  "operation").Mark();
        innerResult().code(CREATE_ACCOUNT_WRONG_TYPE);
        return false;
    }

    if (!mDestAccount)
    {
		mDestAccount = make_shared<AccountFrame>(mCreateAccount.destination);
		mDestAccount->getAccount().seqNum = 0;
		mDestAccount->getAccount().accountType  = mCreateAccount.body.accountType();
		mDestAccount->storeAdd(delta, db);
		if (mCreateAccount.body.accountType() == ACCOUNT_SCRATCH_CARD)
		{
            return doApplyCreateScratch(app, delta, ledgerManager);
		}
        
        app.getMetrics().NewMeter({"op-create-account", "success", "apply"},
                                  "operation").Mark();
        innerResult().code(CREATE_ACCOUNT_SUCCESS);
        return true;
    }
    else
    {
        app.getMetrics().NewMeter({"op-create-account", "failure", "already-exist"},
                         "operation").Mark();
        innerResult().code(CREATE_ACCOUNT_ALREADY_EXIST);
        return false;
    }
}
    
    bool
    CreateAccountOpFrame::doApplyCreateScratch(Application& app, LedgerDelta& delta,
                            LedgerManager& ledgerManager)
    {
        // build a pathPaymentOp
        Operation op;
        op.sourceAccount = mOperation.sourceAccount;
        op.body.type(PATH_PAYMENT);
        PathPaymentOp& ppOp = op.body.pathPaymentOp();
        ppOp.sendAsset = mCreateAccount.body.scratchCard().asset;
        ppOp.destAsset = mCreateAccount.body.scratchCard().asset;
        
        ppOp.destAmount = mCreateAccount.body.scratchCard().amount;
        ppOp.sendMax = mCreateAccount.body.scratchCard().amount;
        
        ppOp.destination = mCreateAccount.destination;
        
        OperationResult opRes;
        opRes.code(opINNER);
        opRes.tr().type(PATH_PAYMENT);
        PathPaymentOpFrame ppayment(op, opRes, mFee, mParentTx, true);
        ppayment.setSourceAccountPtr(mSourceAccount);
        
        if (!ppayment.doCheckValid(app) ||
            !ppayment.doApply(app, delta, ledgerManager))
        {
            if (ppayment.getResultCode() != opINNER)
            {
                throw std::runtime_error("Unexpected error code from pathPayment");
            }
            CreateAccountResultCode res;
            
            switch (PathPaymentOpFrame::getInnerCode(ppayment.getResult()))
            {
                case PATH_PAYMENT_UNDERFUNDED:
                case PATH_PAYMENT_SRC_NO_TRUST:
                    app.getMetrics().NewMeter({"op-create-account", "failure", "underfunded"},
                                              "operation").Mark();
                    res = CREATE_ACCOUNT_UNDERFUNDED;
                    break;
                case PATH_PAYMENT_SRC_NOT_AUTHORIZED:
                    app.getMetrics().NewMeter({"op-create-account", "failure", "src-not-authorized"},
                                              "operation").Mark();
                    res = CREATE_ACCOUNT_NOT_AUTHORIZED_TYPE;
                    break;
                case PATH_PAYMENT_LINE_FULL:
                    app.getMetrics().NewMeter({"op-create-account", "failure", "line-full"},
                                              "operation").Mark();
                    res = CREATE_ACCOUNT_LINE_FULL;
                    break;
                case PATH_PAYMENT_NO_ISSUER:
                    app.getMetrics().NewMeter({"op-create-account", "failure", "no-issuer"},
                                              "operation").Mark();
                    res = CREATE_ACCOUNT_NO_ISSUER;
                    break;
                default:
                    throw std::runtime_error("Unexpected error code from pathPayment");
            }
            innerResult().code(res);
            return false;
        }
        
        assert(PathPaymentOpFrame::getInnerCode(ppayment.getResult()) ==
               PATH_PAYMENT_SUCCESS);
        
        app.getMetrics().NewMeter({"op-payment", "success", "apply"}, "operation").Mark();
        innerResult().code(CREATE_ACCOUNT_SUCCESS);
        
        return true;
    }
    
bool
CreateAccountOpFrame::doCheckValid(Application& app)
{
    switch (mCreateAccount.body.accountType()) {
        case ACCOUNT_ANONYMOUS_USER:
			break;
        case ACCOUNT_SCRATCH_CARD:
			if (mCreateAccount.body.scratchCard().amount <= 0) {
				app.getMetrics().NewMeter({ "op-create-account", "invalid",
					"malformed-scratch-card-amount" },
					"operation").Mark();
				innerResult().code(CREATE_ACCOUNT_MALFORMED);
				return false;
			}

			if (!isAssetValid(app.getIssuer(), mCreateAccount.body.scratchCard().asset))
			{
				app.getMetrics().NewMeter({ "op-create-account", "invalid",
					"malformed-scratch-card-invalid-asset" },
					"operation").Mark();
				innerResult().code(CREATE_ACCOUNT_MALFORMED);
				return false;
			}
            break;
        case ACCOUNT_REGISTERED_USER:
        case ACCOUNT_MERCHANT:
        case ACCOUNT_DISTRIBUTION_AGENT:
        case ACCOUNT_SETTLEMENT_AGENT:
        case ACCOUNT_EXCHANGE_AGENT:
            if (getSourceID() == app.getConfig().BANK_MASTER_KEY){
                break;
            }
            else{
                app.getMetrics().NewMeter({"op-create-account", "invalid",
                    "not-bank-creating-type"},
                                          "operation").Mark();
                innerResult().code(CREATE_ACCOUNT_NOT_AUTHORIZED_TYPE);
                return false;
            }
            break;
        default:
            app.getMetrics().NewMeter({"op-create-account", "invalid",
                "malformed-wrong-type"},
                                      "operation").Mark();
            innerResult().code(CREATE_ACCOUNT_WRONG_TYPE);
            return false;
            break;
    }
    if (mCreateAccount.destination == getSourceID())
    {
        app.getMetrics().NewMeter({"op-create-account", "invalid",
                          "malformed-destination-equals-source"},
                         "operation").Mark();
        innerResult().code(CREATE_ACCOUNT_MALFORMED);
        return false;
    }

    return true;
}
}

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/AdministrativeOpFrame.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "database/Database.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "main/Application.h"
#include <algorithm>

namespace stellar
{

using namespace std;
using xdr::operator==;

AdministrativeOpFrame::AdministrativeOpFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                               TransactionFrame& parentTx)
    : OperationFrame(op, res, fee, parentTx), mAdministrative(mOperation.body.adminOp())
{
}

bool
AdministrativeOpFrame::doApply(Application& app, LedgerDelta& delta,
                        LedgerManager& ledgerManager)
{
	app.getMetrics().NewMeter({ "op-administrative", "success", "apply" }, "operation").Mark();
	innerResult().code(ADMINISTRATIVE_SUCCESS);
	return true;
}

bool
AdministrativeOpFrame::doCheckValid(Application& app)
{
	if (mAdministrative.opData.empty()) {
		app.getMetrics().NewMeter({ "op-administrative", "invalid", "empty-op-data" },
			"operation").Mark();
		innerResult().code(ADMINISTRATIVE_MALFORMED);
		return false;
	}
	
	if (!(getSourceID() == app.getConfig().BANK_MASTER_KEY)) {
		app.getMetrics().NewMeter({ "op-administrative", "invalid", "bank-is-not-source" },
			"operation").Mark();
		innerResult().code(ADMINISTRATIVE_NOT_AUTHORIZED);
		return false;
	}

	bool isAllAdmins = !mUsedSigners.empty();
	for (auto& signer : mUsedSigners) {
		if (signer.signerType != SIGNER_ADMIN) {
			isAllAdmins = false;
			break;
		}
	}

	if (!isAllAdmins) {
		app.getMetrics().NewMeter({ "op-administrative", "invalid", "signers-are-not-admins" },
			"operation").Mark();
		innerResult().code(ADMINISTRATIVE_NOT_AUTHORIZED);
		return false;
	}

	return true;
}
    
}
// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/ReversedPaymentFrame.h"
#include "database/Database.h"
#include "crypto/SecretKey.h"
#include "crypto/SHA.h"
#include "LedgerDelta.h"
#include "util/types.h"
#include "util/basen.h"

using namespace std;
using namespace soci;

namespace stellar
{
const char* ReversedPaymentFrame::kSQLCreateStatement1 =
    "CREATE TABLE reversed_payment"
    "("
        "id           BIGINT NOT NULL,"
		"lastmodified INT    NOT NULL,"
        "PRIMARY KEY (id)"
    ");";

static const char* reversedPaymentColumnSelector =
    "SELECT id, lastmodified FROM reversed_payment";

ReversedPaymentFrame::ReversedPaymentFrame() : EntryFrame(REVERSED_PAYMENT), mReversedPayment(mEntry.data.reversedPayment())
{
}

ReversedPaymentFrame::ReversedPaymentFrame(LedgerEntry const& from)
    : EntryFrame(from), mReversedPayment(mEntry.data.reversedPayment())
{
}

ReversedPaymentFrame::ReversedPaymentFrame(ReversedPaymentFrame const& from) : ReversedPaymentFrame(from.mEntry)
{
}

ReversedPaymentFrame& ReversedPaymentFrame::operator=(ReversedPaymentFrame const& other)
{
    if (&other != this)
    {
		mReversedPayment = other.mReversedPayment;
        mKey = other.mKey;
        mKeyCalculated = other.mKeyCalculated;
    }
    return *this;
}

ReversedPaymentFrame::pointer
ReversedPaymentFrame::loadReversedPayment(int64 id, Database& db)
{
    ReversedPaymentFrame::pointer retData;

    std::string sql = reversedPaymentColumnSelector;
    sql += " WHERE id = :id";
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(use(id));

    auto timer = db.getSelectTimer("reversed_payment");
    loadData(prep, [&retData](LedgerEntry const& data)
               {
                   retData = make_shared<ReversedPaymentFrame>(data);
               });

    return retData;
}

void
ReversedPaymentFrame::loadData(StatementContext& prep,
                       std::function<void(LedgerEntry const&)> dataProcessor)
{
    LedgerEntry le;
    le.data.type(REVERSED_PAYMENT);
    ReversedPaymentEntry& oe = le.data.reversedPayment();

    statement& st = prep.statement();
    st.exchange(into(oe.rID));
	st.exchange(into(le.lastModifiedLedgerSeq));
    st.define_and_bind();
    st.execute(true);
    while (st.got_data())
    {
        dataProcessor(le);
        st.fetch();
    }
}

bool
ReversedPaymentFrame::exists(Database& db, LedgerKey const& key)
{
    int exists = 0;
    auto timer = db.getSelectTimer("reversed_payment-exists");
    auto prep =
        db.getPreparedStatement("SELECT EXISTS (SELECT NULL FROM reversed_payment "
                                "WHERE id=:id)");
    auto& st = prep.statement();
    st.exchange(use(key.reversedPayment().rID));
    st.exchange(into(exists));
    st.define_and_bind();
    st.execute(true);
    return exists != 0;
}

uint64_t
ReversedPaymentFrame::countObjects(soci::session& sess)
{
    uint64_t count = 0;
    sess << "SELECT COUNT(*) FROM reversed_payment;", into(count);
    return count;
}

void
ReversedPaymentFrame::storeDelete(LedgerDelta& delta, Database& db) const
{
    storeDelete(delta, db, getKey());
}

void
ReversedPaymentFrame::storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key)
{
    auto timer = db.getDeleteTimer("reversed_payment");
    auto prep = db.getPreparedStatement("DELETE FROM reversed_payment WHERE id=:id");
    auto& st = prep.statement();
    st.exchange(use(key.reversedPayment().rID));
    st.define_and_bind();
    st.execute(true);
    delta.deleteEntry(key);
}


void
ReversedPaymentFrame::storeChange(LedgerDelta& delta, Database& db)
{
    storeUpdateHelper(delta, db, false);
}

void
ReversedPaymentFrame::storeAdd(LedgerDelta& delta, Database& db)
{
    storeUpdateHelper(delta, db, true);
}

void
ReversedPaymentFrame::storeUpdateHelper(LedgerDelta& delta, Database& db, bool insert)
{
    touch(delta);

    string sql;

    if (insert)
    {
        sql = "INSERT INTO reversed_payment (id, lastmodified)"
               " VALUES (:id, :v1)";
    }
    else
    {
		return;
    }

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();

    
    st.exchange(use(mReversedPayment.rID, "id"));
	st.exchange(use(getLastModified(), "v1"));

    st.define_and_bind();
    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("could not update SQL");
    }

    if (insert)
    {
        delta.addEntry(*this);
    }
    else
    {
        delta.modEntry(*this);
    }
}

void
ReversedPaymentFrame::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS reversed_payment;";
    db.getSession() << kSQLCreateStatement1;
}
}

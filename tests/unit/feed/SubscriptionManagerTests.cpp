//------------------------------------------------------------------------------
/*
    This file is part of clio: https://github.com/XRPLF/clio
    Copyright (c) 2024, the clio developers.

    Permission to use, copy, modify, and distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL,  DIRECT,  INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include "data/Types.hpp"
#include "feed/FeedTestUtil.hpp"
#include "feed/SubscriptionManager.hpp"
#include "util/MockBackendTestFixture.hpp"
#include "util/MockPrometheus.hpp"
#include "util/MockWsBase.hpp"
#include "util/TestObject.hpp"
#include "util/async/context/BasicExecutionContext.hpp"
#include "util/async/context/SyncExecutionContext.hpp"
#include "web/interface/ConnectionBase.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <xrpl/protocol/Book.h>
#include <xrpl/protocol/Fees.h>
#include <xrpl/protocol/Issue.h>
#include <xrpl/protocol/STObject.h>

#include <memory>
#include <string>
#include <vector>

constexpr static auto ACCOUNT1 = "rf1BiGeXwwQoi8Z2ueFYTEXSwuJYfV2Jpn";
constexpr static auto ACCOUNT2 = "rLEsXccBGNR3UPuPu2hUXPjziKC3qKSBun";
constexpr static auto CURRENCY = "0158415500000000C1F76FF6ECB0BAC600000000";
constexpr static auto ISSUER = "rK9DrarGKnVEo2nYp5MfVRXRYf5yRX3mwD";
constexpr static auto LEDGERHASH = "4BC50C9B0D8515D3EAAE1E74B29A95804346C491EE1A95BF25E4AAB854A6A652";

namespace json = boost::json;
using namespace feed;
using namespace feed::impl;

template <class Execution>
class SubscriptionManagerBaseTest : public util::prometheus::WithPrometheus, public MockBackendTest {
protected:
    std::shared_ptr<SubscriptionManager> subscriptionManagerPtr;
    std::shared_ptr<web::ConnectionBase> session;
    MockSession* sessionPtr = nullptr;

    void
    SetUp() override
    {
        subscriptionManagerPtr = std::make_shared<SubscriptionManager>(Execution(2), backend);
        session = std::make_shared<MockSession>();
        session->apiSubVersion = 1;
        sessionPtr = dynamic_cast<MockSession*>(session.get());
    }

    void
    TearDown() override
    {
        session.reset();
        subscriptionManagerPtr.reset();
    }
};

using SubscriptionManagerTest = SubscriptionManagerBaseTest<util::async::SyncExecutionContext>;

using SubscriptionManagerAsyncTest = SubscriptionManagerBaseTest<util::async::PoolExecutionContext>;

TEST_F(SubscriptionManagerAsyncTest, MultipleThreadCtx)
{
    subscriptionManagerPtr->subManifest(session);
    subscriptionManagerPtr->subValidation(session);

    constexpr static auto jsonManifest = R"({"manifest":"test"})";
    constexpr static auto jsonValidation = R"({"validation":"test"})";

    EXPECT_CALL(*sessionPtr, send(testing::_)).Times(testing::AtMost(2));

    subscriptionManagerPtr->forwardManifest(json::parse(jsonManifest).get_object());
    subscriptionManagerPtr->forwardValidation(json::parse(jsonValidation).get_object());
}

TEST_F(SubscriptionManagerAsyncTest, MultipleThreadCtxSessionDieEarly)
{
    subscriptionManagerPtr->subManifest(session);
    subscriptionManagerPtr->subValidation(session);

    EXPECT_CALL(*sessionPtr, send(testing::_)).Times(0);
    session.reset();

    subscriptionManagerPtr->forwardManifest(json::parse(R"({"manifest":"test"})").get_object());
    subscriptionManagerPtr->forwardValidation(json::parse(R"({"validation":"test"})").get_object());
}

TEST_F(SubscriptionManagerTest, ReportCurrentSubscriber)
{
    constexpr static auto ReportReturn =
        R"({
            "ledger":0,
            "transactions":2,
            "transactions_proposed":2,
            "manifests":2,
            "validations":2,
            "account":2,
            "accounts_proposed":2,
            "books":2,
            "book_changes":2
        })";
    std::shared_ptr<web::ConnectionBase> const session1 = std::make_shared<MockSession>();
    std::shared_ptr<web::ConnectionBase> session2 = std::make_shared<MockSession>();
    subscriptionManagerPtr->subBookChanges(session1);
    subscriptionManagerPtr->subBookChanges(session2);
    subscriptionManagerPtr->subManifest(session1);
    subscriptionManagerPtr->subManifest(session2);
    subscriptionManagerPtr->subProposedTransactions(session1);
    subscriptionManagerPtr->subProposedTransactions(session2);
    subscriptionManagerPtr->subTransactions(session1);
    session2->apiSubVersion = 2;
    subscriptionManagerPtr->subTransactions(session2);
    subscriptionManagerPtr->subValidation(session1);
    subscriptionManagerPtr->subValidation(session2);
    auto const account = GetAccountIDWithString(ACCOUNT1);
    subscriptionManagerPtr->subAccount(account, session1);
    subscriptionManagerPtr->subAccount(account, session2);
    subscriptionManagerPtr->subProposedAccount(account, session1);
    subscriptionManagerPtr->subProposedAccount(account, session2);
    auto const issue1 = GetIssue(CURRENCY, ISSUER);
    ripple::Book const book{ripple::xrpIssue(), issue1};
    subscriptionManagerPtr->subBook(book, session1);
    subscriptionManagerPtr->subBook(book, session2);
    EXPECT_EQ(subscriptionManagerPtr->report(), json::parse(ReportReturn));

    // count down when unsub manually
    subscriptionManagerPtr->unsubBookChanges(session1);
    subscriptionManagerPtr->unsubManifest(session1);
    subscriptionManagerPtr->unsubProposedTransactions(session1);
    subscriptionManagerPtr->unsubTransactions(session1);
    subscriptionManagerPtr->unsubValidation(session1);
    subscriptionManagerPtr->unsubAccount(account, session1);
    subscriptionManagerPtr->unsubProposedAccount(account, session1);
    subscriptionManagerPtr->unsubBook(book, session1);

    // try to unsub an account which is not subscribed
    auto const account2 = GetAccountIDWithString(ACCOUNT2);
    subscriptionManagerPtr->unsubAccount(account2, session1);
    subscriptionManagerPtr->unsubProposedAccount(account2, session1);
    auto checkResult = [](json::object reportReturn, int result) {
        EXPECT_EQ(reportReturn["book_changes"], result);
        EXPECT_EQ(reportReturn["validations"], result);
        EXPECT_EQ(reportReturn["transactions_proposed"], result);
        EXPECT_EQ(reportReturn["transactions"], result);
        EXPECT_EQ(reportReturn["manifests"], result);
        EXPECT_EQ(reportReturn["accounts_proposed"], result);
        EXPECT_EQ(reportReturn["account"], result);
        EXPECT_EQ(reportReturn["books"], result);
    };
    checkResult(subscriptionManagerPtr->report(), 1);

    // count down when session disconnect
    session2.reset();
    checkResult(subscriptionManagerPtr->report(), 0);
}

TEST_F(SubscriptionManagerTest, ManifestTest)
{
    constexpr static auto dummyManifest = R"({"manifest":"test"})";
    EXPECT_CALL(*sessionPtr, send(SharedStringJsonEq(dummyManifest))).Times(1);
    subscriptionManagerPtr->subManifest(session);
    subscriptionManagerPtr->forwardManifest(json::parse(dummyManifest).get_object());

    EXPECT_CALL(*sessionPtr, send(SharedStringJsonEq(dummyManifest))).Times(0);
    subscriptionManagerPtr->unsubManifest(session);
    subscriptionManagerPtr->forwardManifest(json::parse(dummyManifest).get_object());
}

TEST_F(SubscriptionManagerTest, ValidationTest)
{
    constexpr static auto dummy = R"({"validation":"test"})";
    EXPECT_CALL(*sessionPtr, send(SharedStringJsonEq(dummy))).Times(1);
    subscriptionManagerPtr->subValidation(session);
    subscriptionManagerPtr->forwardValidation(json::parse(dummy).get_object());

    EXPECT_CALL(*sessionPtr, send(SharedStringJsonEq(dummy))).Times(0);
    subscriptionManagerPtr->unsubValidation(session);
    subscriptionManagerPtr->forwardValidation(json::parse(dummy).get_object());
}

TEST_F(SubscriptionManagerTest, BookChangesTest)
{
    subscriptionManagerPtr->subBookChanges(session);
    EXPECT_EQ(subscriptionManagerPtr->report()["book_changes"], 1);

    auto const ledgerHeader = CreateLedgerHeader(LEDGERHASH, 32);
    auto transactions = std::vector<TransactionAndMetadata>{};
    auto trans1 = TransactionAndMetadata();
    ripple::STObject const obj = CreatePaymentTransactionObject(ACCOUNT1, ACCOUNT2, 1, 1, 32);
    trans1.transaction = obj.getSerializer().peekData();
    trans1.ledgerSequence = 32;
    ripple::STObject const metaObj = CreateMetaDataForBookChange(CURRENCY, ISSUER, 22, 1, 3, 3, 1);
    trans1.metadata = metaObj.getSerializer().peekData();
    transactions.push_back(trans1);
    constexpr static auto bookChangePublish =
        R"({
            "type":"bookChanges",
            "ledger_index":32,
            "ledger_hash":"4BC50C9B0D8515D3EAAE1E74B29A95804346C491EE1A95BF25E4AAB854A6A652",
            "ledger_time":0,
            "changes":
            [
                {
                    "currency_a":"XRP_drops",
                    "currency_b":"rK9DrarGKnVEo2nYp5MfVRXRYf5yRX3mwD/0158415500000000C1F76FF6ECB0BAC600000000",
                    "volume_a":"2",
                    "volume_b":"2",
                    "high":"-1",
                    "low":"-1",
                    "open":"-1",
                    "close":"-1"
                }
            ]
        })";
    EXPECT_CALL(*sessionPtr, send(SharedStringJsonEq(bookChangePublish))).Times(1);

    subscriptionManagerPtr->pubBookChanges(ledgerHeader, transactions);

    subscriptionManagerPtr->unsubBookChanges(session);
    EXPECT_EQ(subscriptionManagerPtr->report()["book_changes"], 0);
}

TEST_F(SubscriptionManagerTest, LedgerTest)
{
    backend->setRange(10, 30);
    auto const ledgerHeader = CreateLedgerHeader(LEDGERHASH, 30);
    EXPECT_CALL(*backend, fetchLedgerBySequence).WillOnce(testing::Return(ledgerHeader));

    auto const feeBlob = CreateLegacyFeeSettingBlob(1, 2, 3, 4, 0);
    EXPECT_CALL(*backend, doFetchLedgerObject).WillOnce(testing::Return(feeBlob));
    // check the function response
    // Information about the ledgers on hand and current fee schedule. This
    // includes the same fields as a ledger stream message, except that it omits
    // the type and txn_count fields
    constexpr static auto LedgerResponse =
        R"({
            "validated_ledgers":"10-30",
            "ledger_index":30,
            "ledger_hash":"4BC50C9B0D8515D3EAAE1E74B29A95804346C491EE1A95BF25E4AAB854A6A652",
            "ledger_time":0,
            "fee_base":1,
            "reserve_base":3,
            "reserve_inc":2
        })";
    boost::asio::io_context ctx;
    boost::asio::spawn(ctx, [this](boost::asio::yield_context yield) {
        auto const res = subscriptionManagerPtr->subLedger(yield, session);
        // check the response
        EXPECT_EQ(res, json::parse(LedgerResponse));
    });
    ctx.run();
    EXPECT_EQ(subscriptionManagerPtr->report()["ledger"], 1);

    // test publish
    auto const ledgerHeader2 = CreateLedgerHeader(LEDGERHASH, 31);
    auto fee2 = ripple::Fees();
    fee2.reserve = 10;
    constexpr static auto ledgerPub =
        R"({
            "type":"ledgerClosed",
            "ledger_index":31,
            "ledger_hash":"4BC50C9B0D8515D3EAAE1E74B29A95804346C491EE1A95BF25E4AAB854A6A652",
            "ledger_time":0,
            "fee_base":0,
            "reserve_base":10,
            "reserve_inc":0,
            "validated_ledgers":"10-31",
            "txn_count":8
        })";
    EXPECT_CALL(*sessionPtr, send(SharedStringJsonEq(ledgerPub))).Times(1);
    subscriptionManagerPtr->pubLedger(ledgerHeader2, fee2, "10-31", 8);

    // test unsub
    subscriptionManagerPtr->unsubLedger(session);
    EXPECT_EQ(subscriptionManagerPtr->report()["ledger"], 0);
}

TEST_F(SubscriptionManagerTest, TransactionTest)
{
    auto const issue1 = GetIssue(CURRENCY, ISSUER);
    auto const account = GetAccountIDWithString(ISSUER);
    ripple::Book const book{ripple::xrpIssue(), issue1};
    subscriptionManagerPtr->subBook(book, session);
    subscriptionManagerPtr->subTransactions(session);
    subscriptionManagerPtr->subAccount(account, session);
    EXPECT_EQ(subscriptionManagerPtr->report()["account"], 1);
    EXPECT_EQ(subscriptionManagerPtr->report()["transactions"], 1);
    EXPECT_EQ(subscriptionManagerPtr->report()["books"], 1);

    auto const ledgerHeader = CreateLedgerHeader(LEDGERHASH, 33);
    auto trans1 = TransactionAndMetadata();
    auto obj = CreatePaymentTransactionObject(ACCOUNT1, ACCOUNT2, 1, 1, 32);
    trans1.transaction = obj.getSerializer().peekData();
    trans1.ledgerSequence = 32;

    auto const metaObj = CreateMetaDataForBookChange(CURRENCY, ISSUER, 22, 3, 1, 1, 3);
    trans1.metadata = metaObj.getSerializer().peekData();
    constexpr static auto OrderbookPublish =
        R"({
            "transaction":
            {
                "Account":"rf1BiGeXwwQoi8Z2ueFYTEXSwuJYfV2Jpn",
                "Amount":"1",
                "DeliverMax":"1",
                "Destination":"rLEsXccBGNR3UPuPu2hUXPjziKC3qKSBun",
                "Fee":"1",
                "Sequence":32,
                "SigningPubKey":"74657374",
                "TransactionType":"Payment",
                "hash":"51D2AAA6B8E4E16EF22F6424854283D8391B56875858A711B8CE4D5B9A422CC2",
                "date":0
            },
            "meta":
            {
                "AffectedNodes":
                [
                    {
                        "ModifiedNode":
                        {
                            "FinalFields":
                            {
                                "TakerGets":"3",
                                "TakerPays":
                                {
                                    "currency":"0158415500000000C1F76FF6ECB0BAC600000000",
                                    "issuer":"rK9DrarGKnVEo2nYp5MfVRXRYf5yRX3mwD",
                                    "value":"1"
                                }
                            },
                            "LedgerEntryType":"Offer",
                            "PreviousFields":
                            {
                                "TakerGets":"1",
                                "TakerPays":
                                {
                                    "currency":"0158415500000000C1F76FF6ECB0BAC600000000",
                                    "issuer":"rK9DrarGKnVEo2nYp5MfVRXRYf5yRX3mwD",
                                    "value":"3"
                                }
                            }
                        }
                    }
                ],
                "TransactionIndex":22,
                "TransactionResult":"tesSUCCESS",
                "delivered_amount":"unavailable"
            },
            "type":"transaction",
            "validated":true,
            "status":"closed",
            "ledger_index":33,
            "ledger_hash":"4BC50C9B0D8515D3EAAE1E74B29A95804346C491EE1A95BF25E4AAB854A6A652",
            "engine_result_code":0,
            "engine_result":"tesSUCCESS",
            "close_time_iso": "2000-01-01T00:00:00Z",
            "engine_result_message":"The transaction was applied. Only final in a validated ledger."
        })";
    EXPECT_CALL(*sessionPtr, send(SharedStringJsonEq(OrderbookPublish))).Times(3);
    subscriptionManagerPtr->pubTransaction(trans1, ledgerHeader);

    subscriptionManagerPtr->unsubBook(book, session);
    subscriptionManagerPtr->unsubTransactions(session);
    subscriptionManagerPtr->unsubAccount(account, session);
    EXPECT_EQ(subscriptionManagerPtr->report()["account"], 0);
    EXPECT_EQ(subscriptionManagerPtr->report()["transactions"], 0);
    EXPECT_EQ(subscriptionManagerPtr->report()["books"], 0);
}

TEST_F(SubscriptionManagerTest, ProposedTransactionTest)
{
    auto const account = GetAccountIDWithString(ACCOUNT1);
    subscriptionManagerPtr->subProposedAccount(account, session);
    subscriptionManagerPtr->subProposedTransactions(session);
    EXPECT_EQ(subscriptionManagerPtr->report()["accounts_proposed"], 1);
    EXPECT_EQ(subscriptionManagerPtr->report()["transactions_proposed"], 1);

    constexpr static auto dummyTransaction =
        R"({
            "transaction":
            {
                "Account":"rf1BiGeXwwQoi8Z2ueFYTEXSwuJYfV2Jpn",
                "Destination":"rLEsXccBGNR3UPuPu2hUXPjziKC3qKSBun"
            }
        })";
    constexpr static auto OrderbookPublish =
        R"({
            "transaction":
            {
                "Account":"rf1BiGeXwwQoi8Z2ueFYTEXSwuJYfV2Jpn",
                "Amount":"1",
                "DeliverMax":"1",
                "Destination":"rLEsXccBGNR3UPuPu2hUXPjziKC3qKSBun",
                "Fee":"1",
                "Sequence":32,
                "SigningPubKey":"74657374",
                "TransactionType":"Payment",
                "hash":"51D2AAA6B8E4E16EF22F6424854283D8391B56875858A711B8CE4D5B9A422CC2",
                "date":0
            },
            "meta":
            {
                "AffectedNodes":
                [
                    {
                        "ModifiedNode":
                        {
                            "FinalFields":
                            {
                                "TakerGets":"3",
                                "TakerPays":
                                {
                                    "currency":"0158415500000000C1F76FF6ECB0BAC600000000",
                                    "issuer":"rf1BiGeXwwQoi8Z2ueFYTEXSwuJYfV2Jpn",
                                    "value":"1"
                                }
                            },
                            "LedgerEntryType":"Offer",
                            "PreviousFields":
                            {
                                "TakerGets":"1",
                                "TakerPays":
                                {
                                    "currency":"0158415500000000C1F76FF6ECB0BAC600000000",
                                    "issuer":"rf1BiGeXwwQoi8Z2ueFYTEXSwuJYfV2Jpn",
                                    "value":"3"
                                }
                            }
                        }
                    }
                ],
                "TransactionIndex":22,
                "TransactionResult":"tesSUCCESS",
                "delivered_amount":"unavailable"
            },
            "type":"transaction",
            "validated":true,
            "status":"closed",
            "ledger_index":33,
            "ledger_hash":"4BC50C9B0D8515D3EAAE1E74B29A95804346C491EE1A95BF25E4AAB854A6A652",
            "engine_result_code":0,
            "engine_result":"tesSUCCESS",
            "close_time_iso": "2000-01-01T00:00:00Z",
            "engine_result_message":"The transaction was applied. Only final in a validated ledger."
        })";
    EXPECT_CALL(*sessionPtr, send(SharedStringJsonEq(dummyTransaction))).Times(2);
    EXPECT_CALL(*sessionPtr, send(SharedStringJsonEq(OrderbookPublish))).Times(2);
    subscriptionManagerPtr->forwardProposedTransaction(json::parse(dummyTransaction).get_object());

    auto const ledgerHeader = CreateLedgerHeader(LEDGERHASH, 33);
    auto trans1 = TransactionAndMetadata();
    auto obj = CreatePaymentTransactionObject(ACCOUNT1, ACCOUNT2, 1, 1, 32);
    trans1.transaction = obj.getSerializer().peekData();
    trans1.ledgerSequence = 32;

    auto const metaObj = CreateMetaDataForBookChange(CURRENCY, ACCOUNT1, 22, 3, 1, 1, 3);
    trans1.metadata = metaObj.getSerializer().peekData();
    subscriptionManagerPtr->pubTransaction(trans1, ledgerHeader);

    // unsub account1
    subscriptionManagerPtr->unsubProposedAccount(account, session);
    EXPECT_EQ(subscriptionManagerPtr->report()["accounts_proposed"], 0);
    subscriptionManagerPtr->unsubProposedTransactions(session);
    EXPECT_EQ(subscriptionManagerPtr->report()["transactions_proposed"], 0);
}

TEST_F(SubscriptionManagerTest, DuplicateResponseSubTxAndProposedTx)
{
    subscriptionManagerPtr->subProposedTransactions(session);
    subscriptionManagerPtr->subTransactions(session);
    EXPECT_EQ(subscriptionManagerPtr->report()["transactions"], 1);
    EXPECT_EQ(subscriptionManagerPtr->report()["transactions_proposed"], 1);

    EXPECT_CALL(*sessionPtr, send(testing::_)).Times(2);

    auto const ledgerHeader = CreateLedgerHeader(LEDGERHASH, 33);
    auto trans1 = TransactionAndMetadata();
    auto obj = CreatePaymentTransactionObject(ACCOUNT1, ACCOUNT2, 1, 1, 32);
    trans1.transaction = obj.getSerializer().peekData();
    trans1.ledgerSequence = 32;

    auto const metaObj = CreateMetaDataForBookChange(CURRENCY, ACCOUNT1, 22, 3, 1, 1, 3);
    trans1.metadata = metaObj.getSerializer().peekData();
    subscriptionManagerPtr->pubTransaction(trans1, ledgerHeader);

    subscriptionManagerPtr->unsubTransactions(session);
    EXPECT_EQ(subscriptionManagerPtr->report()["transactions"], 0);
    subscriptionManagerPtr->unsubProposedTransactions(session);
    EXPECT_EQ(subscriptionManagerPtr->report()["transactions_proposed"], 0);
}

TEST_F(SubscriptionManagerTest, NoDuplicateResponseSubAccountAndProposedAccount)
{
    auto const account = GetAccountIDWithString(ACCOUNT1);
    subscriptionManagerPtr->subProposedAccount(account, session);
    subscriptionManagerPtr->subAccount(account, session);
    EXPECT_EQ(subscriptionManagerPtr->report()["accounts_proposed"], 1);
    EXPECT_EQ(subscriptionManagerPtr->report()["account"], 1);

    EXPECT_CALL(*sessionPtr, send(testing::_)).Times(1);

    auto const ledgerHeader = CreateLedgerHeader(LEDGERHASH, 33);
    auto trans1 = TransactionAndMetadata();
    auto obj = CreatePaymentTransactionObject(ACCOUNT1, ACCOUNT2, 1, 1, 32);
    trans1.transaction = obj.getSerializer().peekData();
    trans1.ledgerSequence = 32;

    auto const metaObj = CreateMetaDataForBookChange(CURRENCY, ACCOUNT1, 22, 3, 1, 1, 3);
    trans1.metadata = metaObj.getSerializer().peekData();
    subscriptionManagerPtr->pubTransaction(trans1, ledgerHeader);

    // unsub account1
    subscriptionManagerPtr->unsubProposedAccount(account, session);
    EXPECT_EQ(subscriptionManagerPtr->report()["accounts_proposed"], 0);
    subscriptionManagerPtr->unsubAccount(account, session);
    EXPECT_EQ(subscriptionManagerPtr->report()["account"], 0);
}

/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ordering/impl/on_demand_ordering_service_impl.hpp"

#include <memory>
#include <thread>

#include <gtest/gtest.h>
#include "backend/protobuf/proto_proposal_factory.hpp"
#include "builders/protobuf/transaction.hpp"
#include "datetime/time.hpp"
#include "framework/test_logger.hpp"
#include "interfaces/iroha_internal/transaction_batch_impl.hpp"
#include "module/irohad/ametsuchi/ametsuchi_mocks.hpp"
#include "module/shared_model/interface_mocks.hpp"
#include "module/shared_model/validators/validators.hpp"
#include "ordering/impl/on_demand_common.hpp"

using namespace iroha;
using namespace iroha::ordering;
using namespace iroha::ordering::transport;

using testing::_;
using testing::A;
using testing::ByMove;
using testing::Invoke;
using testing::Matcher;
using testing::NiceMock;
using testing::Ref;
using testing::Return;

using shared_model::interface::Proposal;
using shared_model::validation::MockValidator;
using MockProposalValidator = MockValidator<Proposal>;

class OnDemandOsTest : public ::testing::Test {
 public:
  std::shared_ptr<OnDemandOrderingService> os;
  const uint64_t transaction_limit = 20;
  const uint32_t proposal_limit = 5;
  const consensus::Round initial_round = {2, kFirstRejectRound},
                         target_round = {4, kNextCommitRoundConsumer},
                         commit_round = {3, kFirstRejectRound},
                         reject_round = {2, kNextRejectRoundConsumer};
  NiceMock<iroha::ametsuchi::MockTxPresenceCache> *mock_cache;

  void SetUp() override {
    // TODO: nickaleks IR-1811 use mock factory
    auto factory = std::make_unique<
        shared_model::proto::ProtoProposalFactory<MockProposalValidator>>();
    auto tx_cache =
        std::make_unique<NiceMock<iroha::ametsuchi::MockTxPresenceCache>>();
    mock_cache = tx_cache.get();
    // every batch is new by default
    ON_CALL(
        *mock_cache,
        check(
            testing::Matcher<const shared_model::interface::TransactionBatch &>(
                _)))
        .WillByDefault(Return(std::vector<iroha::ametsuchi::TxCacheStatusType>{
            iroha::ametsuchi::tx_cache_status_responses::Missing()}));
    os = std::make_shared<OnDemandOrderingServiceImpl>(
        transaction_limit,
        std::move(factory),
        std::move(tx_cache),
        getTestLogger("OdOrderingService"),
        proposal_limit,
        initial_round);
  }

  /**
   * Generate transactions with provided range
   * @param range - pair of [from, to)
   */
  void generateTransactionsAndInsert(std::pair<uint64_t, uint64_t> range) {
    os->onBatches(generateTransactions(range));
  }

  OnDemandOrderingService::CollectionType generateTransactions(
      std::pair<uint64_t, uint64_t> range,
      shared_model::interface::types::TimestampType now = iroha::time::now()) {
    OnDemandOrderingService::CollectionType collection;

    for (auto i = range.first; i < range.second; ++i) {
      collection.push_back(
          std::make_unique<shared_model::interface::TransactionBatchImpl>(
              shared_model::interface::types::SharedTxsCollectionType{
                  std::make_unique<shared_model::proto::Transaction>(
                      shared_model::proto::TransactionBuilder()
                          .createdTime(now + i)
                          .creatorAccountId("foo@bar")
                          .createAsset("asset", "domain", 1)
                          .quorum(1)
                          .build()
                          .signAndAddSignature(
                              shared_model::crypto::DefaultCryptoAlgorithmType::
                                  generateKeypair())
                          .finish())}));
    }
    return collection;
  }

  std::unique_ptr<Proposal> makeMockProposal() {
    auto proposal = std::make_unique<NiceMock<MockProposal>>();
    // TODO: nickaleks IR-1811 clone should return initialized mock
    ON_CALL(*proposal, clone()).WillByDefault(Return(new MockProposal()));

    return proposal;
  }
};

/**
 * @given initialized on-demand OS
 * @when  don't send transactions
 * AND initiate next round
 * @then  check that previous round doesn't have proposal
 */
TEST_F(OnDemandOsTest, EmptyRound) {
  ASSERT_FALSE(os->onRequestProposal(initial_round));

  os->onCollaborationOutcome(commit_round);

  ASSERT_FALSE(os->onRequestProposal(initial_round));
}

/**
 * @given initialized on-demand OS
 * @when  send number of transactions less that limit
 * AND initiate next round
 * @then  check that previous round has all transaction
 */
TEST_F(OnDemandOsTest, NormalRound) {
  generateTransactionsAndInsert({1, 2});

  os->onCollaborationOutcome(commit_round);

  ASSERT_TRUE(os->onRequestProposal(target_round));
}

/**
 * @given initialized on-demand OS
 * @when  send number of transactions greater that limit
 * AND initiate next round
 * @then  check that previous round has only limit of transactions
 * AND the rest of transactions isn't appeared in next after next round
 */
TEST_F(OnDemandOsTest, OverflowRound) {
  generateTransactionsAndInsert({1, transaction_limit * 2});

  os->onCollaborationOutcome(commit_round);

  ASSERT_TRUE(os->onRequestProposal(target_round));
  ASSERT_EQ(transaction_limit,
            (*os->onRequestProposal(target_round))->transactions().size());
}

/**
 * @given initialized on-demand OS
 * @when  send transactions from different threads
 * AND initiate next round
 * @then  check that all transactions appear in proposal
 */
TEST_F(OnDemandOsTest, DISABLED_ConcurrentInsert) {
  auto large_tx_limit = 10000u;
  auto factory = std::make_unique<
      shared_model::proto::ProtoProposalFactory<MockProposalValidator>>();
  auto tx_cache =
      std::make_unique<NiceMock<iroha::ametsuchi::MockTxPresenceCache>>();
  os = std::make_shared<OnDemandOrderingServiceImpl>(
      large_tx_limit,
      std::move(factory),
      std::move(tx_cache),
      getTestLogger("OdOrderingService"),
      proposal_limit,
      initial_round);

  auto call = [this](auto bounds) {
    for (auto i = bounds.first; i < bounds.second; ++i) {
      this->generateTransactionsAndInsert({i, i + 1});
    }
  };

  std::thread one(call, std::make_pair(0u, large_tx_limit / 2));
  std::thread two(call, std::make_pair(large_tx_limit / 2, large_tx_limit));
  one.join();
  two.join();
  os->onCollaborationOutcome(commit_round);
  ASSERT_EQ(large_tx_limit,
            os->onRequestProposal(target_round).get()->transactions().size());
}

/**
 * @given initialized on-demand OS
 * @when  insert commit round and then proposal_limit reject rounds
 * @then  first proposal still not expired
 */
TEST_F(OnDemandOsTest, Erase) {
  generateTransactionsAndInsert({1, 2});
  os->onCollaborationOutcome(
      {commit_round.block_round, commit_round.reject_round});
  ASSERT_TRUE(os->onRequestProposal(
      {commit_round.block_round + 1, commit_round.reject_round}));

  for (auto i = commit_round.reject_round;
       i < commit_round.reject_round + proposal_limit;
       ++i) {
    generateTransactionsAndInsert({1, 2});
    os->onCollaborationOutcome({commit_round.block_round + 1, i});
  }
  ASSERT_TRUE(os->onRequestProposal(
      {commit_round.block_round + 1, commit_round.reject_round}));
}

/**
 * @given initialized on-demand OS @and some transactions are sent to it
 * @when proposal is requested after calling onCollaborationOutcome
 * @then check that proposal factory is called and returns a proposal
 */
TEST_F(OnDemandOsTest, UseFactoryForProposal) {
  auto factory = std::make_unique<MockUnsafeProposalFactory>();
  auto mock_factory = factory.get();
  auto tx_cache =
      std::make_unique<NiceMock<iroha::ametsuchi::MockTxPresenceCache>>();
  ON_CALL(*tx_cache,
          check(A<const shared_model::interface::TransactionBatch &>()))
      .WillByDefault(Invoke([](const auto &batch) {
        iroha::ametsuchi::TxPresenceCache::BatchStatusCollectionType result;
        std::transform(
            batch.transactions().begin(),
            batch.transactions().end(),
            std::back_inserter(result),
            [](auto &tx) {
              return iroha::ametsuchi::tx_cache_status_responses::Missing{
                  tx->hash()};
            });
        return result;
      }));
  os = std::make_shared<OnDemandOrderingServiceImpl>(
      transaction_limit,
      std::move(factory),
      std::move(tx_cache),
      getTestLogger("OdOrderingService"),
      proposal_limit,
      initial_round);

  EXPECT_CALL(*mock_factory, unsafeCreateProposal(_, _, _))
      .WillOnce(Return(ByMove(makeMockProposal())))
      .WillOnce(Return(ByMove(makeMockProposal())));

  generateTransactionsAndInsert({1, 2});

  os->onCollaborationOutcome(commit_round);

  ASSERT_TRUE(os->onRequestProposal(target_round));
}

// Return matcher for batch, which passes it by const &
// used when passing batch as an argument to check() in transaction cache
auto batchRef(const shared_model::interface::TransactionBatch &batch) {
  return Matcher<const shared_model::interface::TransactionBatch &>(Ref(batch));
}

/**
 * @given initialized on-demand OS
 * @when add a batch which was already commited
 * @then the batch is not present in a proposal
 */
TEST_F(OnDemandOsTest, AlreadyProcessedProposalDiscarded) {
  auto batches = generateTransactions({1, 2});
  auto &batch = *batches.at(0);

  EXPECT_CALL(*mock_cache, check(batchRef(batch)))
      .WillOnce(Return(std::vector<iroha::ametsuchi::TxCacheStatusType>{
          iroha::ametsuchi::tx_cache_status_responses::Committed()}));

  os->onBatches(batches);

  os->onCollaborationOutcome(commit_round);

  auto proposal = os->onRequestProposal(initial_round);

  EXPECT_FALSE(proposal);
}

/**
 * @given initialized on-demand OS
 * @when add a batch with new transaction
 * @then batch is present in a proposal
 */
TEST_F(OnDemandOsTest, PassMissingTransaction) {
  auto batches = generateTransactions({1, 2});
  auto &batch = *batches.at(0);

  EXPECT_CALL(*mock_cache, check(batchRef(batch)))
      .WillOnce(Return(std::vector<iroha::ametsuchi::TxCacheStatusType>{
          iroha::ametsuchi::tx_cache_status_responses::Missing()}));

  os->onBatches(batches);

  os->onCollaborationOutcome(commit_round);

  auto proposal = os->onRequestProposal(target_round);

  // since we only sent one transaction,
  // if the proposal is present, there is no need to check for that specific tx
  EXPECT_TRUE(proposal);
}

/**
 * @given initialized on-demand OS
 * @when add 3 batches, with second one being already commited
 * @then 2 new batches are in a proposal and already commited batch is discarded
 */
TEST_F(OnDemandOsTest, SeveralTransactionsOneCommited) {
  auto batches = generateTransactions({1, 4});
  auto &batch1 = *batches.at(0);
  auto &batch2 = *batches.at(1);
  auto &batch3 = *batches.at(2);

  EXPECT_CALL(*mock_cache, check(batchRef(batch1)))
      .WillOnce(Return(std::vector<iroha::ametsuchi::TxCacheStatusType>{
          iroha::ametsuchi::tx_cache_status_responses::Missing()}));
  EXPECT_CALL(*mock_cache, check(batchRef(batch2)))
      .WillOnce(Return(std::vector<iroha::ametsuchi::TxCacheStatusType>{
          iroha::ametsuchi::tx_cache_status_responses::Committed()}));
  EXPECT_CALL(*mock_cache, check(batchRef(batch3)))
      .WillOnce(Return(std::vector<iroha::ametsuchi::TxCacheStatusType>{
          iroha::ametsuchi::tx_cache_status_responses::Missing()}));

  os->onBatches(batches);

  os->onCollaborationOutcome(commit_round);

  auto proposal = os->onRequestProposal(target_round);
  const auto &txs = proposal->get()->transactions();
  auto &batch2_tx = *batch2.transactions().at(0);

  EXPECT_TRUE(proposal);
  EXPECT_EQ(boost::size(txs), 2);
  // already processed transaction is no present in the proposal
  EXPECT_TRUE(std::find(txs.begin(), txs.end(), batch2_tx) == txs.end());
}

/**
 * @given initialized on-demand OS with a batch in collection
 * @when the same batch arrives, round is closed, proposal is requested
 * @then the proposal contains the batch once
 */
TEST_F(OnDemandOsTest, DuplicateTxTest) {
  auto now = iroha::time::now();
  auto txs1 = generateTransactions({1, 2}, now);
  os->onBatches(txs1);

  auto txs2 = generateTransactions({1, 2}, now);
  os->onBatches(txs2);
  os->onCollaborationOutcome(commit_round);
  auto proposal = os->onRequestProposal(target_round);

  ASSERT_EQ(1, boost::size((*proposal)->transactions()));
}

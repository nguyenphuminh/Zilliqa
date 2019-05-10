/*
 * Copyright (C) 2019 Zilliqa
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <array>
#include <chrono>
#include <functional>
#include <thread>

#include "Node.h"
#include "common/Constants.h"
#include "common/Messages.h"
#include "common/Serializable.h"
#include "libCrypto/Sha2.h"
#include "libMediator/Mediator.h"
#include "libMessage/Messenger.h"
#include "libUtils/BitVector.h"
#include "libUtils/DataConversion.h"
#include "libUtils/DetachedFunction.h"
#include "libUtils/Logger.h"
#include "libUtils/TimeLockedFunction.h"
#include "libUtils/TimeUtils.h"
#include "libUtils/TimestampVerifier.h"

using namespace std;

void Node::UpdateDSCommitteeAfterFallback(const uint32_t& shard_id,
                                          const PubKey& leaderPubKey,
                                          const Peer& leaderNetworkInfo,
                                          DequeOfNode& dsComm,
                                          const DequeOfShard& shards) {
  dsComm.clear();
  for (auto const& shardNode : shards[shard_id]) {
    if (std::get<SHARD_NODE_PUBKEY>(shardNode) == leaderPubKey &&
        std::get<SHARD_NODE_PEER>(shardNode) == leaderNetworkInfo) {
      dsComm.push_front({leaderPubKey, leaderNetworkInfo});
    } else {
      dsComm.push_back({std::get<SHARD_NODE_PUBKEY>(shardNode),
                        std::get<SHARD_NODE_PEER>(shardNode)});
    }
  }
}

bool Node::VerifyFallbackBlockCoSignature(const FallbackBlock& fallbackblock) {
  LOG_MARKER();

  unsigned int index = 0;
  unsigned int count = 0;

  uint32_t shard_id = fallbackblock.GetHeader().GetShardId();

  const vector<bool>& B2 = fallbackblock.GetB2();
  if (m_mediator.m_ds->m_shards[shard_id].size() != B2.size()) {
    LOG_GENERAL(WARNING,
                "Mismatch: shard "
                    << fallbackblock.GetHeader().GetShardId()
                    << " size = " << m_mediator.m_ds->m_shards[shard_id].size()
                    << ", co-sig bitmap size = " << B2.size());
    return false;
  }

  // Generate the aggregated key
  vector<PubKey> keys;

  for (auto const& shardNode : m_mediator.m_ds->m_shards[shard_id]) {
    if (B2.at(index)) {
      keys.emplace_back(std::get<SHARD_NODE_PUBKEY>(shardNode));
      count++;
    }
    index++;
  }

  if (count != ConsensusCommon::NumForConsensus(B2.size())) {
    LOG_GENERAL(WARNING, "Cosig was not generated by enough nodes");
    return false;
  }

  shared_ptr<PubKey> aggregatedKey = MultiSig::AggregatePubKeys(keys);
  if (aggregatedKey == nullptr) {
    LOG_GENERAL(WARNING, "Aggregated key generation failed");
    return false;
  }

  // Verify the collective signature
  bytes message;
  if (!fallbackblock.GetHeader().Serialize(message, 0)) {
    LOG_GENERAL(WARNING, "FallbackBlockHeader serialization failed");
    return false;
  }
  fallbackblock.GetCS1().Serialize(message, message.size());
  BitVector::SetBitVector(message, message.size(), fallbackblock.GetB1());
  if (!MultiSig::GetInstance().MultiSigVerify(
          message, 0, message.size(), fallbackblock.GetCS2(), *aggregatedKey)) {
    LOG_GENERAL(WARNING, "Cosig verification failed. Pubkeys");
    for (auto& kv : keys) {
      LOG_GENERAL(WARNING, kv);
    }
    return false;
  }

  return true;
}

bool Node::ProcessFallbackBlock(const bytes& message, unsigned int cur_offset,
                                [[gnu::unused]] const Peer& from) {
  // Message = [Fallback block]
  LOG_MARKER();

  // CheckState
  if (!CheckState(PROCESS_FALLBACKBLOCK)) {
    LOG_GENERAL(INFO,
                "Not in status for ProcessingFallbackBlock, "
                "wait state changing for "
                    << FALLBACK_EXTRA_TIME << " seconds");
    std::unique_lock<std::mutex> cv_lk(m_MutexCVFallbackBlock);
    if (cv_fallbackBlock.wait_for(
            cv_lk, std::chrono::seconds(FALLBACK_EXTRA_TIME),
            [this] { return m_state == WAITING_FALLBACKBLOCK; })) {
      LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
                "Successfully transit to waiting_fallbackblock or I am in the "
                "correct state.");
    } else {
      return false;
    }
  }

  FallbackBlock fallbackblock;

  if (!Messenger::GetNodeFallbackBlock(message, cur_offset, fallbackblock)) {
    LOG_EPOCH(WARNING, m_mediator.m_currentEpochNum,
              "Messenger::GetNodeFallbackBlock failed.");
    return false;
  }

  if (fallbackblock.GetHeader().GetVersion() != FALLBACKBLOCK_VERSION) {
    LOG_CHECK_FAIL("Fallback Block version",
                   fallbackblock.GetHeader().GetVersion(),
                   FALLBACKBLOCK_VERSION);
    return false;
  }

  if (!m_mediator.CheckWhetherBlockIsLatest(
          fallbackblock.GetHeader().GetFallbackDSEpochNo(),
          fallbackblock.GetHeader().GetFallbackEpochNo())) {
    LOG_GENERAL(WARNING,
                "ProcessFallbackBlock CheckWhetherBlockIsLatest failed");
    return false;
  }

  BlockHash temp_blockHash = fallbackblock.GetHeader().GetMyHash();
  if (temp_blockHash != fallbackblock.GetBlockHash()) {
    LOG_GENERAL(WARNING,
                "Block Hash in Newly received FB Block doesn't match. "
                "Calculated: "
                    << temp_blockHash
                    << " Received: " << fallbackblock.GetBlockHash().hex());
    return false;
  }

  // Check timestamp
  if (!VerifyTimestamp(fallbackblock.GetTimestamp(),
                       CONSENSUS_OBJECT_TIMEOUT + FALLBACK_INTERVAL_WAITING +
                           FALLBACK_CHECK_INTERVAL + FALLBACK_EXTRA_TIME)) {
    return false;
  }

  // Check shard
  uint32_t shard_id = fallbackblock.GetHeader().GetShardId();
  {
    lock_guard<mutex> g(m_mediator.m_ds->m_mutexShards);

    if (shard_id >= m_mediator.m_ds->m_shards.size()) {
      LOG_GENERAL(WARNING,
                  "The shard doesn't exist here for this id " << shard_id);
      return false;
    }

    CommitteeHash committeeHash;
    if (!Messenger::GetShardHash(m_mediator.m_ds->m_shards.at(shard_id),
                                 committeeHash)) {
      LOG_EPOCH(WARNING, m_mediator.m_currentEpochNum,
                "Messenger::GetShardHash failed.");
      return false;
    }
    if (committeeHash != fallbackblock.GetHeader().GetCommitteeHash()) {
      LOG_GENERAL(WARNING, "Fallback committee hash mismatched"
                               << endl
                               << "expected: " << committeeHash << endl
                               << "received: "
                               << fallbackblock.GetHeader().GetCommitteeHash());
      return false;
    }

    // Check consensus leader network info and pubkey
    uint16_t leaderConsensusId =
        fallbackblock.GetHeader().GetLeaderConsensusId();
    if (leaderConsensusId >= m_mediator.m_ds->m_shards[shard_id].size()) {
      LOG_GENERAL(
          WARNING,
          "The consensusLeaderId "
              << leaderConsensusId
              << " is larger than the size of that shard member we have "
              << m_mediator.m_ds->m_shards[shard_id].size());
      return false;
    }

    const PubKey& leaderPubKey = fallbackblock.GetHeader().GetLeaderPubKey();
    const Peer& leaderNetworkInfo =
        fallbackblock.GetHeader().GetLeaderNetworkInfo();

    auto leader = make_tuple(leaderPubKey, leaderNetworkInfo, 0);

    auto found = std::find_if(m_mediator.m_ds->m_shards[shard_id].begin(),
                              m_mediator.m_ds->m_shards[shard_id].end(),
                              [&leader](const auto& item) {
                                return (std::get<SHARD_NODE_PUBKEY>(leader) ==
                                        std::get<SHARD_NODE_PUBKEY>(item)) &&
                                       (std::get<SHARD_NODE_PEER>(leader) ==
                                        std::get<SHARD_NODE_PEER>(item));
                              });
    if (found == m_mediator.m_ds->m_shards[shard_id].end()) {
      LOG_GENERAL(
          WARNING,
          "The expected consensus leader not found in sharding structure"
              << endl
              << "PubKey: " << leaderPubKey << endl
              << "Peer: " << leaderNetworkInfo);
      return false;
    }

    if (AccountStore::GetInstance().GetStateRootHash() !=
        fallbackblock.GetHeader().GetStateRootHash()) {
      LOG_GENERAL(WARNING,
                  "The state root hash mismatched"
                      << endl
                      << "expected: "
                      << AccountStore::GetInstance().GetStateRootHash().hex()
                      << endl
                      << "received: "
                      << fallbackblock.GetHeader().GetStateRootHash().hex());
      return false;
    }

    if (!VerifyFallbackBlockCoSignature(fallbackblock)) {
      LOG_EPOCH(WARNING, m_mediator.m_currentEpochNum,
                "FallbackBlock co-sig verification failed");
      return false;
    }

    uint64_t latestInd = m_mediator.m_blocklinkchain.GetLatestIndex() + 1;
    m_mediator.m_blocklinkchain.AddBlockLink(
        latestInd, fallbackblock.GetHeader().GetFallbackDSEpochNo(),
        BlockType::FB, fallbackblock.GetBlockHash());

    bytes dst;

    FallbackBlockWShardingStructure fbblockwshards(fallbackblock,
                                                   m_mediator.m_ds->m_shards);

    if (!fbblockwshards.Serialize(dst, 0)) {
      LOG_GENERAL(WARNING, "Failed to Serialize");
    } else {
      if (!BlockStorage::GetBlockStorage().PutFallbackBlock(
              fallbackblock.GetBlockHash(), dst)) {
        LOG_GENERAL(WARNING, "Unable to store FallbackBlock");
        return false;
      }
    }

    FallbackTimerPulse();
    {
      lock_guard<mutex> g(m_mediator.m_mutexDSCommittee);
      UpdateDSCommitteeAfterFallback(shard_id, leaderPubKey, leaderNetworkInfo,
                                     *m_mediator.m_DSCommittee,
                                     m_mediator.m_ds->m_shards);
    }

    auto writeStateToDisk = [this]() mutable -> void {
      if (!AccountStore::GetInstance().MoveUpdatesToDisk()) {
        LOG_GENERAL(WARNING, "MoveUpdatesToDisk failed, what to do?");
        return;
      }
      LOG_STATE("[FLBLK][" << setw(15) << left
                           << m_mediator.m_selfPeer.GetPrintableIPAddress()
                           << "]["
                           << m_mediator.m_txBlockChain.GetLastBlock()
                                      .GetHeader()
                                      .GetBlockNum() +
                                  1
                           << "] FINISH WRITE STATE TO DISK");
    };
    DetachedFunction(1, writeStateToDisk);
  }

  if (!LOOKUP_NODE_MODE) {
    if (BROADCAST_TREEBASED_CLUSTER_MODE) {
      // Avoid using the original message for broadcasting in case it contains
      // excess data beyond the FallbackBlock
      bytes message2 = {MessageType::NODE, NodeInstructionType::FALLBACKBLOCK};
      if (!Messenger::SetNodeFallbackBlock(message2, MessageOffset::BODY,
                                           fallbackblock)) {
        LOG_GENERAL(WARNING, "Messenger::SetNodeFallbackBlock failed");
      } else {
        SendFallbackBlockToOtherShardNodes(message2);
      }
    }

    // Clean processedTxn may have been produced during last microblock
    // consensus
    {
      lock_guard<mutex> g(m_mutexProcessedTransactions);
      m_processedTransactions.erase(m_mediator.m_currentEpochNum);
    }

    CleanCreatedTransaction();

    CleanMicroblockConsensusBuffer();

    AccountStore::GetInstance().InitTemp();

    InitiatePoW();
  } else {
    m_mediator.m_consensusID = 0;
    m_consensusLeaderID = 0;
  }

  LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
            "I am a node and my DS committee is successfully fallback to shard "
                << shard_id);

  return true;
}

void Node::SendFallbackBlockToOtherShardNodes(
    const bytes& fallbackblock_message) {
  LOG_MARKER();
  unsigned int cluster_size = NUM_FORWARDED_BLOCK_RECEIVERS_PER_SHARD;
  if (cluster_size <= NUM_DS_ELECTION) {
    LOG_GENERAL(
        WARNING,
        "Adjusting NUM_FORWARDED_BLOCK_RECEIVERS_PER_SHARD to be greater than "
        "NUM_DS_ELECTION. Why not correct the constant.xml next time.");
    cluster_size = NUM_DS_ELECTION + 1;
  }
  LOG_GENERAL(INFO,
              "Primary CLUSTER SIZE used is "
              "(NUM_FORWARDED_BLOCK_RECEIVERS_PER_SHARD):"
                  << cluster_size);
  SendBlockToOtherShardNodes(fallbackblock_message, cluster_size,
                             NUM_OF_TREEBASED_CHILD_CLUSTERS);
}

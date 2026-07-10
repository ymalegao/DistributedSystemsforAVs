/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "platform/consensus/ordering/pbft/message_manager.h"

#include <glog/logging.h>

#include <iostream>

#include "common/utils/utils.h"

namespace resdb {

namespace {

const char* RequestTypeName(int type) {
  switch (type) {
    case Request::TYPE_PRE_PREPARE:
      return "PRE_PREPARE";
    case Request::TYPE_PREPARE:
      return "PREPARE";
    case Request::TYPE_COMMIT:
      return "COMMIT";
    case Request::TYPE_RESPONSE:
      return "RESPONSE";
    default:
      return "OTHER";
  }
}

const char* TxnStatusName(TransactionStatue status) {
  switch (status) {
    case TransactionStatue::None:
      return "None";
    case TransactionStatue::Prepare:
      return "Prepare";
    case TransactionStatue::READY_PREPARE:
      return "READY_PREPARE";
    case TransactionStatue::READY_COMMIT:
      return "READY_COMMIT";
    case TransactionStatue::READY_EXECUTE:
      return "READY_EXECUTE";
    case TransactionStatue::EXECUTED:
      return "EXECUTED";
    default:
      return "UNKNOWN";
  }
}

bool ShouldDebugPbftRequest(const Request& request) {
  return request.seq() >= 2 || request.hash().rfind("omnet-tx-", 0) == 0;
}

bool IsVoteRequestType(int type) {
  return type == Request::TYPE_PREPARE || type == Request::TYPE_COMMIT;
}

void LogVoteDrop(int self_id, const Request& request, const char* reason,
                 const std::string& extra = "") {
  if (!IsVoteRequestType(request.type()) || !ShouldDebugPbftRequest(request)) {
    return;
  }
  std::cout << "[VOTE-DROP] self=" << self_id
            << " omnet_self=" << OmnetForcedView::ResdbSenderToOmnet(self_id)
            << " seq=" << request.seq()
            << " type=" << RequestTypeName(request.type())
            << " sender=" << request.sender_id()
            << " omnet_sender="
            << OmnetForcedView::ResdbSenderToOmnet(request.sender_id())
            << " hash=" << request.hash()
            << " reason=" << (reason ? reason : "?");
  if (!extra.empty()) {
    std::cout << " " << extra;
  }
  std::cout << "\n";
}

}  // namespace

MessageManager::MessageManager(
    const ResDBConfig& config,
    std::unique_ptr<TransactionManager> transaction_manager,
    CheckPointManager* checkpoint_manager, SystemInfo* system_info)
    : config_(config),
      queue_("executed"),
      system_info_(system_info),
      checkpoint_manager_(checkpoint_manager),
      transaction_executor_(std::make_unique<TransactionExecutor>(
          config,
          [&](std::unique_ptr<Request> request,
              std::unique_ptr<BatchUserResponse> resp_msg) {
            if (request->is_recovery()) {
              if (checkpoint_manager_) {
                checkpoint_manager_->AddCommitData(std::move(request));
              }
              return;
            }
            resp_msg->set_proxy_id(request->proxy_id());
            resp_msg->set_seq(request->seq());
            resp_msg->set_current_view(request->current_view());
            resp_msg->set_primary_id(GetCurrentPrimary());
            if (transaction_executor_->NeedResponse() &&
                resp_msg->proxy_id() != 0) {
              queue_.Push(std::move(resp_msg));
            }
            if (checkpoint_manager_) {
              checkpoint_manager_->AddCommitData(std::move(request));
            }
          },
          system_info_, std::move(transaction_manager))),
      collector_pool_(std::make_unique<LockFreeCollectorPool>(
          "txn", config_.GetMaxProcessTxn(), transaction_executor_.get(),
          config_.GetConfigData().enable_viewchange())) {
  global_stats_ = Stats::GetGlobalStats();
  transaction_executor_->SetSeqUpdateNotifyFunc(
      [&](uint64_t seq) { collector_pool_->Update(seq - 1); });
  checkpoint_manager_->SetExecutor(transaction_executor_.get());
  checkpoint_manager_->SetResetExecute(
      [&](uint64_t seq) { SetNextCommitSeq(seq); });
}

MessageManager::~MessageManager() {
  std::cerr << "[MM-STOP] calling executor Stop()\n" << std::flush;
  if (transaction_executor_) {
    transaction_executor_->Stop();
  }
  std::cerr << "[MM-STOP] DONE\n" << std::flush;
}

std::unique_ptr<BatchUserResponse> MessageManager::GetResponseMsg() {
  return queue_.Pop();
}

int64_t MessageManager::GetCurrentPrimary() const {
  return system_info_->GetPrimaryId();
}

uint64_t MessageManager ::GetCurrentView() const {
  return system_info_->GetCurrentView();
}

void MessageManager::SetNextSeq(uint64_t seq) {
  LOG(ERROR) << "set next old seq:" << next_seq_;
  next_seq_ = seq;
  LOG(ERROR) << "set next seq:" << next_seq_;
}

int64_t MessageManager::GetNextSeq() { return next_seq_; }

absl::StatusOr<uint64_t> MessageManager::AssignNextSeq() {
  std::unique_lock<std::mutex> lk(seq_mutex_);
  uint32_t max_executed_seq = transaction_executor_->GetMaxPendingExecutedSeq();
  global_stats_->SeqGap(next_seq_ - max_executed_seq);
  if (next_seq_ - max_executed_seq >
      static_cast<uint64_t>(config_.GetMaxProcessTxn())) {
    // LOG(ERROR) << "next_seq_: " << next_seq_ << " max_executed_seq: " <<
    // max_executed_seq;
    return absl::InvalidArgumentError("Seq has been used up.");
  }
  return next_seq_++;
}

std::vector<ReplicaInfo> MessageManager::GetReplicas() {
  return system_info_->GetReplicas();
}

// Check if the request is valid.
// 1. view is the same as the current view
// 2. seq is larger or equal than the next execute seq.
// 3. inside the water mark.
bool MessageManager::IsValidMsg(const Request& request) {
  if (request.type() == Request::TYPE_RESPONSE) {
    return true;
  }
  // view should be the same as the current one.
  if (static_cast<uint64_t>(request.current_view()) != GetCurrentView()) {
    LOG(ERROR) << "message view :[" << request.current_view()
               << "] is older than the cur view :[" << GetCurrentView() << "]";
    std::cout << "[PBFT-VALIDMSG] REJECT view=" << request.current_view()
              << " curView=" << GetCurrentView()
              << " type=" << request.type()
              << " seq=" << request.seq() << "\n";
    LogVoteDrop(config_.GetSelfInfo().id(), request, "no-view",
                "detail=view-mismatch cur_view=" +
                    std::to_string(GetCurrentView()));
    return false;
  }

  if (static_cast<uint64_t>(request.seq()) <
      transaction_executor_->GetMaxPendingExecutedSeq()) {
    std::cout << "[VALIDMSG-REJECT-SEQ] seq=" << request.seq()
              << " type=" << request.type()
              << " maxPendingExec=" << transaction_executor_->GetMaxPendingExecutedSeq()
              << " (seq too old)\n";
    LogVoteDrop(config_.GetSelfInfo().id(), request, "no-request",
                "detail=seq-too-old max_pending_exec=" +
                    std::to_string(
                        transaction_executor_->GetMaxPendingExecutedSeq()));
    return false;
  }

  return true;
}

void MessageManager::SetOmnetForcedViewRegistry(
    std::shared_ptr<OmnetForcedViewRegistry> registry) {
  forced_view_registry_ = std::move(registry);
}

bool MessageManager::HasForcedViewForRequest(const Request& request) {
  if (!forced_view_registry_) {
    if (ShouldDebugPbftRequest(request)) {
      std::cout << "[PBFT-FORCED-CHECK] self=" << config_.GetSelfInfo().id()
                << " seq=" << request.seq()
                << " type=" << RequestTypeName(request.type())
                << " hash=" << request.hash()
                << " result=miss"
                << " reason=no-registry\n";
    }
    return false;
  }
  auto view = forced_view_registry_->FindForRequest(request);
  if (ShouldDebugPbftRequest(request)) {
    std::cout << "[PBFT-FORCED-CHECK] self=" << config_.GetSelfInfo().id()
              << " omnet_self="
              << OmnetForcedView::ResdbSenderToOmnet(config_.GetSelfInfo().id())
              << " seq=" << request.seq()
              << " type=" << RequestTypeName(request.type())
              << " sender=" << request.sender_id()
              << " hash=" << request.hash()
              << " result=" << (view ? "hit" : "miss");
    if (view) {
      std::cout << " view_epoch=" << view->epoch
                << " view_seq=" << view->seq
                << " N=" << view->active_omnet_ids.size()
                << " quorum=" << view->quorum
                << " primary=r" << view->primary_omnet;
    }
    std::cout << "\n";
  }
  return view.has_value();
}

bool MessageManager::IsSelfActiveForRequest(const Request& request) {
  if (!forced_view_registry_) return true;
  auto view = forced_view_registry_->FindForRequest(request);
  if (!view) return true;
  return view->IsActiveOmnet(
      OmnetForcedView::ResdbSenderToOmnet(config_.GetSelfInfo().id()));
}

int MessageManager::QuorumForRequest(const Request& request) {
  if (forced_view_registry_) {
    auto view = forced_view_registry_->FindForRequest(request);
    if (view) {
      if (ShouldDebugPbftRequest(request)) {
        std::cout << "[PBFT-QUORUM] self=" << config_.GetSelfInfo().id()
                  << " seq=" << request.seq()
                  << " hash=" << request.hash()
                  << " source=forced"
                  << " quorum=" << view->quorum
                  << " N=" << view->active_omnet_ids.size()
                  << " epoch=" << view->epoch << "\n";
      }
      return view->quorum;
    }
  }
  if (ShouldDebugPbftRequest(request)) {
    std::cout << "[PBFT-QUORUM] self=" << config_.GetSelfInfo().id()
              << " seq=" << request.seq()
              << " hash=" << request.hash()
              << " source=static"
              << " quorum=" << config_.GetMinDataReceiveNum() << "\n";
  }
  return config_.GetMinDataReceiveNum();
}

bool MessageManager::IsSenderActiveForRequest(const Request& request) {
  if (!forced_view_registry_) return true;
  auto view = forced_view_registry_->FindForRequest(request);
  if (!view) return true;
  bool active = view->IsActiveResdbSender(request.sender_id());
  if (!active) {
    std::cout << "[ACTIVE-VOTE-DROP] sender=" << request.sender_id()
              << " omnet_sender="
              << OmnetForcedView::ResdbSenderToOmnet(request.sender_id())
              << " epoch=" << view->epoch
              << " seq=" << request.seq()
              << " hash=" << request.hash()
              << " reason=inactive\n";
  }
  return active;
}

bool MessageManager::MayConsensusChangeStatus(
    const Request& request, int type, int received_count,
    std::atomic<TransactionStatue>* status, bool ret) {
  const int quorum = QuorumForRequest(request);
  const TransactionStatue before =
      status ? status->load(std::memory_order_acquire) : TransactionStatue::None;
  bool changed = ret;
  switch (type) {
    case Request::TYPE_PRE_PREPARE:
      if (*status == TransactionStatue::None) {
        TransactionStatue old_status = TransactionStatue::None;
        changed = status->compare_exchange_strong(
            old_status, TransactionStatue::READY_PREPARE,
            std::memory_order_acq_rel, std::memory_order_acq_rel);
      }
      break;
    case Request::TYPE_PREPARE:
      if (*status == TransactionStatue::READY_PREPARE &&
          quorum <= received_count) {
        TransactionStatue old_status = TransactionStatue::READY_PREPARE;
        changed = status->compare_exchange_strong(
            old_status, TransactionStatue::READY_COMMIT,
            std::memory_order_acq_rel, std::memory_order_acq_rel);
      }
      break;
    case Request::TYPE_COMMIT:
      if (*status == TransactionStatue::READY_COMMIT &&
          quorum <= received_count) {
        TransactionStatue old_status = TransactionStatue::READY_COMMIT;
        changed = status->compare_exchange_strong(
            old_status, TransactionStatue::READY_EXECUTE,
            std::memory_order_acq_rel, std::memory_order_acq_rel);
      }
      break;
  }
  const bool forced = HasForcedViewForRequest(request);
  if ((forced || request.seq() >= 2) &&
      (type == Request::TYPE_PRE_PREPARE ||
       type == Request::TYPE_PREPARE ||
       type == Request::TYPE_COMMIT)) {
    const TransactionStatue after =
        status ? status->load(std::memory_order_acquire) : TransactionStatue::None;
    std::cout << "[PBFT-COUNT] self=" << config_.GetSelfInfo().id()
              << " omnet_self="
              << OmnetForcedView::ResdbSenderToOmnet(config_.GetSelfInfo().id())
              << " seq=" << request.seq()
              << " type=" << RequestTypeName(type)
              << " sender=" << request.sender_id()
              << " omnet_sender="
              << OmnetForcedView::ResdbSenderToOmnet(request.sender_id())
              << " hash=" << request.hash()
              << " count=" << received_count
              << " quorum=" << quorum
              << " status_before=" << TxnStatusName(before)
              << " status_after=" << TxnStatusName(after)
              << " changed=" << (changed ? 1 : 0)
              << " forced=" << (forced ? 1 : 0)
              << "\n";
  }
  return changed;
}

// Add commit messages and return the number of messages have been received.
// The commit messages only include post(pre-prepare), prepare and commit
// messages. Messages are handled by state (PREPARE,COMMIT,READY_EXECUTE).

// If there are enough messages and the state is changed after adding the
// message, return 1, otherwise return 0. Return -2 if the request is not valid.
CollectorResultCode MessageManager::AddConsensusMsg(
    const SignatureInfo& signature, std::unique_ptr<Request> request) {
  if (request == nullptr || !IsValidMsg(*request)) {
    LOG(ERROR) << " msg not invalid";
    std::cout << "[PBFT-ADDMSG] INVALID seq=" << (request ? request->seq() : 0)
              << " type=" << (request ? request->type() : -1) << "\n";
    return CollectorResultCode::INVALID;
  }

  int type = request->type();
  uint64_t seq = request->seq();
  const int sender_id = request->sender_id();
  const std::string hash = request->hash();
  const bool forced = HasForcedViewForRequest(*request);
  int resp_received_count = 0;
  int proxy_id = request->proxy_id();
  if (IsVoteRequestType(type) && !forced && forced_view_registry_ &&
      forced_view_registry_->HasAny() && ShouldDebugPbftRequest(*request)) {
    LogVoteDrop(config_.GetSelfInfo().id(), *request, "no-view",
                "detail=forced-view-miss");
  }
  if (!IsSenderActiveForRequest(*request)) {
    LogVoteDrop(config_.GetSelfInfo().id(), *request, "inactive-sender");
    if (forced || seq >= 2) {
      std::cout << "[PBFT-ADD-RESULT] self=" << config_.GetSelfInfo().id()
                << " seq=" << seq
                << " type=" << RequestTypeName(type)
                << " sender=" << sender_id
                << " hash=" << hash
                << " result=inactive-sender"
                << " forced=" << (forced ? 1 : 0)
                << "\n";
    }
    return CollectorResultCode::OK;
  }
  if (checkpoint_manager_->IsCommitted(seq)) {
    LOG(ERROR) << " seq:" << seq << " type:" << type << " has been committed";
    std::cout << "[ADDMSG-ALREADY-COMMITTED] seq=" << seq << " type=" << type << "\n";
    LogVoteDrop(config_.GetSelfInfo().id(), *request, "duplicate",
                "detail=already-committed");
    return CollectorResultCode::STATE_CHANGED;
  }

  TransactionCollector* collector = collector_pool_->GetCollector(seq);
  if (collector != nullptr && IsVoteRequestType(type) &&
      ShouldDebugPbftRequest(*request)) {
    if (!collector->HasMainRequest()) {
      LogVoteDrop(config_.GetSelfInfo().id(), *request, "no-request",
                  "detail=missing-pre-prepare");
    } else {
      const std::string main_hash = collector->MainRequestHash();
      if (!main_hash.empty() && main_hash != hash) {
        LogVoteDrop(config_.GetSelfInfo().id(), *request, "hash-mismatch",
                    "main_hash=" + main_hash);
      }
    }
    if (collector->HasVoteFrom(type, hash, sender_id)) {
      LogVoteDrop(config_.GetSelfInfo().id(), *request, "duplicate",
                  "detail=same-sender");
    }
  }
  int ret = collector->AddRequest(
      std::move(request), signature, type == Request::TYPE_PRE_PREPARE,
      [&](const Request& request, int received_count,
          TransactionCollector::CollectorDataType* data,
          std::atomic<TransactionStatue>* status, bool force) {
        if (MayConsensusChangeStatus(request, type, received_count, status,
                                     force)) {
          resp_received_count = 1;
        }
      });
  if (ret == 1) {
    SetLastCommittedTime(proxy_id);
  } else if (ret != 0) {
    LOG(ERROR) << " add request fail";
    Request drop_request;
    drop_request.set_type(type);
    drop_request.set_seq(seq);
    drop_request.set_sender_id(sender_id);
    drop_request.set_hash(hash);
    LogVoteDrop(config_.GetSelfInfo().id(), drop_request, "no-request",
                "detail=collector-reject ret=" + std::to_string(ret));
    if (forced || seq >= 2) {
      std::cout << "[PBFT-ADD-RESULT] self=" << config_.GetSelfInfo().id()
                << " seq=" << seq
                << " type=" << RequestTypeName(type)
                << " sender=" << sender_id
                << " hash=" << hash
                << " result=invalid"
                << " ret=" << ret
                << " status=" << TxnStatusName(collector->GetStatus())
                << " forced=" << (forced ? 1 : 0)
                << "\n";
    }
    return CollectorResultCode::INVALID;
  }
  if (resp_received_count > 0) {
    if (type == Request::TYPE_COMMIT) {
      if (checkpoint_manager_) {
        checkpoint_manager_->AddCommitState(seq);
      }
    }
    if (forced || seq >= 2) {
      std::cout << "[PBFT-ADD-RESULT] self=" << config_.GetSelfInfo().id()
                << " seq=" << seq
                << " type=" << RequestTypeName(type)
                << " sender=" << sender_id
                << " hash=" << hash
                << " result=state-changed"
                << " status=" << TxnStatusName(collector->GetStatus())
                << " forced=" << (forced ? 1 : 0)
                << "\n";
    }
    return CollectorResultCode::STATE_CHANGED;
  }
  if (forced || seq >= 2) {
    std::cout << "[PBFT-ADD-RESULT] self=" << config_.GetSelfInfo().id()
              << " seq=" << seq
              << " type=" << RequestTypeName(type)
              << " sender=" << sender_id
              << " hash=" << hash
              << " result=ok"
              << " status=" << TxnStatusName(collector->GetStatus())
              << " forced=" << (forced ? 1 : 0)
              << "\n";
  }
  return CollectorResultCode::OK;
}

std::vector<RequestInfo> MessageManager::GetPreparedProof(uint64_t seq) {
  return collector_pool_->GetCollector(seq)->GetPreparedProof();
}

int MessageManager::GetReplicaState(ReplicaState* state) {
  *state->mutable_replica_config() = config_.GetConfigData();
  return 0;
}

Storage* MessageManager::GetStorage() {
  return transaction_executor_->GetStorage();
}

void MessageManager::SetNextCommitSeq(int seq) {
  LOG(ERROR) << " set next commit seq:" << seq;
  std::cout << "[SET-NEXT-COMMIT-SEQ] seq=" << seq
            << " old_next_seq=" << next_seq_
            << " old_pending_exec=" << transaction_executor_->GetMaxPendingExecutedSeq() + 1
            << "\n";
  SetNextSeq(seq);
  SetHighestPreparedSeq(seq);
  collector_pool_->Reset(seq);
  checkpoint_manager_->SetLastCommit(seq - 1);
  std::cout << "[SET-NEXT-COMMIT-SEQ-DONE] seq=" << seq << "\n";
  return transaction_executor_->SetPendingExecutedSeq(seq);
}

void MessageManager::EnsureNextSeqAheadOfExecuted() {
  const uint64_t max_executed = transaction_executor_->GetMaxPendingExecutedSeq();
  uint64_t need = max_executed + 1;
  if (checkpoint_manager_) {
    need = std::max(need, checkpoint_manager_->GetLastCommit() + 1);
  }
  if (static_cast<uint64_t>(next_seq_) >= need) return;
  std::cout << "[SEQ-SYNC] advancing next_seq from " << next_seq_
            << " to " << need << " max_executed=" << max_executed
            << " last_commit="
            << (checkpoint_manager_ ? checkpoint_manager_->GetLastCommit() : 0)
            << "\n";
  SetNextSeq(need);
}

void MessageManager::AdvanceExecutorAfterGossipCancel(uint32_t cancelled_epoch) {
  // Ledger prefix per epoch: ORDER(e) then CANCEL(e). After gossip-adopting
  // CANCEL(e), the next executable PBFT seq is 2*(e+1)+1.
  const uint64_t min_next = 2ULL * (static_cast<uint64_t>(cancelled_epoch) + 1ULL) + 1ULL;
  const uint64_t current_next = transaction_executor_->GetMaxPendingExecutedSeq() + 1;
  const uint64_t target = std::max(current_next, min_next);
  transaction_executor_->AdvanceExecuteSeq(target);
  if (collector_pool_) {
    collector_pool_->Update(target - 1);
  }
  const uint64_t need = std::max(target, current_next);
  if (static_cast<uint64_t>(next_seq_) < need) {
    std::cout << "[SEQ-SYNC] gossip-cancel advancing next_seq from " << next_seq_
              << " to " << need << " cancelled_epoch=" << cancelled_epoch << "\n";
    SetNextSeq(need);
  }
}

void MessageManager::SetLastCommittedTime(uint64_t proxy_id) {
  lct_lock_.lock();
  last_committed_time_[proxy_id] = GetCurrentTime();
  lct_lock_.unlock();
}

uint64_t MessageManager::GetLastCommittedTime(uint64_t proxy_id) {
  lct_lock_.lock();
  auto value = last_committed_time_[proxy_id];
  lct_lock_.unlock();
  return value;
}

bool MessageManager::IsPreapared(uint64_t seq) {
  return collector_pool_->GetCollector(seq)->IsPrepared();
}

uint64_t MessageManager::GetHighestPreparedSeq() {
  return checkpoint_manager_->GetHighestPreparedSeq();
}

void MessageManager::SetHighestPreparedSeq(uint64_t seq) {
  return checkpoint_manager_->SetHighestPreparedSeq(seq);
}

void MessageManager::SetDuplicateManager(DuplicateManager* manager) {
  transaction_executor_->SetDuplicateManager(manager);
}

void MessageManager::SendResponse(std::unique_ptr<Request> request) {
  std::unique_ptr<BatchUserResponse> response =
      std::make_unique<BatchUserResponse>();
  response->set_createtime(GetCurrentTime());
  // response->set_local_id(batch_request.local_id());
  response->set_hash(request->hash());
  response->set_proxy_id(request->proxy_id());
  response->set_seq(request->seq());
  response->set_current_view(GetCurrentView());
  response->set_primary_id(GetCurrentPrimary());
  if (transaction_executor_->NeedResponse() && response->proxy_id() != 0) {
    queue_.Push(std::move(response));
  }
}

LockFreeCollectorPool* MessageManager::GetCollectorPool() {
  return collector_pool_.get();
}

}  // namespace resdb

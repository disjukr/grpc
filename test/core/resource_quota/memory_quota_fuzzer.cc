// Copyright 2021 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <map>

#include "src/core/lib/gpr/useful.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/resource_quota/memory_quota.h"
#include "src/libfuzzer/libfuzzer_macro.h"
#include "test/core/resource_quota/memory_quota_fuzzer.pb.h"

bool squelch = true;
bool leak_check = true;

namespace grpc_core {

namespace {
ReclamationPass MapReclamationPass(memory_quota_fuzzer::Reclaimer::Pass pass) {
  switch (pass) {
    case memory_quota_fuzzer::Reclaimer::BENIGN:
      return ReclamationPass::kBenign;
    case memory_quota_fuzzer::Reclaimer::IDLE:
      return ReclamationPass::kIdle;
    case memory_quota_fuzzer::Reclaimer::DESTRUCTIVE:
      return ReclamationPass::kDestructive;
    default:
      return ReclamationPass::kBenign;
  }
}

class Fuzzer {
 public:
  void Run(const memory_quota_fuzzer::Msg& msg) {
    grpc_core::ExecCtx exec_ctx;
    RunMsg(msg);
    memory_quotas_.clear();
    memory_allocators_.clear();
    allocations_.clear();
  }

 private:
  void RunMsg(const memory_quota_fuzzer::Msg& msg) {
    for (int i = 0; i < msg.actions_size(); ++i) {
      const auto& action = msg.actions(i);
      switch (action.action_type_case()) {
        case memory_quota_fuzzer::Action::kFlushExecCtx:
          ExecCtx::Get()->Flush();
          break;
        case memory_quota_fuzzer::Action::kCreateQuota:
          memory_quotas_.emplace(action.quota(), MemoryQuota());
          break;
        case memory_quota_fuzzer::Action::kDeleteQuota:
          memory_quotas_.erase(action.quota());
          break;
        case memory_quota_fuzzer::Action::kCreateAllocator:
          WithQuota(action.quota(), [this, action](MemoryQuota* q) {
            memory_allocators_.emplace(action.allocator(),
                                       q->CreateMemoryOwner());
          });
          break;
        case memory_quota_fuzzer::Action::kDeleteAllocator:
          memory_allocators_.erase(action.allocator());
          break;
        case memory_quota_fuzzer::Action::kSetQuotaSize:
          WithQuota(action.quota(), [action](MemoryQuota* q) {
            q->SetSize(Clamp(action.set_quota_size(), uint64_t{0},
                             uint64_t{std::numeric_limits<ssize_t>::max()}));
          });
          break;
        case memory_quota_fuzzer::Action::kRebindQuota:
          WithQuota(action.quota(), [this, action](MemoryQuota* q) {
            WithAllocator(action.allocator(),
                          [q](MemoryOwner* a) { a->Rebind(q); });
          });
          break;
        case memory_quota_fuzzer::Action::kCreateAllocation: {
          auto min = action.create_allocation().min();
          auto max = action.create_allocation().max();
          if (min > max) break;
          if (max > MemoryRequest::max_allowed_size()) break;
          MemoryRequest req(min, max);
          WithAllocator(
              action.allocator(), [this, action, req](MemoryOwner* a) {
                auto alloc = a->allocator()->MakeReservation(req);
                allocations_.emplace(action.allocation(), std::move(alloc));
              });
        } break;
        case memory_quota_fuzzer::Action::kDeleteAllocation:
          allocations_.erase(action.allocation());
          break;
        case memory_quota_fuzzer::Action::kPostReclaimer: {
          std::function<void(ReclamationSweep)> reclaimer;
          auto cfg = action.post_reclaimer();
          if (cfg.synchronous()) {
            reclaimer = [this, cfg](ReclamationSweep) { RunMsg(cfg.msg()); };
          } else {
            reclaimer = [cfg, this](ReclamationSweep sweep) {
              struct Args {
                ReclamationSweep sweep;
                memory_quota_fuzzer::Msg msg;
                Fuzzer* fuzzer;
              };
              auto* args = new Args{std::move(sweep), cfg.msg(), this};
              auto* closure = GRPC_CLOSURE_CREATE(
                  [](void* arg, grpc_error_handle) {
                    auto* args = static_cast<Args*>(arg);
                    args->fuzzer->RunMsg(args->msg);
                    delete args;
                  },
                  args, nullptr);
              ExecCtx::Get()->Run(DEBUG_LOCATION, closure, GRPC_ERROR_NONE);
            };
            auto pass = MapReclamationPass(cfg.pass());
            WithAllocator(action.allocator(),
                          [pass, reclaimer](MemoryOwner* a) {
                            a->PostReclaimer(pass, reclaimer);
                          });
          }
        } break;
        case memory_quota_fuzzer::Action::ACTION_TYPE_NOT_SET:
          break;
      }
    }
  }

  template <typename F>
  void WithQuota(int quota, F f) {
    auto it = memory_quotas_.find(quota);
    if (it == memory_quotas_.end()) return;
    f(&it->second);
  }

  template <typename F>
  void WithAllocator(int allocator, F f) {
    auto it = memory_allocators_.find(allocator);
    if (it == memory_allocators_.end()) return;
    f(&it->second);
  }

  std::map<int, MemoryQuota> memory_quotas_;
  std::map<int, MemoryOwner> memory_allocators_;
  std::map<int, MemoryAllocator::Reservation> allocations_;
};

}  // namespace

}  // namespace grpc_core

static void dont_log(gpr_log_func_args* /*args*/) {}

DEFINE_PROTO_FUZZER(const memory_quota_fuzzer::Msg& msg) {
  if (squelch) gpr_set_log_function(dont_log);
  gpr_log_verbosity_init();
  grpc_tracer_init();
  grpc_core::Fuzzer().Run(msg);
}

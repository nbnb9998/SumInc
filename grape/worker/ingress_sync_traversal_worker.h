
#ifndef GRAPE_WORKER_INGRESS_SYNC_TRAVERSAL_WORKER_H_
#define GRAPE_WORKER_INGRESS_SYNC_TRAVERSAL_WORKER_H_

#include <grape/fragment/loader.h>

#include <boost/mpi.hpp>
#include <boost/serialization/vector.hpp>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "flags.h"
#include "grape/app/traversal_app_base.h"
#include "grape/communication/communicator.h"
#include "grape/communication/sync_comm.h"
#include "grape/graph/adj_list.h"
#include "grape/parallel/parallel_engine.h"
#include "grape/parallel/parallel_message_manager.h"
#include "timer.h"

namespace grape {

template <typename FRAG_T, typename VALUE_T>
class IterateKernel;

/**
 * @brief A Worker manages the computation cycle. DefaultWorker is a kind of
 * worker for apps derived from AppBase.
 *
 * @tparam APP_T
 */

template <typename APP_T>
class IngressSyncTraversalWorker : public ParallelEngine {
  static_assert(std::is_base_of<TraversalAppBase<typename APP_T::fragment_t,
                                                 typename APP_T::value_t>,
                                APP_T>::value,
                "IngressSyncTraversalWorker should work with App");

 public:
  using fragment_t = typename APP_T::fragment_t;
  using value_t = typename APP_T::value_t;
  using delta_t = typename APP_T::delta_t;
  using vertex_t = typename APP_T::vertex_t;
  using message_manager_t = ParallelMessageManager;
  using oid_t = typename fragment_t::oid_t;
  using vid_t = typename APP_T::vid_t;

  IngressSyncTraversalWorker(std::shared_ptr<APP_T> app,
                             std::shared_ptr<fragment_t>& graph)
      : app_(app), fragment_(graph) {}

  void Init(const CommSpec& comm_spec,
            const ParallelEngineSpec& pe_spec = DefaultParallelEngineSpec()) {
    fragment_->PrepareToRunApp(APP_T::message_strategy,
                               APP_T::need_split_edges);

    comm_spec_ = comm_spec;

    // 等待所有worker执行完毕
    MPI_Barrier(comm_spec_.comm());

    // 初始化发消息相关的buffer
    messages_.Init(comm_spec_.comm());
    messages_.InitChannels(thread_num());
    communicator_.InitCommunicator(comm_spec.comm());

    InitParallelEngine(pe_spec);
    if (FLAGS_cilk) {
      LOG(INFO) << "Thread num: " << getWorkers();
    }
  }

  void deltaCompute() {
    IncFragmentBuilder<fragment_t> inc_fragment_builder(fragment_,
                                                        FLAGS_directed);

    if (comm_spec_.worker_id() == grape::kCoordinatorRank) {
      LOG(INFO) << "Parsing update file";
    }
    inc_fragment_builder.Init(FLAGS_efile_update);
    auto inner_vertices = fragment_->InnerVertices();
    auto outer_vertices = fragment_->OuterVertices();

    auto deleted_edges = inc_fragment_builder.GetDeletedEdgesGid();
    std::unordered_set<vid_t> local_gid_set;

    for (auto v : fragment_->Vertices()) {
      local_gid_set.insert(fragment_->Vertex2Gid(v));
    }

    auto vertices = fragment_->Vertices();
    DenseVertexSet<vid_t> curr_modified, next_modified, reset_vertices;

    curr_modified.Init(vertices);
    next_modified.Init(vertices);
    reset_vertices.Init(inner_vertices);  // Only used for counting purpose

    for (auto& pair : deleted_edges) {
      vid_t u_gid = pair.first, v_gid = pair.second;

      if (local_gid_set.find(u_gid) != local_gid_set.end() &&
          fragment_->IsInnerGid(v_gid)) {
        vertex_t u, v;
        CHECK(fragment_->Gid2Vertex(u_gid, u));
        CHECK(fragment_->Gid2Vertex(v_gid, v));

        auto parent_gid = app_->DeltaParentGid(v);

        if (parent_gid == u_gid) {
          curr_modified.Insert(v);
        }
      }
    }

    auto& channels = messages_.Channels();

    if (comm_spec_.worker_id() == grape::kCoordinatorRank) {
      LOG(INFO) << "Resetting";
    }

    do {
      messages_.StartARound();
      messages_.ParallelProcess<fragment_t, grape::EmptyType>(
          thread_num(), *fragment_,
          [&curr_modified](int tid, vertex_t v, const grape::EmptyType& msg) {
            curr_modified.Insert(v);
          });

      ForEachSimple(curr_modified, inner_vertices,
                    [this, &next_modified](int tid, vertex_t u) {
                      auto u_gid = fragment_->Vertex2Gid(u);
                      auto oes = fragment_->GetOutgoingAdjList(u);

                      for (auto e : oes) {
                        auto v = e.neighbor;

                        if (app_->DeltaParentGid(v) == u_gid) {
                          next_modified.Insert(v);
                        }
                      }
                    });

      ForEachSimple(curr_modified, inner_vertices,
                    [this, &reset_vertices](int tid, vertex_t u) {
                      app_->values_[u] = app_->GetIdentityElement();
                      app_->deltas_[u].Reset(app_->GetIdentityElement());
                      reset_vertices.Insert(u);
                    });

      ForEach(next_modified, outer_vertices,
              [&channels, this](int tid, vertex_t v) {
                grape::EmptyType dummy;
                channels[tid].SyncStateOnOuterVertex(*fragment_, v, dummy);
                app_->deltas_[v].Reset(app_->GetIdentityElement());
              });
      messages_.FinishARound();

      if (next_modified.Count() > 0) {
        messages_.ForceContinue();
      }

      curr_modified.Clear();
      curr_modified.Swap(next_modified);
    } while (!messages_.ToTerminate());

    size_t n_reset = 0, local_n_reset = reset_vertices.Count();

    Communicator communicator;

    communicator.InitCommunicator(comm_spec_.comm());
    communicator.template Sum(local_n_reset, n_reset);

    if (comm_spec_.worker_id() == grape::kCoordinatorRank) {
      LOG(INFO) << "# of reset vertices: " << n_reset << " reset percent: "
                << (float) n_reset / fragment_->GetTotalVerticesNum();
      LOG(INFO) << "Start a round from all vertices";
    }

    // We have to use hashmap to keep delta because the outer vertices may
    // change
    VertexArray<value_t, vid_t> values;
    VertexArray<delta_t, vid_t> deltas;

    values.Init(inner_vertices);
    deltas.Init(inner_vertices);

    for (auto v : inner_vertices) {
      values[v] = app_->values_[v];
      deltas[v] = app_->deltas_[v];
    }

    fragment_ = inc_fragment_builder.Build();
    // Important!!! outer vertices may change, we should acquire it after new
    // graph is loaded
    outer_vertices = fragment_->OuterVertices();
    // Reset all states, active vertices will be marked in curr_modified_
    app_->Init(comm_spec_, fragment_);

    // copy to new graph
    for (auto v : inner_vertices) {
      app_->values_[v] = values[v];
      app_->deltas_[v] = deltas[v];
    }

    // Start a round without any condition
    messages_.StartARound();
    for (auto u : inner_vertices) {
      auto& value = app_->values_[u];
      auto& delta = app_->deltas_[u];

      if (delta.value != app_->GetIdentityElement()) {
        app_->Compute(u, value, delta, next_modified);
      }
    }

    ForEach(
        next_modified, outer_vertices, [&channels, this](int tid, vertex_t v) {
          auto& delta_to_send = app_->deltas_[v];
          //减少无用消息的发送
          if (delta_to_send.value != app_->GetIdentityElement()) {
            channels[tid].SyncStateOnOuterVertex(*fragment_, v, delta_to_send);
          }
        });
    messages_.FinishARound();
    app_->next_modified_.Swap(next_modified);
  }

  void Query() {
    MPI_Barrier(comm_spec_.comm());

    // allocate dependency arrays
    app_->Init(comm_spec_, fragment_);
    int step = 1;
    bool batch_stage = true;

    double exec_time = 0;

    messages_.Start();

    // Run an empty round, otherwise ParallelProcess will stuck
    messages_.StartARound();
    messages_.InitChannels(thread_num());
    messages_.FinishARound();

    while (true) {
      exec_time -= GetCurrentTime();
      auto inner_vertices = fragment_->InnerVertices();
      auto outer_vertices = fragment_->OuterVertices();

      messages_.StartARound();
      app_->next_modified_.ParallelClear(thread_num());

      {
        messages_.ParallelProcess<fragment_t, DependencyData<vid_t, value_t>>(
            thread_num(), *fragment_,
            [this](int tid, vertex_t v,
                   const DependencyData<vid_t, value_t>& msg) {
              if (app_->AccumulateTo(v, msg)) {
                app_->curr_modified_.Insert(v);
              }
            });
      }

      // Traverse outgoing neighbors
      if (FLAGS_cilk) {
        ForEachCilk(
            app_->curr_modified_, inner_vertices, [this](int tid, vertex_t u) {
              auto& value = app_->values_[u];
              auto last_value = value;
              auto& delta = app_->deltas_[u];

              if (app_->CombineValueDelta(value, delta)) {
                app_->Compute(u, last_value, delta, app_->next_modified_);
              }
            });
      } else {
        ForEachSimple(
            app_->curr_modified_, inner_vertices, [this](int tid, vertex_t u) {
              auto& value = app_->values_[u];
              auto last_value = value;
              // We don't cleanup delta with identity element, since we expect
              // the algorithm is monotonic
              auto& delta = app_->deltas_[u];

              if (app_->CombineValueDelta(value, delta)) {
                app_->Compute(u, last_value, delta, app_->next_modified_);
              }
            });
      }

      auto& channels = messages_.Channels();

      // send local delta to remote
      ForEach(app_->next_modified_, outer_vertices,
              [&channels, this](int tid, vertex_t v) {
                auto& delta_to_send = app_->deltas_[v];

                if (delta_to_send.value != app_->GetIdentityElement()) {
                  channels[tid].SyncStateOnOuterVertex(*fragment_, v,
                                                       delta_to_send);
                }
              });

      if (app_->next_modified_.Count() > 0) {
        messages_.ForceContinue();
      }

      VLOG(1) << "[Worker " << comm_spec_.worker_id()
              << "]: Finished IterateKernel - " << step;
      messages_.FinishARound();

      exec_time += GetCurrentTime();

      bool terminate = messages_.ToTerminate();

      if (terminate) {
        if (batch_stage) {
          batch_stage = false;

          if (comm_spec_.worker_id() == grape::kCoordinatorRank) {
            LOG(INFO) << "Batch time: " << exec_time << " sec";
          }
          exec_time = 0;
          step = 0;

          if (!FLAGS_efile_update.empty()) {
            deltaCompute();  // 重新加载图
          } else {
            LOG(ERROR) << "Missing efile_update or efile_updated";
            break;
          }
        } else {
          if (comm_spec_.worker_id() == grape::kCoordinatorRank) {
            LOG(INFO) << "Inc time: " << exec_time << " sec";
          }
          break;
        }
      }

      ++step;
      app_->next_modified_.Swap(app_->curr_modified_);
    }
    MPI_Barrier(comm_spec_.comm());
  }

  void Output(std::ostream& os) {
    auto inner_vertices = fragment_->InnerVertices();
    auto& values = app_->values_;

    for (auto v : inner_vertices) {
      os << fragment_->GetId(v) << " " << values[v] << std::endl;
    }
  }

  void Finalize() { messages_.Finalize(); }

 private:
  std::shared_ptr<APP_T> app_;
  std::shared_ptr<fragment_t> fragment_;
  message_manager_t messages_;
  Communicator communicator_;
  CommSpec comm_spec_;
};

}  // namespace grape

#endif
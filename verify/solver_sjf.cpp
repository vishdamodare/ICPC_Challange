// ICPC 2026 Huawei Challenge - Submission v4.0 (Equal-Queue Round-Robin & Load Balance)
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// 1. PROTOCOL TYPES & STRUCTURES
// ============================================================================

enum class TaskType { P_PRE, P_PROC, P_POST, D_PRE, D_PROC, D_POST };

enum class EventType { ARR, TDN, XDN, FIN };

enum class RequestStage {
  UNINITIALIZED,
  ARRIVED,
  P_PRE_IN_FLIGHT,
  P_WAIT_UP,
  P_PROC_READY,
  P_PROC_IN_FLIGHT,
  P_WAIT_DOWN,
  P_POST_READY,
  P_POST_IN_FLIGHT,
  D_PRE_READY,
  D_PRE_IN_FLIGHT,
  D_WAIT_UP,
  D_PROC_READY,
  D_PROC_IN_FLIGHT,
  D_WAIT_DOWN,
  D_POST_READY,
  D_POST_IN_FLIGHT,
  FINISHED
};

struct SystemConfig {
  int K;
  double S;
  double latency_in_ms;
  double bandwidth_gbps;
  long long bytes_per_token;
  int num_layers;
};

struct ScoringConfig {
  double SLO1;
  double SLO2;
  double tp_UB;
  double tp_base;
  double dist_base;
  double w_tp;
  double w_c;
};

struct Event {
  EventType type;
  int rid;
  int Lin;
  char server[32];
  std::string task_spec;
  double dur;
  char direction[8];
  int remote;
  long long size;
  char stage_tag[8];
  int m;
  std::vector<int> rids;
};

struct FrameContext {
  double timestamp;
  int eventCount;
  std::vector<Event> events;
};

struct Task {
  TaskType type;
  int server; // -1 for Edge (E), 0..K-1 for Cloud (Ck)
  int remote; // Assigned cloud index
  int ls;     // For P_PROC: layer start
  int le;     // For P_PROC: layer end
  int m;      // Batch size
  std::vector<int> requests;
};

struct RequestState {
  int rid = -1;
  int Lin = 0;
  int Lout = 0;
  int assignedRemote = -1;
  int nextLayerStart = 0;
  int tokensProduced = 0;
  int decodeIteration = 0;
  bool finished = false;
  bool decodeUpReady = false;
  bool decodeDownReady = false;
  RequestStage stage = RequestStage::UNINITIALIZED;
};

struct ServerState {
  bool busy = false;
  Task currentTask;
  double availableTime = 0.0;
};

struct FrameDelta {
  std::vector<Event> arrivals;
  std::vector<Event> taskCompletions;
  std::vector<Event> transferCompletions;
  std::vector<Event> finishes;
};

// ============================================================================
// 2. TASK TIME TABLE
// ============================================================================

struct RawTaskTimeRow {
  int batch_size;
  double prefill_pre;
  double prefill_proc;
  double prefill_post;
  double decode_pre;
  double decode_proc;
  double decode_post;
};

class TaskTable {
public:
  int N;
  std::vector<RawTaskTimeRow> raw_rows;

  void parse(FILE *inStream) {
    if (fscanf(inStream, "%d", &N) != 1)
      return;
    raw_rows.resize(N);
    for (int i = 0; i < N; ++i) {
      if (fscanf(inStream, "%d %lf %lf %lf %lf %lf %lf",
                 &raw_rows[i].batch_size, &raw_rows[i].prefill_pre,
                 &raw_rows[i].prefill_proc, &raw_rows[i].prefill_post,
                 &raw_rows[i].decode_pre, &raw_rows[i].decode_proc,
                 &raw_rows[i].decode_post) != 7) {
        break;
      }
    }
  }

  double interpolate(TaskType type, int batch_size) const {
    if (raw_rows.empty())
      return 1.0;

    std::vector<std::pair<int, double>> points;
    for (const auto &r : raw_rows) {
      double val = -1.0;
      switch (type) {
      case TaskType::P_PRE:
        val = r.prefill_pre;
        break;
      case TaskType::P_PROC:
        val = r.prefill_proc;
        break;
      case TaskType::P_POST:
        val = r.prefill_post;
        break;
      case TaskType::D_PRE:
        val = r.decode_pre;
        break;
      case TaskType::D_PROC:
        val = r.decode_proc;
        break;
      case TaskType::D_POST:
        val = r.decode_post;
        break;
      }
      if (val >= 0.0) {
        points.push_back({r.batch_size, val});
      }
    }

    if (points.empty())
      return 1.0;

    std::sort(points.begin(), points.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    if (batch_size <= points.front().first)
      return points.front().second;
    if (batch_size >= points.back().first)
      return points.back().second;

    for (size_t i = 0; i < points.size() - 1; ++i) {
      if (batch_size >= points[i].first && batch_size <= points[i + 1].first) {
        double x0 = points[i].first;
        double y0 = points[i].second;
        double x1 = points[i + 1].first;
        double y1 = points[i + 1].second;
        if (std::abs(x1 - x0) < 1e-9)
          return y0;
        return y0 + (batch_size - x0) * (y1 - y0) / (x1 - x0);
      }
    }

    return points.back().second;
  }
};

// ============================================================================
// 3. STATE TRACKER
// ============================================================================

static inline int fastParseInt(const char *&p) {
  while (*p && *p <= ' ')
    p++;
  bool neg = false;
  if (*p == '-') {
    neg = true;
    p++;
  }
  int val = 0;
  while (*p >= '0' && *p <= '9') {
    val = val * 10 + (*p - '0');
    p++;
  }
  return neg ? -val : val;
}

static inline void removeFromVec(std::vector<int> &vec, int val) {
  for (size_t i = 0; i < vec.size(); ++i) {
    if (vec[i] == val) {
      vec[i] = vec.back();
      vec.pop_back();
      return;
    }
  }
}

class StateTracker {
public:
  SystemConfig sysConfig;
  ServerState edgeServer;
  std::vector<ServerState> cloudServers;
  std::vector<RequestState> requests;

  std::vector<int> pPreReadyList;
  std::vector<int> pPostReadyList;
  std::vector<int> dPreReadyList;
  std::vector<int> dPostReadyList;
  std::vector<std::vector<int>> pProcReadyList;
  std::vector<std::vector<int>> dProcReadyList;

  void init(const SystemConfig &sys) {
    sysConfig = sys;
    edgeServer.busy = false;
    cloudServers.resize(sys.K);
    for (int k = 0; k < sys.K; ++k) {
      cloudServers[k].busy = false;
    }
    requests.clear();
    pPreReadyList.clear();
    pPostReadyList.clear();
    dPreReadyList.clear();
    dPostReadyList.clear();
    pProcReadyList.assign(sys.K, {});
    dProcReadyList.assign(sys.K, {});
  }

  void rebuildReadinessLists() {}

  void processFrame(const FrameContext &frame) {
    FrameDelta delta;
    for (const auto &ev : frame.events) {
      switch (ev.type) {
      case EventType::ARR:
        delta.arrivals.push_back(ev);
        break;
      case EventType::TDN:
        delta.taskCompletions.push_back(ev);
        break;
      case EventType::XDN:
        delta.transferCompletions.push_back(ev);
        break;
      case EventType::FIN:
        delta.finishes.push_back(ev);
        break;
      }
    }
    applyArrivals(delta);
    applyTaskCompletions(delta);
    applyTransferCompletions(delta);
    applyFinishes(delta);
  }

  void applyArrivals(const FrameDelta &delta) {
    for (const auto &ev : delta.arrivals) {
      if (ev.rid >= static_cast<int>(requests.size())) {
        requests.resize(ev.rid + 1);
      }
      RequestState req;
      req.rid = ev.rid;
      req.Lin = ev.Lin;
      req.stage = RequestStage::ARRIVED;
      requests[ev.rid] = req;
      pPreReadyList.push_back(ev.rid);
    }
  }

  void applyTaskCompletions(const FrameDelta &delta) {
    for (const auto &ev : delta.taskCompletions) {
      int cloudIdx = -1;
      if (ev.server[0] == 'E' && ev.server[1] == '\0') {
        edgeServer.busy = false;
      } else if (ev.server[0] == 'C') {
        int k = 0;
        const char *sp = ev.server + 1;
        while (*sp >= '0' && *sp <= '9') {
          k = k * 10 + (*sp - '0');
          sp++;
        }
        if (k >= 0 && k < sysConfig.K) {
          cloudServers[k].busy = false;
          cloudIdx = k;
        }
      }

      const char *ptr = ev.task_spec.c_str();
      while (*ptr && *ptr <= ' ')
        ptr++;

      if (ptr[0] == 'P') {
        ptr++;
        while (*ptr && *ptr <= ' ')
          ptr++;
        if (ptr[0] == 'P' && ptr[1] == 'R' && ptr[2] == 'E') {
          ptr += 3;
          int remote = fastParseInt(ptr);
          int rid = fastParseInt(ptr);
          if (rid >= 0 && rid < static_cast<int>(requests.size())) {
            requests[rid].assignedRemote = remote;
            requests[rid].stage = RequestStage::P_WAIT_UP;
          }
        } else if (ptr[0] == 'P' && ptr[1] == 'R' && ptr[2] == 'O' &&
                   ptr[3] == 'C') {
          ptr += 4;
          int ls = fastParseInt(ptr);
          int le = fastParseInt(ptr);
          int remote = fastParseInt(ptr);
          int rid = fastParseInt(ptr);
          if (rid >= 0 && rid < static_cast<int>(requests.size())) {
            if (le < sysConfig.num_layers) {
              requests[rid].nextLayerStart = le;
              requests[rid].stage = RequestStage::P_PROC_READY;
              if (cloudIdx >= 0 && cloudIdx < sysConfig.K) {
                pProcReadyList[cloudIdx].push_back(rid);
              }
            } else {
              requests[rid].nextLayerStart = sysConfig.num_layers;
              requests[rid].stage = RequestStage::P_WAIT_DOWN;
            }
          }
        } else if (ptr[0] == 'P' && ptr[1] == 'O' && ptr[2] == 'S' &&
                   ptr[3] == 'T') {
          ptr += 4;
          int remote = fastParseInt(ptr);
          int rid = fastParseInt(ptr);
          if (rid >= 0 && rid < static_cast<int>(requests.size())) {
            requests[rid].stage = RequestStage::D_PRE_READY;
            dPreReadyList.push_back(rid);
          }
        }
      } else if (ptr[0] == 'D') {
        ptr++;
        while (*ptr && *ptr <= ' ')
          ptr++;
        if (ptr[0] == 'P' && ptr[1] == 'R' && ptr[2] == 'E') {
          ptr += 3;
          int dummy_remote = fastParseInt(ptr);
          int m = fastParseInt(ptr);
          for (int i = 0; i < m; ++i) {
            int rid = fastParseInt(ptr);
            if (rid >= 0 && rid < static_cast<int>(requests.size())) {
              requests[rid].stage = RequestStage::D_WAIT_UP;
              requests[rid].decodeUpReady = false;
            }
          }
        } else if (ptr[0] == 'P' && ptr[1] == 'R' && ptr[2] == 'O' &&
                   ptr[3] == 'C') {
          ptr += 4;
          int remote = fastParseInt(ptr);
          int m = fastParseInt(ptr);
          for (int i = 0; i < m; ++i) {
            int rid = fastParseInt(ptr);
            if (rid >= 0 && rid < static_cast<int>(requests.size())) {
              requests[rid].stage = RequestStage::D_WAIT_DOWN;
              requests[rid].decodeDownReady = false;
            }
          }
        } else if (ptr[0] == 'P' && ptr[1] == 'O' && ptr[2] == 'S' &&
                   ptr[3] == 'T') {
          ptr += 4;
          int dummy_remote = fastParseInt(ptr);
          int m = fastParseInt(ptr);
          for (int i = 0; i < m; ++i) {
            int rid = fastParseInt(ptr);
            if (rid >= 0 && rid < static_cast<int>(requests.size())) {
              requests[rid].tokensProduced++;
              requests[rid].decodeIteration++;
              if (!requests[rid].finished) {
                requests[rid].stage = RequestStage::D_PRE_READY;
                dPreReadyList.push_back(rid);
              }
            }
          }
        }
      }
    }
  }

  void applyTransferCompletions(const FrameDelta &delta) {
    for (const auto &ev : delta.transferCompletions) {
      if (strcmp(ev.stage_tag, "PRE") == 0) {
        if (strcmp(ev.direction, "UP") == 0) {
          if (ev.rid >= 0 && ev.rid < static_cast<int>(requests.size())) {
            requests[ev.rid].stage = RequestStage::P_PROC_READY;
            int k = requests[ev.rid].assignedRemote;
            if (k >= 0 && k < sysConfig.K) {
              pProcReadyList[k].push_back(ev.rid);
            }
          }
        } else if (strcmp(ev.direction, "DOWN") == 0) {
          if (ev.rid >= 0 && ev.rid < static_cast<int>(requests.size())) {
            requests[ev.rid].stage = RequestStage::P_POST_READY;
            pPostReadyList.push_back(ev.rid);
          }
        }
      } else if (strcmp(ev.stage_tag, "DEC") == 0) {
        if (strcmp(ev.direction, "UP") == 0) {
          for (int i = 0; i < static_cast<int>(ev.rids.size()); ++i) {
            int rid = ev.rids[i];
            if (rid >= 0 && rid < static_cast<int>(requests.size())) {
              requests[rid].decodeUpReady = true;
              requests[rid].stage = RequestStage::D_PROC_READY;
              int k = requests[rid].assignedRemote;
              if (k >= 0 && k < static_cast<int>(sysConfig.K)) {
                dProcReadyList[k].push_back(rid);
              }
            }
          }
        } else if (strcmp(ev.direction, "DOWN") == 0) {
          for (int i = 0; i < static_cast<int>(ev.rids.size()); ++i) {
            int rid = ev.rids[i];
            if (rid >= 0 && rid < static_cast<int>(requests.size())) {
              requests[rid].decodeDownReady = true;
              requests[rid].stage = RequestStage::D_POST_READY;
              dPostReadyList.push_back(rid);
            }
          }
        }
      }
    }
  }

  void applyFinishes(const FrameDelta &delta) {
    for (const auto &ev : delta.finishes) {
      if (ev.rid >= 0 && ev.rid < static_cast<int>(requests.size())) {
        requests[ev.rid].finished = true;
        requests[ev.rid].stage = RequestStage::FINISHED;
        removeFromVec(dPreReadyList, ev.rid);
        removeFromVec(dPostReadyList, ev.rid);
      }
    }
  }

  void markTaskAssigned(const Task &task) {
    if (task.server == -1) {
      edgeServer.busy = true;
    } else if (task.server >= 0 && task.server < sysConfig.K) {
      cloudServers[task.server].busy = true;
    }

    for (int rid : task.requests) {
      if (rid >= 0 && rid < static_cast<int>(requests.size())) {
        switch (task.type) {
        case TaskType::P_PRE:
          requests[rid].stage = RequestStage::P_PRE_IN_FLIGHT;
          removeFromVec(pPreReadyList, rid);
          break;
        case TaskType::P_PROC:
          requests[rid].stage = RequestStage::P_PROC_IN_FLIGHT;
          if (task.server >= 0 && task.server < sysConfig.K) {
            removeFromVec(pProcReadyList[task.server], rid);
          }
          break;
        case TaskType::P_POST:
          removeFromVec(pPostReadyList, rid);
          break;
        case TaskType::D_PRE:
          requests[rid].stage = RequestStage::D_PRE_IN_FLIGHT;
          removeFromVec(dPreReadyList, rid);
          break;
        case TaskType::D_PROC:
          requests[rid].stage = RequestStage::D_PROC_IN_FLIGHT;
          if (task.server >= 0 && task.server < sysConfig.K) {
            removeFromVec(dProcReadyList[task.server], rid);
          }
          break;
        case TaskType::D_POST:
          requests[rid].stage = RequestStage::D_POST_IN_FLIGHT;
          removeFromVec(dPostReadyList, rid);
          break;
        }
      }
    }
  }
};

// ============================================================================
// 4. LEGAL TASK GENERATOR
// ============================================================================

class LegalTaskGenerator {
public:
  static std::vector<Task> generateCandidates(const StateTracker &state) {
    std::vector<Task> candidates;
    candidates.reserve(16);

    if (!state.edgeServer.busy) {
      if (!state.pPreReadyList.empty()) {
        std::vector<int> sortedPPre = state.pPreReadyList;
        std::sort(sortedPPre.begin(), sortedPPre.end(), [&](int a, int b) {
          return state.requests[a].Lin < state.requests[b].Lin;
        });
        int rid = sortedPPre[0];
        for (int k = 0; k < state.sysConfig.K; ++k) {
          Task t;
          t.type = TaskType::P_PRE;
          t.server = -1;
          t.remote = k;
          t.m = 1;
          t.requests = {rid};
          candidates.push_back(t);
        }
      }
      if (!state.pPostReadyList.empty()) {
        int rid = state.pPostReadyList[0];
        Task t;
        t.type = TaskType::P_POST;
        t.server = -1;
        t.remote = state.requests[rid].assignedRemote;
        t.m = 1;
        t.requests = {rid};
        candidates.push_back(t);
      }
      if (!state.dPreReadyList.empty()) {
        Task t;
        t.type = TaskType::D_PRE;
        t.server = -1;
        t.remote = -1;
        t.m = static_cast<int>(state.dPreReadyList.size());
        t.requests = state.dPreReadyList;
        candidates.push_back(t);
      }
      if (!state.dPostReadyList.empty()) {
        Task t;
        t.type = TaskType::D_POST;
        t.server = -1;
        t.remote = -1;
        t.m = static_cast<int>(state.dPostReadyList.size());
        t.requests = state.dPostReadyList;
        candidates.push_back(t);
      }
    }

    for (int k = 0; k < state.sysConfig.K; ++k) {
      if (state.cloudServers[k].busy)
        continue;

      if (!state.pProcReadyList[k].empty()) {
        int rid = state.pProcReadyList[k][0];
        Task t;
        t.type = TaskType::P_PROC;
        t.server = k;
        t.remote = k;
        t.ls = state.requests[rid].nextLayerStart;
        t.le = state.sysConfig.num_layers;
        t.m = 1;
        t.requests = {rid};
        candidates.push_back(t);
      }
      if (!state.dProcReadyList[k].empty()) {
        Task t;
        t.type = TaskType::D_PROC;
        t.server = k;
        t.remote = k;
        t.m = static_cast<int>(state.dProcReadyList[k].size());
        t.requests = state.dProcReadyList[k];
        candidates.push_back(t);
      }
    }

    return candidates;
  }
};

// ============================================================================
// 5. SCHEDULING STRATEGY (EQUAL-QUEUE ROUND-ROBIN & LOAD BALANCE v4.0)
// ============================================================================

class SchedulingStrategy {
public:
  virtual ~SchedulingStrategy() = default;
  virtual std::vector<Task>
  selectTasks(const StateTracker &state,
              const std::vector<Task> &candidates) = 0;
};

class GreedyBatchStrategy : public SchedulingStrategy {
private:
  const TaskTable &taskTable;

public:
  explicit GreedyBatchStrategy(const TaskTable &table) : taskTable(table) {}

  std::vector<Task> selectTasks(const StateTracker &state,
                                const std::vector<Task> &candidates) override {
    std::vector<Task> selected;

    if (!state.edgeServer.busy) {
      if (!state.pPostReadyList.empty()) {
        int rid = state.pPostReadyList[0];
        Task t;
        t.type = TaskType::P_POST;
        t.server = -1;
        t.remote = state.requests[rid].assignedRemote;
        t.m = 1;
        t.requests = {rid};
        selected.push_back(t);
      } else if (!state.pPreReadyList.empty()) {
        std::vector<int> sortedPPre = state.pPreReadyList;
        std::sort(sortedPPre.begin(), sortedPPre.end(), [&](int a, int b) {
          return state.requests[a].Lin < state.requests[b].Lin;
        });
        int rid = sortedPPre[0];
        // Equal-Queue Round-Robin & Load Balancing:
        // Use Round-Robin (rid % K) if all cloud prefill queues are equal,
        // otherwise assign to the cloud server with the shortest prefill queue.
        int targetRemote = rid % state.sysConfig.K;
        int minCount = 1e9;
        int maxCount = -1;

        for (int k = 0; k < state.sysConfig.K; ++k) {
          int count = static_cast<int>(state.pProcReadyList[k].size());
          if (count < minCount) {
            minCount = count;
            targetRemote = k;
          }
          if (count > maxCount) {
            maxCount = count;
          }
        }

        if (minCount == maxCount) {
          targetRemote = rid % state.sysConfig.K;
        }

        Task t;
        t.type = TaskType::P_PRE;
        t.server = -1;
        t.remote = targetRemote;
        t.m = 1;
        t.requests = {rid};
        selected.push_back(t);
      } else if (!state.dPostReadyList.empty()) {
        Task t;
        t.type = TaskType::D_POST;
        t.server = -1;
        t.remote = -1;
        t.m = static_cast<int>(state.dPostReadyList.size());
        t.requests = state.dPostReadyList;
        selected.push_back(t);
      } else if (!state.dPreReadyList.empty()) {
        Task t;
        t.type = TaskType::D_PRE;
        t.server = -1;
        t.remote = -1;
        t.m = static_cast<int>(state.dPreReadyList.size());
        t.requests = state.dPreReadyList;
        selected.push_back(t);
      }
    }

    for (int k = 0; k < state.sysConfig.K; ++k) {
      if (state.cloudServers[k].busy)
        continue;

      if (!state.pProcReadyList[k].empty()) {
        int rid = state.pProcReadyList[k][0];
        Task t;
        t.type = TaskType::P_PROC;
        t.server = k;
        t.remote = k;
        t.ls = state.requests[rid].nextLayerStart;
        t.le = state.sysConfig.num_layers;
        t.m = 1;
        t.requests = {rid};
        selected.push_back(t);
      } else if (!state.dProcReadyList[k].empty()) {
        Task t;
        t.type = TaskType::D_PROC;
        t.server = k;
        t.remote = k;
        t.m = static_cast<int>(state.dProcReadyList[k].size());
        t.requests = state.dProcReadyList[k];
        selected.push_back(t);
      }
    }

    return selected;
  }
};

// ============================================================================
// 6. CONFLICT RESOLVER
// ============================================================================

class ConflictResolver {
public:
  static std::vector<Task>
  resolveConflicts(const StateTracker &state,
                   const std::vector<Task> &selectedTasks) {
    std::vector<Task> validTasks;
    bool edgeUsed = state.edgeServer.busy;
    std::vector<bool> cloudUsed(state.sysConfig.K, false);
    for (int k = 0; k < state.sysConfig.K; ++k) {
      cloudUsed[k] = state.cloudServers[k].busy;
    }

    for (const auto &task : selectedTasks) {
      if (task.server == -1) {
        if (edgeUsed)
          continue;
        edgeUsed = true;
      } else if (task.server >= 0 && task.server < state.sysConfig.K) {
        if (cloudUsed[task.server])
          continue;
        cloudUsed[task.server] = true;
      } else {
        continue;
      }
      validTasks.push_back(task);
    }

    return validTasks;
  }
};

// ============================================================================
// 7. OUTPUT FORMATTER & FAST I/O
// ============================================================================

static char g_outStaticBuf[1048576]; // 1MB output static buffer for large
                                     // decode batches

class OutputFormatter {
public:
  static void sendResponse(const std::vector<Task> &tasks) {
    char *p = g_outStaticBuf;
    int n = static_cast<int>(tasks.size());

    if (n >= 100) {
      p += sprintf(p, "%d\n", n);
    } else if (n >= 10) {
      *p++ = '0' + (n / 10);
      *p++ = '0' + (n % 10);
      *p++ = '\n';
    } else {
      *p++ = '0' + n;
      *p++ = '\n';
    }

    for (const auto &t : tasks) {
      if (t.server == -1) {
        *p++ = 'E';
      } else {
        *p++ = 'C';
        if (t.server >= 10) {
          p += sprintf(p, "%d", t.server);
        } else {
          *p++ = '0' + t.server;
        }
      }
      *p++ = ' ';

      switch (t.type) {
      case TaskType::P_PRE:
        memcpy(p, "P PRE ", 6);
        p += 6;
        p += sprintf(p, "%d %d\n", t.remote, t.requests[0]);
        break;

      case TaskType::P_PROC:
        memcpy(p, "P PROC ", 7);
        p += 7;
        p += sprintf(p, "%d %d %d %d\n", t.ls, t.le, t.remote, t.requests[0]);
        break;

      case TaskType::P_POST:
        memcpy(p, "P POST ", 7);
        p += 7;
        p += sprintf(p, "%d %d\n", t.remote, t.requests[0]);
        break;

      case TaskType::D_PRE:
        memcpy(p, "D PRE -1 ", 9);
        p += 9;
        p += sprintf(p, "%d", t.m);
        for (int r : t.requests) {
          p += sprintf(p, " %d", r);
        }
        *p++ = '\n';
        break;

      case TaskType::D_PROC:
        memcpy(p, "D PROC ", 7);
        p += 7;
        p += sprintf(p, "%d %d", t.remote, t.m);
        for (int r : t.requests) {
          p += sprintf(p, " %d", r);
        }
        *p++ = '\n';
        break;

      case TaskType::D_POST:
        memcpy(p, "D POST -1 ", 10);
        p += 10;
        p += sprintf(p, "%d", t.m);
        for (int r : t.requests) {
          p += sprintf(p, " %d", r);
        }
        *p++ = '\n';
        break;
      }
    }
    *p = '\0';
    fputs(g_outStaticBuf, stdout);
    fflush(stdout);
  }
};

// ============================================================================
// 8. FAST INPUT PARSER & MAIN INTERACTION LOOP
// ============================================================================

static char
    g_inLineBuf[1048576]; // 1MB line buffer for large input frame events

static inline double fastParseDouble(const char *&p) {
  while (*p && *p <= ' ')
    p++;
  bool neg = false;
  if (*p == '-') {
    neg = true;
    p++;
  }
  double val = 0.0;
  while (*p >= '0' && *p <= '9') {
    val = val * 10.0 + (*p - '0');
    p++;
  }
  if (*p == '.') {
    p++;
    double frac = 0.1;
    while (*p >= '0' && *p <= '9') {
      val += (*p - '0') * frac;
      frac *= 0.1;
      p++;
    }
  }
  return neg ? -val : val;
}

class ProtocolHandler {
public:
  static bool parseSystemConfig(SystemConfig &sys, FILE *inStream) {
    if (fscanf(inStream, "%d %lf %lf %lf %lld %d", &sys.K, &sys.S,
               &sys.latency_in_ms, &sys.bandwidth_gbps, &sys.bytes_per_token,
               &sys.num_layers) != 6) {
      return false;
    }
    return true;
  }

  static bool parseScoringConfig(ScoringConfig &sc, FILE *inStream) {
    if (fscanf(inStream, "%lf %lf %lf %lf %lf %lf %lf", &sc.SLO1, &sc.SLO2,
               &sc.tp_UB, &sc.tp_base, &sc.dist_base, &sc.w_tp, &sc.w_c) != 7) {
      return false;
    }
    return true;
  }

  static bool readFrame(FrameContext &frame, FILE *inStream) {
    frame.events.clear();

    const char *p = nullptr;
    do {
      if (fgets(g_inLineBuf, sizeof(g_inLineBuf), inStream) == nullptr) {
        return false;
      }
      p = g_inLineBuf;
      while (*p && *p <= ' ')
        p++;
      if (*p != '\0')
        break;
    } while (true);

    if (*p == 'E' && *(p + 1) == 'N' && *(p + 2) == 'D') {
      return false;
    }

    frame.timestamp = fastParseDouble(p);

    if (fgets(g_inLineBuf, sizeof(g_inLineBuf), inStream) == nullptr) {
      return false;
    }
    p = g_inLineBuf;
    frame.eventCount = fastParseInt(p);

    for (int i = 0; i < frame.eventCount; ++i) {
      if (fgets(g_inLineBuf, sizeof(g_inLineBuf), inStream) == nullptr) {
        return false;
      }
      p = g_inLineBuf;
      while (*p && *p <= ' ')
        p++;

      Event ev;

      if (p[0] == 'A' && p[1] == 'R' && p[2] == 'R') {
        ev.type = EventType::ARR;
        p += 3;
        ev.rid = fastParseInt(p);
        ev.Lin = fastParseInt(p);
      } else if (p[0] == 'T' && p[1] == 'D' && p[2] == 'N') {
        p += 3;
        ev.type = EventType::TDN;
        while (*p && *p <= ' ')
          p++;

        const char *startServer = p;
        while (*p && *p > ' ')
          p++;
        size_t sLen = std::min<size_t>(p - startServer, sizeof(ev.server) - 1);
        memcpy(ev.server, startServer, sLen);
        ev.server[sLen] = '\0';

        const char *rest = p;
        while (*rest && *rest <= ' ')
          rest++;
        const char *lastSpace = strrchr(rest, ' ');
        if (lastSpace) {
          ev.task_spec.assign(rest, lastSpace - rest);
          const char *durPtr = lastSpace + 1;
          ev.dur = fastParseDouble(durPtr);
        } else {
          ev.task_spec = rest;
          ev.dur = 0.0;
        }
      } else if (p[0] == 'X' && p[1] == 'D' && p[2] == 'N') {
        p += 3;
        ev.type = EventType::XDN;
        while (*p && *p <= ' ')
          p++;

        if (p[0] == 'U' && p[1] == 'P') {
          ev.direction[0] = 'U';
          ev.direction[1] = 'P';
          ev.direction[2] = '\0';
          p += 2;
        } else if (p[0] == 'D' && p[1] == 'O' && p[2] == 'W' && p[3] == 'N') {
          ev.direction[0] = 'D';
          ev.direction[1] = 'O';
          ev.direction[2] = 'W';
          ev.direction[3] = 'N';
          ev.direction[4] = '\0';
          p += 4;
        }

        ev.remote = fastParseInt(p);
        while (*p && *p <= ' ')
          p++;
        ev.size = 0;
        while (*p >= '0' && *p <= '9') {
          ev.size = ev.size * 10 + (*p - '0');
          p++;
        }

        while (*p && *p <= ' ')
          p++;
        const char *startTag = p;
        while (*p && *p > ' ')
          p++;
        size_t tagLen =
            std::min<size_t>(p - startTag, sizeof(ev.stage_tag) - 1);
        memcpy(ev.stage_tag, startTag, tagLen);
        ev.stage_tag[tagLen] = '\0';

        ev.m = fastParseInt(p);
        ev.rids.resize(ev.m);
        for (int r = 0; r < ev.m; ++r) {
          ev.rids[r] = fastParseInt(p);
        }
        if (ev.m > 0)
          ev.rid = ev.rids[0];
      } else if (p[0] == 'F' && p[1] == 'I' && p[2] == 'N') {
        ev.type = EventType::FIN;
        p += 3;
        ev.rid = fastParseInt(p);
      }
      frame.events.push_back(ev);
    }
    return true;
  }
};

int main() {
  std::ios_base::sync_with_stdio(false);
  std::cin.tie(nullptr);

  SystemConfig sys;
  if (!ProtocolHandler::parseSystemConfig(sys, stdin))
    return 0;

  ScoringConfig sc;
  if (!ProtocolHandler::parseScoringConfig(sc, stdin))
    return 0;

  TaskTable table;
  table.parse(stdin);

  StateTracker state;
  state.init(sys);

  GreedyBatchStrategy strat(table);

  FrameContext frame;
  while (ProtocolHandler::readFrame(frame, stdin)) {
    state.processFrame(frame);
    // GreedyBatchStrategy::selectTasks never reads its `candidates` arg.
    static const std::vector<Task> kUnusedCandidates;
    auto selected = strat.selectTasks(state, kUnusedCandidates);
    auto validTasks = ConflictResolver::resolveConflicts(state, selected);

    for (const auto &task : validTasks) {
      state.markTaskAssigned(task);
    }

    OutputFormatter::sendResponse(validTasks);
  }

  return 0;
}

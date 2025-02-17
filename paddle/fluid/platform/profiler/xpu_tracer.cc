// Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.
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
//

#include "paddle/fluid/platform/profiler/xpu_tracer.h"

#include <mutex>
#include <unordered_map>

#include "paddle/fluid/platform/enforce.h"
#include "paddle/phi/core/os_info.h"
#ifdef PADDLE_WITH_XPU
#include "paddle/phi/backends/device_manager.h"
#endif

#ifdef PADDLE_WITH_XPTI
#define XPTI_CALL(call)                                                       \
  do {                                                                        \
    XPTIResult _status = call;                                                \
    if (_status != XPTI_SUCCESS) {                                            \
      LOG(ERROR) << "Function " << #call << " failed with error " << _status; \
      exit(-1);                                                               \
    }                                                                         \
  } while (0)
#endif  // PADDLE_WITH_XPTI

namespace paddle::platform {

#ifdef PADDLE_WITH_XPTI
using XPTIEvent = baidu::xpu::xpti::XPTIEvent;
using XPTIEventAPI = baidu::xpu::xpti::XPTIEventAPI;
using XPTIEventKernel = baidu::xpu::xpti::XPTIEventKernel;
using XPTIEventMem = baidu::xpu::xpti::XPTIEventMem;
using XPTIEventWait = baidu::xpu::xpti::XPTIEventWait;
#endif

void XPUTracer::PrepareTracing() {
  PADDLE_ENFORCE_EQ(
      state_ == TracerState::UNINITED || state_ == TracerState::STOPPED,
      true,
      common::errors::PreconditionNotMet("XPUTracer must be UNINITED"));
#ifdef PADDLE_WITH_XPTI
  XPTI_CALL(phi::dynload::xptiActivityEnable());
  VLOG(3) << "enable xpti activity";
#endif
  state_ = TracerState::READY;
}

void XPUTracer::StartTracing() {
  PADDLE_ENFORCE_EQ(
      state_ == TracerState::READY,
      true,
      common::errors::PreconditionNotMet("Tracer must be READY or STOPPED"));
#ifdef PADDLE_WITH_XPTI
  XPTI_CALL(phi::dynload::xptiStartTracing());
#endif
  tracing_start_ns_ = phi::PosixInNsec();
  state_ = TracerState::STARTED;
}

void XPUTracer::StopTracing() {
  PADDLE_ENFORCE_EQ(
      state_,
      TracerState::STARTED,
      common::errors::PreconditionNotMet("Tracer must be STARTED"));
#ifdef PADDLE_WITH_XPTI
  XPTI_CALL(phi::dynload::xptiStopTracing());
  XPTI_CALL(phi::dynload::xptiActivityDisable());
  VLOG(3) << "disable xpti activity";
#endif
  state_ = TracerState::STOPPED;
}

#ifdef PADDLE_WITH_XPTI
static void AddApiRecord(const XPTIEvent* xpti_event,
                         uint64_t start_ns,
                         TraceEventCollector* collector) {
  const auto* api = dynamic_cast<const XPTIEventAPI*>(xpti_event);
  if (api == nullptr) {
    VLOG(4) << "xpu event " << xpti_event->name << " is not a API event";
    return;
  }
  if (api->start < start_ns) {
    VLOG(4) << "xpu event " << api->name << " start " << api->start
            << " is before profiler start " << start_ns << ", drop event";
    return;
  }
  RuntimeTraceEvent event;
  event.name = api->name;
  event.start_ns = api->start;
  event.end_ns = api->end;
  event.process_id = api->pid;
  event.thread_id = api->tid;
  event.correlation_id = api->token;

  collector->AddRuntimeEvent(std::move(event));
  VLOG(4) << "Add api event " << event.name;
}

static void AddKernelRecord(const XPTIEvent* xpti_event,
                            uint64_t start_ns,
                            TraceEventCollector* collector) {
  const auto* kernel = dynamic_cast<const XPTIEventKernel*>(xpti_event);
  if (kernel == nullptr) {
    VLOG(4) << "xpu event " << xpti_event->name << " is not a kernel event";
    return;
  }
  if (kernel->start < start_ns) {
    VLOG(4) << "xpu event " << kernel->name << "start " << kernel->start
            << "is before profiler start " << start_ns << ", drop event";
    return;
  }
  DeviceTraceEvent event;
  event.name = kernel->name;
  event.type = TracerEventType::Kernel;
  event.start_ns = kernel->start;
  event.end_ns = kernel->end;
  event.device_id = kernel->device_id;
  event.stream_id = kernel->stream_id;
  event.correlation_id = kernel->token;

  collector->AddDeviceEvent(std::move(event));
  VLOG(4) << "Add kernel event " << event.name;
}

static void AddWaitRecord(const XPTIEvent* xpti_event,
                          uint64_t start_ns,
                          TraceEventCollector* collector) {
  const auto* wait = dynamic_cast<const XPTIEventWait*>(xpti_event);
  if (wait == nullptr) {
    VLOG(4) << "xpu event" << xpti_event->name << "is not a wait event";
    return;
  }
  if (wait->start < start_ns) {
    VLOG(4) << "xpu event " << wait->name << "start " << wait->start
            << "is before profiler start " << start_ns << ", drop event";
    return;
  }
  RuntimeTraceEvent event;
  event.name = wait->name;
  event.start_ns = wait->start;
  event.end_ns = wait->end;
  event.process_id = wait->pid;
  event.thread_id = wait->tid;

  collector->AddRuntimeEvent(std::move(event));
  VLOG(4) << "Add wait event " << event.name;
}

static void AddMemcpyRecord(const XPTIEvent* xpti_event,
                            uint64_t start_ns,
                            TraceEventCollector* collector) {
  const auto* memcpy = dynamic_cast<const XPTIEventMem*>(xpti_event);
  if (memcpy == nullptr) {
    VLOG(4) << "xpu event" << xpti_event->name << "is not a memcpy event";
    return;
  }
  if (memcpy->start < start_ns) {
    VLOG(4) << "xpu event " << memcpy->name << "start " << memcpy->start
            << "is before profiler start " << start_ns << ", drop event";
    return;
  }
  RuntimeTraceEvent event;
  event.name = memcpy->name;
  event.start_ns = memcpy->start;
  event.end_ns = memcpy->end;
  event.process_id = memcpy->pid;
  event.thread_id = memcpy->tid;

  collector->AddRuntimeEvent(std::move(event));
  VLOG(4) << "Add memcpy event " << event.name;
}
#endif

void XPUTracer::CollectTraceData(TraceEventCollector* collector) {
  PADDLE_ENFORCE_EQ(
      state_,
      TracerState::STOPPED,
      common::errors::PreconditionNotMet("Tracer must be STOPPED"));
#ifdef PADDLE_WITH_XPTI
  XPTI_CALL(phi::dynload::xptiActivityFlushAll());
  baidu::xpu::xpti::XPTIEvent* record = nullptr;
  while (true) {
    XPTIResult status = phi::dynload::xptiActivityGetNextRecord(&record);
    if (status == XPTI_SUCCESS) {
      switch (record->type) {
        case XPTI_EVENT_TYPE_API:
          AddApiRecord(record, tracing_start_ns_, collector);
          break;
        case XPTI_EVENT_TYPE_KERNEL:
          AddKernelRecord(record, tracing_start_ns_, collector);
          break;
        case XPTI_EVENT_TYPE_MEMCPY:
          AddMemcpyRecord(record, tracing_start_ns_, collector);
          break;
        case XPTI_EVENT_TYPE_WAIT:
          AddWaitRecord(record, tracing_start_ns_, collector);
          break;
        default:
          break;
      }
      phi::dynload::xptiActivityPopRecord();
    } else if (status == XPTI_INVALID_DATA) {
      // data queue already empty
      VLOG(4) << "xpti data queue is empty now, collect trace data done";
      break;
    } else {
      XPTI_CALL(status);
    }
    // free XPTIEvent
  }
#endif
}

}  // namespace paddle::platform

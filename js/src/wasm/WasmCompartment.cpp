/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2016 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmCompartment.h"

#include "vm/JSCompartment.h"
#include "wasm/WasmInstance.h"

#include "vm/Debugger-inl.h"

using namespace js;
using namespace wasm;

Compartment::Compartment(JSRuntime* rt)
  : runtime_(rt)
{}

Compartment::~Compartment()
{
    MOZ_ASSERT(instances_.empty());
}

struct InstanceComparator
{
    const Instance& target;
    explicit InstanceComparator(const Instance& target) : target(target) {}

    int operator()(const Instance* instance) const {
        if (instance == &target)
            return 0;

        // Instances can share code, so the segments can be equal (though they
        // can't partially overlap).  If the codeBases are equal, we sort by
        // Instance address.  Thus a Code may map to many instances.

        // Compare by the first tier, always.

        Tier instanceTier = instance->code().stableTier();
        Tier targetTier = target.code().stableTier();

        if (instance->codeBase(instanceTier) == target.codeBase(targetTier))
            return instance < &target ? -1 : 1;

        return target.codeBase(targetTier) < instance->codeBase(instanceTier) ? -1 : 1;
    }
};

bool
Compartment::registerInstance(JSContext* cx, HandleWasmInstanceObject instanceObj)
{
    MOZ_ASSERT(runtime_ == cx->runtime());

    Instance& instance = instanceObj->instance();
    MOZ_ASSERT(this == &instance.compartment()->wasm);

    instance.ensureProfilingLabels(cx->runtime()->geckoProfiler().enabled());

    if (instance.debugEnabled() && instance.compartment()->debuggerObservesAllExecution())
        instance.ensureEnterFrameTrapsState(cx, true);

    {
        if (!instances_.reserve(instances_.length() + 1))
            return false;

        auto runtimeInstances = cx->runtime()->wasmInstances.lock();
        if (!runtimeInstances->reserve(runtimeInstances->length() + 1))
            return false;

        // To avoid implementing rollback, do not fail after mutations start.

        InstanceComparator cmp(instance);
        size_t index;

        MOZ_ALWAYS_FALSE(BinarySearchIf(instances_, 0, instances_.length(), cmp, &index));
        MOZ_ALWAYS_TRUE(instances_.insert(instances_.begin() + index, &instance));

        MOZ_ALWAYS_FALSE(BinarySearchIf(runtimeInstances.get(), 0, runtimeInstances->length(), cmp, &index));
        MOZ_ALWAYS_TRUE(runtimeInstances->insert(runtimeInstances->begin() + index, &instance));
    }

    // Notify the debugger after wasmInstances is unlocked.
    Debugger::onNewWasmInstance(cx, instanceObj);
    return true;
}

void
Compartment::unregisterInstance(Instance& instance)
{
    InstanceComparator cmp(instance);
    size_t index;

    if (BinarySearchIf(instances_, 0, instances_.length(), cmp, &index))
        instances_.erase(instances_.begin() + index);

    auto runtimeInstances = runtime_->wasmInstances.lock();
    if (BinarySearchIf(runtimeInstances.get(), 0, runtimeInstances->length(), cmp, &index))
        runtimeInstances->erase(runtimeInstances->begin() + index);
}

void
Compartment::ensureProfilingLabels(bool profilingEnabled)
{
    for (Instance* instance : instances_)
        instance->ensureProfilingLabels(profilingEnabled);
}

void
Compartment::addSizeOfExcludingThis(MallocSizeOf mallocSizeOf, size_t* compartmentTables)
{
    *compartmentTables += instances_.sizeOfExcludingThis(mallocSizeOf);
}

void
wasm::InterruptRunningCode(JSContext* cx)
{
    auto runtimeInstances = cx->runtime()->wasmInstances.lock();
    for (Instance* instance : runtimeInstances.get())
        instance->tlsData()->setInterrupt();
}

void
wasm::ResetInterruptState(JSContext* cx)
{
    auto runtimeInstances = cx->runtime()->wasmInstances.lock();
    for (Instance* instance : runtimeInstances.get())
        instance->tlsData()->resetInterrupt(cx);
}

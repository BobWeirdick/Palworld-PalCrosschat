#pragma once

#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

// Shared SEH (structured exception handling) wrappers for Unreal calls that have been
// observed to AV on this game build (see README "After a Palworld update" / commit
// history). C++ try/catch does NOT catch these — they are hardware exceptions, not C++
// exceptions — so every call site that touches ProcessEvent or FWeakObjectPtr::Get()
// must go through here instead of calling them directly.
namespace PalCrosschat
{
    // obj->ProcessEvent(fn, params) inside SEH. Returns false (never crashes the
    // process) if the call faults or any argument is null.
    bool SafeProcessEvent(RC::Unreal::UObject* obj, RC::Unreal::UFunction* fn, void* params);

    // weak.Get() inside SEH. Resets weak and returns nullptr if the call faults.
    RC::Unreal::UObject* SafeWeakGet(RC::Unreal::FWeakObjectPtr& weak);
}

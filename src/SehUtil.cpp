#include "SehUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <DynamicOutput/DynamicOutput.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace PalCrosschat
{
    namespace
    {
        struct PeCtx
        {
            UObject* obj = nullptr;
            UFunction* fn = nullptr;
            void* params = nullptr;
        };

        int ProcessEventSeh(PeCtx* ctx)
        {
            __try
            {
                ctx->obj->ProcessEvent(ctx->fn, ctx->params);
                return 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 1;
            }
        }

        struct WeakGetCtx
        {
            FWeakObjectPtr* weak = nullptr;
            UObject* out = nullptr;
        };

        int WeakGetSeh(WeakGetCtx* ctx)
        {
            __try
            {
                ctx->out = ctx->weak->Get();
                return 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                ctx->out = nullptr;
                return 1;
            }
        }
    }

    bool SafeProcessEvent(UObject* obj, UFunction* fn, void* params)
    {
        if (!obj || !fn || !params)
        {
            return false;
        }
        PeCtx ctx{obj, fn, params};
        if (ProcessEventSeh(&ctx) != 0)
        {
            Output::send<LogLevel::Error>(
                STR("[PalCrosschat] ProcessEvent faulted; call skipped\n"));
            return false;
        }
        return true;
    }

    UObject* SafeWeakGet(FWeakObjectPtr& weak)
    {
        WeakGetCtx ctx{};
        ctx.weak = &weak;
        if (WeakGetSeh(&ctx) != 0)
        {
            weak.Reset();
            return nullptr;
        }
        return ctx.out;
    }
}

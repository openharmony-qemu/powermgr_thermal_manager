/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "thermal_manager_napi.h"

#include <memory>

#include "napi_utils.h"
#include "napi_errors.h"
#include "thermal_common.h"
#include "thermal_level_info.h"
#ifdef THERMAL_API_METRICS_ENABLE
#include "histogram_plugin_macros.h"
#endif

using namespace OHOS::PowerMgr;
using namespace OHOS;

namespace {
const uint8_t ARG_0 = 0;
const uint8_t ARG_1 = 1;
constexpr uint32_t MAX_ARGC = 1;
thread_local auto& g_thermalMgrClient = ThermalMgrClient::GetInstance();
thread_local sptr<ThermalLevelCallback> g_thermalLevelCallback = new (std::nothrow) ThermalLevelCallback();
#ifdef THERMAL_API_METRICS_ENABLE
constexpr int32_t HISTOGRAM_API_CALL_COUNT = 1;
#endif
} // namespace

namespace OHOS {
namespace PowerMgr {
struct ThermalCallbackCleanup {
    sptr<ThermalLevelCallback> callback;
};
} // namespace PowerMgr
} // namespace OHOS

ThermalLevelCallback::~ThermalLevelCallback()
{
    ReleaseCallback();
}

bool ThermalLevelCallback::UpdateCallback(napi_env env, napi_value jsCallback, bool legacy)
{
    napi_ref reference = nullptr;
    if (napi_ok != napi_create_reference(env, jsCallback, 1, &reference)) {
        THERMAL_HILOGW(COMP_FWK, "Failed to create a JS callback reference");
        return false;
    }
    auto cleanup = new (std::nothrow) ThermalCallbackCleanup {sptr<ThermalLevelCallback>(this)};
    if (cleanup == nullptr) {
        napi_delete_reference(env, reference);
        return false;
    }
    if (napi_add_env_cleanup_hook(env, CleanupCallbackEnv, cleanup) != napi_ok) {
        napi_delete_reference(env, reference);
        delete cleanup;
        return false;
    }
    ReleaseCallback();
    std::lock_guard lock(mutex_);
    callbackRef_ = reference;
    env_ = env;
    cleanup_ = cleanup;
    legacy_ = legacy;
    return true;
}

void ThermalLevelCallback::ReleaseCallback()
{
    napi_ref reference = nullptr;
    napi_env env = nullptr;
    ThermalCallbackCleanup* cleanup = nullptr;
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        reference = callbackRef_;
        env = env_;
        cleanup = cleanup_;
        callbackRef_ = nullptr;
        env_ = nullptr;
        cleanup_ = nullptr;
    }
    const bool cleanupRemoved = cleanup != nullptr &&
        napi_remove_env_cleanup_hook(env, CleanupCallbackEnv, cleanup) == napi_ok;
    if (reference != nullptr) {
        napi_delete_reference(env, reference);
    }
    // Drop the hook's strong owner only after releasing mutex_ so its final
    // destructor cannot re-enter ReleaseCallback while the mutex is held.
    if (cleanupRemoved) {
        delete cleanup;
    }
}

bool ThermalLevelCallback::HasCallback()
{
    std::lock_guard lock(mutex_);
    return callbackRef_ != nullptr && env_ != nullptr;
}

void ThermalLevelCallback::CleanupCallbackEnv(void* data)
{
    std::unique_ptr<ThermalCallbackCleanup> cleanup(static_cast<ThermalCallbackCleanup*>(data));
    auto callback = cleanup->callback;
    {
        std::lock_guard lock(callback->mutex_);
        if (callback->cleanup_ != cleanup.get()) {
            // A previous hook whose removal failed must only release its own
            // owner, not clear a later subscription or a different live env.
            return;
        }
        // This hook is already executing, so ReleaseCallback must not remove
        // it or delete its data a second time.
        callback->cleanup_ = nullptr;
    }
    callback->ReleaseCallback();
    ThermalMgrClient::GetInstance().UnSubscribeThermalLevelCallback(callback);
}

bool ThermalLevelCallback::OnAsyncThermalLevelChanged(ThermalLevel level)
{
    std::lock_guard lock(mutex_);
    THERMAL_RETURN_IF_WITH_RET(env_ == nullptr, false);
    const uint64_t generation = generation_;
    auto uvcallback = [callback = sptr<ThermalLevelCallback>(this), level, generation]() {
        callback->OnThermalLevel(level, generation);
    };
    if (napi_send_event(env_, uvcallback, napi_eprio_low, __func__) != napi_status::napi_ok) {
        THERMAL_HILOGW(COMP_FWK, "uv_queue_work is failed");
        return false;
    }
    return true;
}

void ThermalLevelCallback::OnThermalLevel(ThermalLevel level, uint64_t generation)
{
    THERMAL_HILOGD(COMP_FWK, "level is: %{public}d", static_cast<int32_t>(level));
    napi_env env = nullptr;
    napi_value callback = nullptr;
    bool legacy = false;
    napi_handle_scope scope = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (generation != generation_ || callbackRef_ == nullptr || env_ == nullptr) {
            return;
        }
        env = env_;
        legacy = legacy_;
        if (napi_open_handle_scope(env, &scope) != napi_ok || scope == nullptr) {
            return;
        }
        if (napi_get_reference_value(env, callbackRef_, &callback) != napi_ok || callback == nullptr) {
            napi_close_handle_scope(env, scope);
            return;
        }
    }

    napi_value levelValue = nullptr;
    if (napi_ok != napi_create_int32(env, static_cast<int32_t>(level), &levelValue)) {
        THERMAL_HILOGW(COMP_FWK, "napi_create_int32 callback failed");
        napi_close_handle_scope(env, scope);
        return;
    }
    napi_value arguments[2] = {levelValue, nullptr};
    size_t argc = 1;
    if (legacy) {
        if (napi_get_undefined(env, &arguments[0]) != napi_ok) {
            napi_close_handle_scope(env, scope);
            return;
        }
        arguments[1] = levelValue;
        argc = 2;
    }
    napi_value callResult = nullptr;
    // Never hold mutex_ while invoking JavaScript: callbacks may unregister
    // themselves or replace the subscription synchronously.
    napi_status status = napi_call_function(env, nullptr, callback, argc, arguments, &callResult);
    if (status != napi_ok) {
        THERMAL_HILOGE(COMP_FWK, "napi_call_function callback failed, status = %{public}d", status);
    }
    napi_close_handle_scope(env, scope);
}

napi_value ThermalManagerNapi::SubscribeThermalLevelLegacy(napi_env env, napi_callback_info info)
{
    return SubscribeThermalLevelImpl(env, info, true);
}

napi_value ThermalManagerNapi::UnSubscribeThermalLevelLegacy(napi_env env, napi_callback_info info)
{
    return UnSubscribeThermalLevelImpl(env, info, true);
}

napi_value ThermalManagerNapi::Init(napi_env env, napi_value exports)
{
    napi_property_descriptor desc[] = {
        // Old Interface
        DECLARE_NAPI_STATIC_FUNCTION("subscribeThermalLevel", SubscribeThermalLevelLegacy),
        DECLARE_NAPI_STATIC_FUNCTION("unsubscribeThermalLevel", UnSubscribeThermalLevelLegacy),
        DECLARE_NAPI_STATIC_FUNCTION("getThermalLevel", GetThermalLevel),
        // New Interface
        DECLARE_NAPI_STATIC_FUNCTION("registerThermalLevelCallback", SubscribeThermalLevel),
        DECLARE_NAPI_STATIC_FUNCTION("unregisterThermalLevelCallback", UnSubscribeThermalLevel),
        DECLARE_NAPI_STATIC_FUNCTION("getLevel", GetThermalLevel),
    };
    NAPI_CALL(env, napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc));
    InitThermalLevel(env, exports);

    return exports;
}

napi_value ThermalManagerNapi::InitThermalLevel(napi_env env, napi_value exports)
{
    napi_value cool;
    napi_value normal;
    napi_value warm;
    napi_value hot;
    napi_value overheated;
    napi_value warning;
    napi_value emergency;
    napi_value escape;

    napi_create_uint32(env, static_cast<uint32_t>(ThermalLevel::COOL), &cool);
    napi_create_uint32(env, static_cast<uint32_t>(ThermalLevel::NORMAL), &normal);
    napi_create_uint32(env, static_cast<uint32_t>(ThermalLevel::WARM), &warm);
    napi_create_uint32(env, static_cast<uint32_t>(ThermalLevel::HOT), &hot);
    napi_create_uint32(env, static_cast<uint32_t>(ThermalLevel::OVERHEATED), &overheated);
    napi_create_uint32(env, static_cast<uint32_t>(ThermalLevel::WARNING), &warning);
    napi_create_uint32(env, static_cast<uint32_t>(ThermalLevel::EMERGENCY), &emergency);
    napi_create_uint32(env, static_cast<uint32_t>(ThermalLevel::ESCAPE), &escape);

    napi_property_descriptor desc[] = {
        DECLARE_NAPI_STATIC_PROPERTY("COOL", cool),
        DECLARE_NAPI_STATIC_PROPERTY("NORMAL", normal),
        DECLARE_NAPI_STATIC_PROPERTY("WARM", warm),
        DECLARE_NAPI_STATIC_PROPERTY("HOT", hot),
        DECLARE_NAPI_STATIC_PROPERTY("OVERHEATED", overheated),
        DECLARE_NAPI_STATIC_PROPERTY("WARNING", warning),
        DECLARE_NAPI_STATIC_PROPERTY("EMERGENCY", emergency),
        DECLARE_NAPI_STATIC_PROPERTY("ESCAPE", escape),
    };

    napi_value result = nullptr;
    napi_define_class(env, "ThermalLevel", NAPI_AUTO_LENGTH, EnumThermalLevelConstructor, nullptr,
        sizeof(desc) / sizeof(*desc), desc, &result);
    napi_set_named_property(env, exports, "ThermalLevel", result);
    return exports;
}

napi_value ThermalManagerNapi::EnumThermalLevelConstructor(napi_env env, napi_callback_info info)
{
    size_t argc = 0;
    napi_value argv[ARG_1] = {0};
    napi_value jsthis = nullptr;
    void* data = nullptr;

    napi_status status = napi_get_cb_info(env, info, &argc, argv, &jsthis, &data);

    THERMAL_HILOGI(COMP_FWK, "EnumThermalLevelConstructor %{public}d", status);
    if (status != napi_ok) {
        return nullptr;
    }
    return jsthis;
}

napi_value ThermalManagerNapi::GetThermalLevel(napi_env env, napi_callback_info info)
{
#ifdef THERMAL_API_METRICS_ENABLE
    HISTOGRAM_BOOLEAN("BasicServicesKit.ThermalManager.getThermalLevel.Boolean", HISTOGRAM_API_CALL_COUNT);
#endif
    ThermalLevel level = g_thermalMgrClient.GetThermalLevel();
    int32_t levelValue = static_cast<int32_t>(level);
    napi_value napiValue;
    NAPI_CALL(env, napi_create_int32(env, levelValue, &napiValue));

    THERMAL_HILOGI(COMP_FWK, "level is %{public}d", levelValue);
    return napiValue;
}

napi_value ThermalManagerNapi::SubscribeThermalLevel(napi_env env, napi_callback_info info)
{
    return SubscribeThermalLevelImpl(env, info, false);
}

napi_value ThermalManagerNapi::SubscribeThermalLevelImpl(napi_env env, napi_callback_info info, bool legacy)
{
#ifdef THERMAL_API_METRICS_ENABLE
    HISTOGRAM_BOOLEAN("BasicServicesKit.ThermalManager.subscribeThermalLevel.Boolean", HISTOGRAM_API_CALL_COUNT);
#endif
    size_t argc = MAX_ARGC;
    napi_value argv[argc];
    NapiUtils::GetCallbackInfo(env, info, argc, argv);

    NapiErrors error;
    if (argc != MAX_ARGC || !NapiUtils::CheckValueType(env, argv[ARG_0], napi_function)) {
        return error.ThrowError(env, ThermalErrors::ERR_PARAM_INVALID);
    }

    napi_value result;
    napi_get_undefined(env, &result);

    if (g_thermalLevelCallback == nullptr || !g_thermalLevelCallback->UpdateCallback(env, argv[ARG_0], legacy)) {
        return error.ThrowError(env, ThermalErrors::ERR_CONNECTION_FAIL);
    }
    if (!g_thermalMgrClient.SubscribeThermalLevelCallback(g_thermalLevelCallback, false)) {
        g_thermalLevelCallback->ReleaseCallback();
        return error.ThrowError(env, ThermalErrors::ERR_CONNECTION_FAIL);
    }

    return result;
}

napi_value ThermalManagerNapi::UnSubscribeThermalLevel(napi_env env, napi_callback_info info)
{
    return UnSubscribeThermalLevelImpl(env, info, false);
}

napi_value ThermalManagerNapi::UnSubscribeThermalLevelImpl(napi_env env, napi_callback_info info, bool legacy)
{
#ifdef THERMAL_API_METRICS_ENABLE
    HISTOGRAM_BOOLEAN("BasicServicesKit.ThermalManager.unsubscribeThermalLevel.Boolean", HISTOGRAM_API_CALL_COUNT);
#endif
    size_t argc = MAX_ARGC;
    napi_value argv[argc];
    NapiUtils::GetCallbackInfo(env, info, argc, argv);

    NapiErrors error;
    const bool omitted = argc == ARG_0 ||
        (argc == MAX_ARGC && NapiUtils::CheckValueType(env, argv[ARG_0], napi_undefined));
    if (argc > MAX_ARGC || (!omitted && !NapiUtils::CheckValueType(env, argv[ARG_0], napi_function))) {
        return error.ThrowError(env, ThermalErrors::ERR_PARAM_INVALID);
    }
    napi_value result = nullptr;
    napi_get_undefined(env, &result);
    if (g_thermalLevelCallback != nullptr && g_thermalLevelCallback->HasCallback()) {
        if (!g_thermalMgrClient.UnSubscribeThermalLevelCallback(g_thermalLevelCallback)) {
            return error.ThrowError(env, ThermalErrors::ERR_CONNECTION_FAIL);
        }
        g_thermalLevelCallback->ReleaseCallback();
    }
    THERMAL_RETURN_IF_WITH_RET(omitted, result);
    napi_value argument = nullptr;
    if (legacy && napi_get_undefined(env, &argument) != napi_ok) {
        return result;
    }
    napi_status status = napi_call_function(env, nullptr, argv[ARG_0], legacy ? 1 : 0,
        legacy ? &argument : nullptr, &result);
    if (status != napi_ok) {
        THERMAL_HILOGW(COMP_FWK, "status=%{public}d", status);
        return result;
    }
    return result;
}

EXTERN_C_START
/*
 * function for module exports
 */
static napi_value ThermalInit(napi_env env, napi_value exports)
{
    THERMAL_HILOGD(COMP_FWK, "Enter");

    napi_value ret = ThermalManagerNapi::Init(env, exports);

    THERMAL_HILOGD(COMP_FWK, "Exit");

    return ret;
}
EXTERN_C_END

/*
 * Module definition
 */
static napi_module g_module = {.nm_version = 1,
    .nm_flags = 0,
    .nm_filename = "thermal",
    .nm_register_func = ThermalInit,
    .nm_modname = "thermal",
    .nm_priv = ((void*)0),
    .reserved = {0}};

/*
 * Module registration
 */
extern "C" __attribute__((constructor)) void RegisterModule()
{
    napi_module_register(&g_module);
}

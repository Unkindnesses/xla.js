#include <node_api.h>

#include <dlfcn.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "xla/pjrt/c/pjrt_c_api.h"

namespace {

struct LibraryRef {
  void* handle = nullptr;
  const PJRT_Api* api = nullptr;
};

struct ClientRef {
  LibraryRef* library = nullptr;
  PJRT_Client* client = nullptr;
};

struct ExecutableRef {
  ClientRef* client = nullptr;
  PJRT_LoadedExecutable* executable = nullptr;
  napi_env env = nullptr;
  napi_ref client_ref = nullptr;
};

enum class AsyncExecuteStep {
  kStart,
  kAfterInput,
  kAfterExecute,
  kAfterHostCopy,
};

struct AsyncExecuteRef {
  napi_env env = nullptr;
  napi_deferred deferred = nullptr;
  napi_threadsafe_function ready = nullptr;
  napi_ref executable_ref = nullptr;
  ExecutableRef* executable = nullptr;
  PJRT_Buffer* input = nullptr;
  PJRT_Buffer* output = nullptr;
  PJRT_Event* waiting_event = nullptr;
  float input_value = NAN;
  float result = NAN;
  AsyncExecuteStep step = AsyncExecuteStep::kStart;
  std::string error;
};

template <class T>
T* External(napi_env env, napi_value value) {
  void* data = nullptr;
  napi_get_value_external(env, value, &data);
  return static_cast<T*>(data);
}

void Throw(napi_env env, const std::string& message) {
  napi_throw_error(env, nullptr, message.c_str());
}

bool HasPendingException(napi_env env) {
  bool pending = false;
  napi_is_exception_pending(env, &pending);
  return pending;
}

std::string StringArg(napi_env env, napi_callback_info info, size_t index,
                      napi_value* args, size_t argc) {
  if (index >= argc) return "";
  size_t size = 0;
  napi_get_value_string_utf8(env, args[index], nullptr, 0, &size);
  std::string result(size, '\0');
  napi_get_value_string_utf8(env, args[index], result.data(), size + 1, &size);
  return result;
}

int32_t IntArg(napi_env env, napi_value value, int32_t fallback) {
  napi_valuetype type = napi_undefined;
  napi_typeof(env, value, &type);
  if (type == napi_undefined || type == napi_null) return fallback;
  int32_t result = fallback;
  napi_get_value_int32(env, value, &result);
  return result;
}

double NumberArg(napi_env env, napi_value value) {
  double result = NAN;
  napi_get_value_double(env, value, &result);
  return result;
}

std::string SingleDeviceCompileOptions() {
  const unsigned char bytes[] = {
      0x1a, 0x0f, 0x20, 0x01, 0x28, 0x01, 0x4a, 0x09, 0x08,
      0x01, 0x10, 0x01, 0x1a, 0x03, 0x0a, 0x01, 0x00,
  };
  return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

std::string PjrtErrorMessage(const PJRT_Api* api, PJRT_Error* error) {
  PJRT_Error_Message_Args message_args{
      .struct_size = PJRT_Error_Message_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .error = error,
  };
  api->PJRT_Error_Message(&message_args);
  std::string message(message_args.message, message_args.message_size);

  PJRT_Error_Destroy_Args destroy_args{
      .struct_size = PJRT_Error_Destroy_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .error = error,
  };
  api->PJRT_Error_Destroy(&destroy_args);
  return message;
}

void CheckPjrt(napi_env env, const PJRT_Api* api, PJRT_Error* error) {
  if (!error) return;

  Throw(env, PjrtErrorMessage(api, error));
}

void DestroyEvent(const PJRT_Api* api, PJRT_Event* event) {
  if (!event) return;
  PJRT_Event_Destroy_Args args{
      .struct_size = PJRT_Event_Destroy_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .event = event,
  };
  api->PJRT_Event_Destroy(&args);
}

void DestroyBuffer(const PJRT_Api* api, PJRT_Buffer* buffer) {
  if (!buffer) return;
  PJRT_Buffer_Destroy_Args args{
      .struct_size = PJRT_Buffer_Destroy_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .buffer = buffer,
  };
  api->PJRT_Buffer_Destroy(&args);
}

napi_value ExternalValue(napi_env env, void* data) {
  napi_value result;
  napi_create_external(env, data, nullptr, nullptr, &result);
  return result;
}

void FreeClientRef(ClientRef* ref) {
  if (!ref) return;
  if (ref->client) {
    PJRT_Client_Destroy_Args args{
        .struct_size = PJRT_Client_Destroy_Args_STRUCT_SIZE,
        .extension_start = nullptr,
        .client = ref->client,
    };
    ref->library->api->PJRT_Client_Destroy(&args);
  }
  if (ref->library->handle) dlclose(ref->library->handle);
  ref->client = nullptr;
  ref->library->handle = nullptr;
}

void FreeExecutableRef(ExecutableRef* ref) {
  if (!ref) return;
  if (ref->executable) {
    PJRT_LoadedExecutable_Destroy_Args args{
        .struct_size = PJRT_LoadedExecutable_Destroy_Args_STRUCT_SIZE,
        .extension_start = nullptr,
        .executable = ref->executable,
    };
    ref->client->library->api->PJRT_LoadedExecutable_Destroy(&args);
  }
  if (ref->client_ref) napi_delete_reference(ref->env, ref->client_ref);
  ref->executable = nullptr;
  ref->client_ref = nullptr;
}

void DeleteClientRef(napi_env, void* data, void*) {
  auto* ref = static_cast<ClientRef*>(data);
  FreeClientRef(ref);
  delete ref->library;
  delete ref;
}

void DeleteExecutableRef(napi_env, void* data, void*) {
  auto* ref = static_cast<ExecutableRef*>(data);
  FreeExecutableRef(ref);
  delete ref;
}

napi_value CreateClient(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

  auto path = StringArg(env, info, 0, args, argc);
  int32_t device_count = argc > 1 ? IntArg(env, args[1], 1) : 1;

  auto* library = new LibraryRef;
  library->handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library->handle) {
    std::string message = "Failed to load PJRT plugin: ";
    message += dlerror();
    delete library;
    Throw(env, message);
    return nullptr;
  }

  auto get_api = reinterpret_cast<const PJRT_Api* (*)()>(dlsym(library->handle, "GetPjrtApi"));
  if (!get_api) {
    dlclose(library->handle);
    delete library;
    Throw(env, "PJRT plugin does not export GetPjrtApi");
    return nullptr;
  }

  library->api = get_api();
  auto* ref = new ClientRef{.library = library};

  std::string option_name = "cpu_device_count";
  PJRT_NamedValue create_option{
      .struct_size = PJRT_NamedValue_STRUCT_SIZE,
      .extension_start = nullptr,
      .name = option_name.c_str(),
      .name_size = option_name.size(),
      .type = PJRT_NamedValue_kInt64,
      .int64_value = device_count,
      .value_size = 1,
  };
  PJRT_Client_Create_Args create_args{
      .struct_size = PJRT_Client_Create_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .create_options = &create_option,
      .num_options = 1,
      .client = nullptr,
  };
  CheckPjrt(env, library->api, library->api->PJRT_Client_Create(&create_args));
  if (HasPendingException(env) || !create_args.client) {
    delete ref;
    return nullptr;
  }

  ref->client = create_args.client;
  return ExternalValue(env, ref);
}

napi_value PlatformName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  auto* ref = External<ClientRef>(env, args[0]);
  PJRT_Client_PlatformName_Args name_args{
      .struct_size = PJRT_Client_PlatformName_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .client = ref->client,
  };
  CheckPjrt(env, ref->library->api, ref->library->api->PJRT_Client_PlatformName(&name_args));
  napi_value result;
  napi_create_string_utf8(env, name_args.platform_name, name_args.platform_name_size, &result);
  return result;
}

napi_value DeviceCount(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  auto* ref = External<ClientRef>(env, args[0]);
  PJRT_Client_Devices_Args device_args{
      .struct_size = PJRT_Client_Devices_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .client = ref->client,
  };
  CheckPjrt(env, ref->library->api, ref->library->api->PJRT_Client_Devices(&device_args));
  napi_value result;
  napi_create_uint32(env, static_cast<uint32_t>(device_args.num_devices), &result);
  return result;
}

napi_value Compile(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value args[3];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

  auto* client = External<ClientRef>(env, args[0]);
  auto code = StringArg(env, info, 1, args, argc);
  auto format = argc > 2 ? StringArg(env, info, 2, args, argc) : "mlir";
  std::string compile_options = SingleDeviceCompileOptions();

  PJRT_Program program{
      .struct_size = PJRT_Program_STRUCT_SIZE,
      .extension_start = nullptr,
      .code = code.data(),
      .code_size = code.size(),
      .format = format.c_str(),
      .format_size = format.size(),
  };
  PJRT_Client_Compile_Args compile_args{
      .struct_size = PJRT_Client_Compile_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .client = client->client,
      .program = &program,
      .compile_options = compile_options.data(),
      .compile_options_size = compile_options.size(),
  };
  CheckPjrt(env, client->library->api, client->library->api->PJRT_Client_Compile(&compile_args));
  if (HasPendingException(env) || !compile_args.executable) return nullptr;

  auto* executable = new ExecutableRef{
      .client = client,
      .executable = compile_args.executable,
      .env = env,
  };
  napi_create_reference(env, args[0], 1, &executable->client_ref);
  return ExternalValue(env, executable);
}

napi_value DisposeClient(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

  auto* ref = External<ClientRef>(env, args[0]);
  DeleteClientRef(env, ref, nullptr);
  return nullptr;
}

napi_value DisposeExecutable(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

  auto* ref = External<ExecutableRef>(env, args[0]);
  DeleteExecutableRef(env, ref, nullptr);
  return nullptr;
}

const PJRT_Api* AsyncApi(AsyncExecuteRef* ref) {
  return ref->executable->client->library->api;
}

void SetAsyncError(AsyncExecuteRef* ref, const std::string& message) {
  if (ref->error.empty()) ref->error = message;
}

void SetAsyncPjrtError(AsyncExecuteRef* ref, PJRT_Error* error) {
  if (!error) return;
  SetAsyncError(ref, PjrtErrorMessage(AsyncApi(ref), error));
}

PJRT_Device* AsyncFirstAddressableDevice(AsyncExecuteRef* ref) {
  auto* client = ref->executable->client;
  PJRT_Client_AddressableDevices_Args args{
      .struct_size = PJRT_Client_AddressableDevices_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .client = client->client,
  };
  SetAsyncPjrtError(ref, AsyncApi(ref)->PJRT_Client_AddressableDevices(&args));
  if (!ref->error.empty()) return nullptr;
  if (args.num_addressable_devices == 0) {
    SetAsyncError(ref, "PJRT client has no addressable devices");
    return nullptr;
  }
  return args.addressable_devices[0];
}

void CompleteAsyncExecute(AsyncExecuteRef* ref) {
  auto api = AsyncApi(ref);
  DestroyEvent(api, ref->waiting_event);
  DestroyBuffer(api, ref->input);
  DestroyBuffer(api, ref->output);
  if (ref->executable_ref) napi_delete_reference(ref->env, ref->executable_ref);
  if (ref->ready) napi_release_threadsafe_function(ref->ready, napi_tsfn_release);
  delete ref;
}

void RejectAsyncExecute(AsyncExecuteRef* ref) {
  napi_value message;
  napi_value error;
  napi_create_string_utf8(ref->env, ref->error.c_str(), ref->error.size(), &message);
  napi_create_error(ref->env, nullptr, message, &error);
  napi_reject_deferred(ref->env, ref->deferred, error);
  CompleteAsyncExecute(ref);
}

void ResolveAsyncExecute(AsyncExecuteRef* ref) {
  napi_value result;
  napi_create_double(ref->env, ref->result, &result);
  napi_resolve_deferred(ref->env, ref->deferred, result);
  CompleteAsyncExecute(ref);
}

void ContinueAsyncExecute(AsyncExecuteRef* ref);

void OnAsyncPjrtReady(PJRT_Error* error, void* user_arg) {
  auto* ref = static_cast<AsyncExecuteRef*>(user_arg);
  SetAsyncPjrtError(ref, error);
  DestroyEvent(AsyncApi(ref), ref->waiting_event);
  ref->waiting_event = nullptr;
  napi_call_threadsafe_function(ref->ready, ref, napi_tsfn_nonblocking);
}

void OnAsyncJsReady(napi_env env, napi_value, void*, void* data) {
  if (!env) return;
  ContinueAsyncExecute(static_cast<AsyncExecuteRef*>(data));
}

bool WaitForPjrtEvent(AsyncExecuteRef* ref, PJRT_Event* event) {
  if (!event) return false;

  ref->waiting_event = event;
  PJRT_Event_OnReady_Args args{
      .struct_size = PJRT_Event_OnReady_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .event = event,
      .callback = OnAsyncPjrtReady,
      .user_arg = ref,
  };
  SetAsyncPjrtError(ref, AsyncApi(ref)->PJRT_Event_OnReady(&args));
  if (!ref->error.empty()) {
    DestroyEvent(AsyncApi(ref), ref->waiting_event);
    ref->waiting_event = nullptr;
  }
  return ref->error.empty();
}

bool EnqueueAsyncInput(AsyncExecuteRef* ref) {
  auto* client = ref->executable->client;
  PJRT_Device* device = AsyncFirstAddressableDevice(ref);
  if (!device) return false;

  PJRT_Client_BufferFromHostBuffer_Args args{
      .struct_size = PJRT_Client_BufferFromHostBuffer_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .client = client->client,
      .data = &ref->input_value,
      .type = PJRT_Buffer_Type_F32,
      .dims = nullptr,
      .num_dims = 0,
      .byte_strides = nullptr,
      .num_byte_strides = 0,
      .host_buffer_semantics = PJRT_HostBufferSemantics_kImmutableOnlyDuringCall,
      .device = device,
      .memory = nullptr,
      .device_layout = nullptr,
  };
  SetAsyncPjrtError(ref, AsyncApi(ref)->PJRT_Client_BufferFromHostBuffer(&args));
  ref->input = args.buffer;
  ref->step = AsyncExecuteStep::kAfterInput;
  return ref->error.empty() && WaitForPjrtEvent(ref, args.done_with_host_buffer);
}

bool EnqueueAsyncExecution(AsyncExecuteRef* ref) {
  PJRT_Buffer* input_list[] = {ref->input};
  PJRT_Buffer** argument_lists[] = {input_list};
  PJRT_Buffer* output_list[] = {nullptr};
  PJRT_Buffer** output_lists[] = {output_list};
  PJRT_Event* complete_events[] = {nullptr};
  PJRT_ExecuteOptions options{
      .struct_size = PJRT_ExecuteOptions_STRUCT_SIZE,
      .extension_start = nullptr,
  };
  PJRT_LoadedExecutable_Execute_Args args{
      .struct_size = PJRT_LoadedExecutable_Execute_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .executable = ref->executable->executable,
      .options = &options,
      .argument_lists = argument_lists,
      .num_devices = 1,
      .num_args = 1,
      .output_lists = output_lists,
      .device_complete_events = complete_events,
  };
  SetAsyncPjrtError(ref, AsyncApi(ref)->PJRT_LoadedExecutable_Execute(&args));
  DestroyBuffer(AsyncApi(ref), ref->input);
  ref->input = nullptr;
  ref->output = output_list[0];
  ref->step = AsyncExecuteStep::kAfterExecute;
  return ref->error.empty() && WaitForPjrtEvent(ref, complete_events[0]);
}

bool EnqueueAsyncHostCopy(AsyncExecuteRef* ref) {
  if (!ref->output) {
    SetAsyncError(ref, "PJRT execution did not produce an output buffer");
    return false;
  }

  PJRT_Buffer_ToHostBuffer_Args args{
      .struct_size = PJRT_Buffer_ToHostBuffer_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .src = ref->output,
      .host_layout = nullptr,
      .dst = &ref->result,
      .dst_size = sizeof(ref->result),
  };
  SetAsyncPjrtError(ref, AsyncApi(ref)->PJRT_Buffer_ToHostBuffer(&args));
  ref->step = AsyncExecuteStep::kAfterHostCopy;
  return ref->error.empty() && WaitForPjrtEvent(ref, args.event);
}

void ContinueAsyncExecute(AsyncExecuteRef* ref) {
  if (!ref->error.empty()) {
    RejectAsyncExecute(ref);
    return;
  }

  bool waiting = false;
  if (ref->step == AsyncExecuteStep::kStart) waiting = EnqueueAsyncInput(ref);
  if (!waiting && ref->error.empty() && ref->step == AsyncExecuteStep::kAfterInput)
    waiting = EnqueueAsyncExecution(ref);
  if (!waiting && ref->error.empty() && ref->step == AsyncExecuteStep::kAfterExecute)
    waiting = EnqueueAsyncHostCopy(ref);
  if (waiting) return;
  if (!ref->error.empty()) {
    RejectAsyncExecute(ref);
    return;
  }
  ResolveAsyncExecute(ref);
}

napi_value ExecuteF32Scalar(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

  napi_value promise;
  napi_deferred deferred;
  napi_create_promise(env, &deferred, &promise);

  auto* executable = External<ExecutableRef>(env, args[0]);
  auto* ref = new AsyncExecuteRef{
      .env = env,
      .deferred = deferred,
      .executable = executable,
      .input_value = static_cast<float>(NumberArg(env, args[1])),
  };
  napi_create_reference(env, args[0], 1, &ref->executable_ref);

  napi_value name;
  napi_create_string_utf8(env, "PJRT executeF32Scalar", NAPI_AUTO_LENGTH, &name);
  napi_create_threadsafe_function(env, nullptr, nullptr, name, 0, 1, nullptr, nullptr, nullptr,
                                  OnAsyncJsReady, &ref->ready);
  ContinueAsyncExecute(ref);
  return promise;
}

napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor properties[] = {
      {"createClient", nullptr, CreateClient, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"platformName", nullptr, PlatformName, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"deviceCount", nullptr, DeviceCount, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"compile", nullptr, Compile, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"disposeClient", nullptr, DisposeClient, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"disposeExecutable", nullptr, DisposeExecutable, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"executeF32Scalar", nullptr, ExecuteF32Scalar, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties);
  return exports;
}

}  // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)

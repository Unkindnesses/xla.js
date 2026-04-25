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

void CheckPjrt(napi_env env, const PJRT_Api* api, PJRT_Error* error) {
  if (!error) return;

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
  Throw(env, message);
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

PJRT_Device* FirstAddressableDevice(napi_env env, ClientRef* ref) {
  PJRT_Client_AddressableDevices_Args args{
      .struct_size = PJRT_Client_AddressableDevices_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .client = ref->client,
  };
  CheckPjrt(env, ref->library->api, ref->library->api->PJRT_Client_AddressableDevices(&args));
  if (HasPendingException(env)) return nullptr;
  if (args.num_addressable_devices == 0) {
    Throw(env, "PJRT client has no addressable devices");
    return nullptr;
  }
  return args.addressable_devices[0];
}

napi_value ExternalValue(napi_env env, void* data, napi_finalize finalizer) {
  napi_value result;
  napi_create_external(env, data, finalizer, nullptr, &result);
  return result;
}

void FinalizeClient(napi_env, void* data, void*) {
  auto* ref = static_cast<ClientRef*>(data);
  if (ref->client) {
    PJRT_Client_Destroy_Args args{
        .struct_size = PJRT_Client_Destroy_Args_STRUCT_SIZE,
        .extension_start = nullptr,
        .client = ref->client,
    };
    ref->library->api->PJRT_Client_Destroy(&args);
  }
  if (ref->library->handle) dlclose(ref->library->handle);
  delete ref->library;
  delete ref;
}

void FinalizeExecutable(napi_env, void* data, void*) {
  auto* ref = static_cast<ExecutableRef*>(data);
  if (ref->executable) {
    PJRT_LoadedExecutable_Destroy_Args args{
        .struct_size = PJRT_LoadedExecutable_Destroy_Args_STRUCT_SIZE,
        .extension_start = nullptr,
        .executable = ref->executable,
    };
    ref->client->library->api->PJRT_LoadedExecutable_Destroy(&args);
  }
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
  return ExternalValue(env, ref, FinalizeClient);
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

  auto* executable = new ExecutableRef{.client = client, .executable = compile_args.executable};
  return ExternalValue(env, executable, FinalizeExecutable);
}

PJRT_Buffer* BufferFromF32Scalar(napi_env env, ClientRef* ref, float value) {
  PJRT_Device* device = FirstAddressableDevice(env, ref);
  if (!device) return nullptr;

  PJRT_Client_BufferFromHostBuffer_Args args{
      .struct_size = PJRT_Client_BufferFromHostBuffer_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .client = ref->client,
      .data = &value,
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
  CheckPjrt(env, ref->library->api, ref->library->api->PJRT_Client_BufferFromHostBuffer(&args));
  if (args.done_with_host_buffer) {
    PJRT_Event_Await_Args await_args{
        .struct_size = PJRT_Event_Await_Args_STRUCT_SIZE,
        .extension_start = nullptr,
        .event = args.done_with_host_buffer,
    };
    CheckPjrt(env, ref->library->api, ref->library->api->PJRT_Event_Await(&await_args));
    DestroyEvent(ref->library->api, args.done_with_host_buffer);
  }
  return args.buffer;
}

float F32ScalarFromBuffer(napi_env env, ClientRef* ref, PJRT_Buffer* buffer) {
  size_t size = sizeof(float);
  float result = NAN;
  PJRT_Buffer_ToHostBuffer_Args args{
      .struct_size = PJRT_Buffer_ToHostBuffer_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .src = buffer,
      .host_layout = nullptr,
      .dst = &result,
      .dst_size = size,
  };
  CheckPjrt(env, ref->library->api, ref->library->api->PJRT_Buffer_ToHostBuffer(&args));
  if (args.event) {
    PJRT_Event_Await_Args await_args{
        .struct_size = PJRT_Event_Await_Args_STRUCT_SIZE,
        .extension_start = nullptr,
        .event = args.event,
    };
    CheckPjrt(env, ref->library->api, ref->library->api->PJRT_Event_Await(&await_args));
    DestroyEvent(ref->library->api, args.event);
  }
  return result;
}

napi_value ExecuteF32Scalar(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value args[2];
  napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
  auto* executable = External<ExecutableRef>(env, args[0]);
  auto* client = executable->client;
  PJRT_Buffer* input = BufferFromF32Scalar(env, client, static_cast<float>(NumberArg(env, args[1])));
  if (!input) return nullptr;

  PJRT_Buffer* input_list[] = {input};
  PJRT_Buffer** argument_lists[] = {input_list};
  PJRT_Buffer* output_list[] = {nullptr};
  PJRT_Buffer** output_lists[] = {output_list};
  PJRT_Event* complete_events[] = {nullptr};
  PJRT_ExecuteOptions options{
      .struct_size = PJRT_ExecuteOptions_STRUCT_SIZE,
      .extension_start = nullptr,
  };
  PJRT_LoadedExecutable_Execute_Args execute_args{
      .struct_size = PJRT_LoadedExecutable_Execute_Args_STRUCT_SIZE,
      .extension_start = nullptr,
      .executable = executable->executable,
      .options = &options,
      .argument_lists = argument_lists,
      .num_devices = 1,
      .num_args = 1,
      .output_lists = output_lists,
      .device_complete_events = complete_events,
  };
  CheckPjrt(env, client->library->api, client->library->api->PJRT_LoadedExecutable_Execute(&execute_args));
  DestroyBuffer(client->library->api, input);

  if (complete_events[0]) {
    PJRT_Event_Await_Args await_args{
        .struct_size = PJRT_Event_Await_Args_STRUCT_SIZE,
        .extension_start = nullptr,
        .event = complete_events[0],
    };
    CheckPjrt(env, client->library->api, client->library->api->PJRT_Event_Await(&await_args));
    DestroyEvent(client->library->api, complete_events[0]);
  }

  if (!output_list[0]) {
    Throw(env, "PJRT execution did not produce an output buffer");
    return nullptr;
  }

  float value = F32ScalarFromBuffer(env, client, output_list[0]);
  DestroyBuffer(client->library->api, output_list[0]);
  napi_value result;
  napi_create_double(env, value, &result);
  return result;
}

napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor properties[] = {
      {"createClient", nullptr, CreateClient, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"platformName", nullptr, PlatformName, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"deviceCount", nullptr, DeviceCount, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"compile", nullptr, Compile, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"executeF32Scalar", nullptr, ExecuteF32Scalar, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties);
  return exports;
}

}  // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)

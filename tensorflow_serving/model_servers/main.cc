/* Copyright 2016 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// gRPC server implementation of
// tensorflow_serving/apis/prediction_service.proto.
//
// It bring up a standard server to serve a single TensorFlow model using
// command line flags, or multiple models via config file.
//
// ModelServer prioritizes easy invocation over flexibility,
// and thus serves a statically configured set of models. New versions of these
// models will be loaded and managed over time using the EagerLoadPolicy at:
//     tensorflow_serving/core/eager_load_policy.h.
// by AspiredVersionsManager at:
//     tensorflow_serving/core/aspired_versions_manager.h
//
// ModelServer has inter-request batching support built-in, by using the
// BatchingSession at:
//     tensorflow_serving/batching/batching_session.h
//
// To serve a single model, run with:
//     $path_to_binary/tensorflow_model_server \
//     --model_base_path=[/tmp/my_model | gs://gcs_address]
// IMPORTANT: Be sure the base path excludes the version directory. For
// example for a model at /tmp/my_model/123, where 123 is the version, the base
// path is /tmp/my_model.
//
// To specify model name (default "default"): --model_name=my_name
// To specify port (default 8500): --port=my_port
// To enable batching (default disabled): --enable_batching
// To log on stderr (default disabled): --alsologtostderr

#include <unistd.h>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "google/protobuf/wrappers.pb.h"
#include "grpc++/security/server_credentials.h"
#include "grpc++/server.h"
#include "grpc++/server_builder.h"
#include "grpc++/server_context.h"
#include "grpc++/support/status.h"
#include "grpc++/support/status_code_enum.h"
#include "grpc/grpc.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/init_main.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/protobuf/config.pb.h"
#include "tensorflow/core/util/command_line_flags.h"
#include "tensorflow_serving/apis/prediction_service.grpc.pb.h"
#include "tensorflow_serving/apis/prediction_service.pb.h"
#include "tensorflow_serving/config/model_server_config.pb.h"
#include "tensorflow_serving/core/eager_load_policy.h"
#include "tensorflow_serving/model_servers/model_platform_types.h"
#include "tensorflow_serving/model_servers/platform_config_util.h"
#include "tensorflow_serving/model_servers/server_core.h"
#include "tensorflow_serving/servables/tensorflow/predict_impl.h"

using tensorflow::serving::AspiredVersionsManager;
using tensorflow::serving::AspiredVersionPolicy;
using tensorflow::serving::BatchingParameters;
using tensorflow::serving::EagerLoadPolicy;
using tensorflow::serving::EventBus;
using tensorflow::serving::FileSystemStoragePathSourceConfig;
using tensorflow::serving::FileSystemStoragePathSourceConfig_VersionPolicy;
using tensorflow::serving::FileSystemStoragePathSourceConfig_VersionPolicy_Name;
using tensorflow::serving::Loader;
using tensorflow::serving::ModelServerConfig;
using tensorflow::serving::ServableState;
using tensorflow::serving::ServerCore;
using tensorflow::serving::SessionBundleConfig;
using tensorflow::serving::Target;
using tensorflow::serving::TensorflowPredictor;
using tensorflow::serving::UniquePtrWithDeps;
using tensorflow::string;

using grpc::InsecureServerCredentials;
using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerCompletionQueue;
using tensorflow::serving::PredictRequest;
using tensorflow::serving::PredictResponse;
using tensorflow::serving::PredictionService;

namespace {

tensorflow::Status LoadCustomModelConfig(
    const ::google::protobuf::Any& any,
    EventBus<ServableState>* servable_event_bus,
    UniquePtrWithDeps<AspiredVersionsManager>* manager) {
  CHECK(false)  // Crash ok
      << "ModelServer does not yet support custom model config.";
}

ModelServerConfig BuildSingleModelConfig(
    const string& model_name, const string& model_base_path,
    const FileSystemStoragePathSourceConfig_VersionPolicy&
        model_version_policy) {
  ModelServerConfig config;
  LOG(INFO) << "Building single TensorFlow model file config: "
            << " model_name: " << model_name
            << " model_base_path: " << model_base_path
            << " model_version_policy: " << model_version_policy;
  tensorflow::serving::ModelConfig* single_model =
      config.mutable_model_config_list()->add_config();
  single_model->set_name(model_name);
  single_model->set_base_path(model_base_path);
  single_model->set_model_platform(
      tensorflow::serving::kTensorFlowModelPlatform);
  single_model->set_version_policy(model_version_policy);
  return config;
}

grpc::Status ToGRPCStatus(const tensorflow::Status& status) {
  const int kErrorMessageLimit = 1024;
  string error_message;
  if (status.error_message().length() > kErrorMessageLimit) {
    error_message =
        status.error_message().substr(0, kErrorMessageLimit) + "...TRUNCATED";
  } else {
    error_message = status.error_message();
  }
  return grpc::Status(static_cast<grpc::StatusCode>(status.code()),
                      error_message);
}

class PredictionServiceImpl final : public PredictionService::Service {
 public:
  explicit PredictionServiceImpl(std::unique_ptr<ServerCore> core,
                                 bool use_saved_model)
      : core_(std::move(core)),
        predictor_(new TensorflowPredictor(use_saved_model)) {}

  grpc::Status Predict(ServerContext* context, const PredictRequest* request,
                       PredictResponse* response) override {
    const grpc::Status status =
        ToGRPCStatus(predictor_->Predict(core_.get(), *request, response));
    if (!status.ok()) {
      VLOG(1) << "Predict failed: " << status.error_message();
    }
    return status;
  }

 private:
  std::unique_ptr<ServerCore> core_;
  std::unique_ptr<TensorflowPredictor> predictor_;
};

void RunServer(int port, std::unique_ptr<ServerCore> core,
               bool use_saved_model) {
  // "0.0.0.0" is the way to listen on localhost in gRPC.
  const string server_address = "0.0.0.0:" + std::to_string(port);
  PredictionServiceImpl service(std::move(core), use_saved_model);
  ServerBuilder builder;
  std::shared_ptr<grpc::ServerCredentials> creds = InsecureServerCredentials();
  builder.AddListeningPort(server_address, creds);
  builder.RegisterService(&service);
  builder.SetMaxMessageSize(tensorflow::kint32max);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  LOG(INFO) << "Running ModelServer at " << server_address << " ...";
  server->Wait();
}

// Parses an ascii PlatformConfigMap protobuf from 'file'.
tensorflow::serving::PlatformConfigMap ParsePlatformConfigMap(
    const string& file) {
  std::unique_ptr<tensorflow::ReadOnlyMemoryRegion> file_data;
  TF_CHECK_OK(  // Crash ok
      tensorflow::Env::Default()->NewReadOnlyMemoryRegionFromFile(file,
                                                                  &file_data));
  string file_data_str(static_cast<const char*>(file_data->data()),
                       file_data->length());
  tensorflow::serving::PlatformConfigMap platform_config_map;
  QCHECK(tensorflow::protobuf::TextFormat::ParseFromString(  // Crash ok
      file_data_str, &platform_config_map));
  return platform_config_map;
}

}  // namespace

int main(int argc, char** argv) {
  tensorflow::int32 port = 8500;
  bool enable_batching = false;
  tensorflow::string model_name = "default";
  tensorflow::int32 file_system_poll_wait_seconds = 1;
  tensorflow::string model_base_path;
  bool use_saved_model = true;
  // Tensorflow session parallelism of zero means that both inter and intra op
  // thread pools will be auto configured.
  tensorflow::int64 tensorflow_session_parallelism = 0;
  string platform_config_file = "";
  tensorflow::string model_version_policy =
      FileSystemStoragePathSourceConfig_VersionPolicy_Name(
          FileSystemStoragePathSourceConfig::LATEST_VERSION);
  std::vector<tensorflow::Flag> flag_list = {
      tensorflow::Flag("port", &port, "port to listen on"),
      tensorflow::Flag("enable_batching", &enable_batching, "enable batching"),
      tensorflow::Flag("model_name", &model_name, "name of model"),
      tensorflow::Flag(
          "model_version_policy", &model_version_policy,
          "The version policy which determines the number of model versions to "
          "be served at the same time. The default value is LATEST_VERSION, "
          "which will serve only the latest version. See "
          "file_system_storage_path_source.proto for the list of possible "
          "VersionPolicy."),
      tensorflow::Flag("file_system_poll_wait_seconds",
                       &file_system_poll_wait_seconds,
                       "interval in seconds between each poll of the file "
                       "system for new model version"),
      tensorflow::Flag("model_base_path", &model_base_path,
                       "path to export (required)"),
      tensorflow::Flag("use_saved_model", &use_saved_model,
                       "If true, use SavedModel in the server; otherwise, use "
                       "SessionBundle. It is used by tensorflow serving team "
                       "to control the rollout of SavedModel and is not "
                       "expected to be set by users directly."),
      tensorflow::Flag("tensorflow_session_parallelism",
                       &tensorflow_session_parallelism,
                       "Number of threads to use for running a "
                       "Tensorflow session. Auto-configured by default."
                       "Note that this option is ignored if "
                       "--platform_config_file is non-empty."),
      tensorflow::Flag("platform_config_file", &platform_config_file,
                       "If non-empty, read an ascii PlatformConfigMap protobuf "
                       "from the supplied file name, and use that platform "
                       "config instead of the Tensorflow platform. (If used, "
                       "--enable_batching and --use_saved_model are "
                       "ignored.)")};
  string usage = tensorflow::Flags::Usage(argv[0], flag_list);
  const bool parse_result = tensorflow::Flags::Parse(&argc, argv, flag_list);
  if (!parse_result || model_base_path.empty()) {
    std::cout << usage;
    return -1;
  }
  tensorflow::port::InitMain(argv[0], &argc, &argv);
  if (argc != 1) {
    std::cout << "unknown argument: " << argv[1] << "\n" << usage;
  }

  FileSystemStoragePathSourceConfig_VersionPolicy parsed_version_policy;
  bool valid_policy = FileSystemStoragePathSourceConfig_VersionPolicy_Parse(
      model_version_policy, &parsed_version_policy);
  QCHECK(valid_policy)  // Crash ok.
      << "Invalid model_version_policy input argument: " << model_version_policy
      << "\n"
      << usage;

  // For ServerCore Options, we leave servable_state_monitor_creator unspecified
  // so the default servable_state_monitor_creator will be used.
  ServerCore::Options options;
  options.model_server_config = BuildSingleModelConfig(
      model_name, model_base_path, parsed_version_policy);

  if (platform_config_file.empty()) {
    SessionBundleConfig session_bundle_config;
    // Batching config
    if (enable_batching) {
      BatchingParameters* batching_parameters =
          session_bundle_config.mutable_batching_parameters();
      batching_parameters->mutable_thread_pool_name()->set_value(
          "model_server_batch_threads");
    }

    session_bundle_config.mutable_session_config()
        ->set_intra_op_parallelism_threads(tensorflow_session_parallelism);
    session_bundle_config.mutable_session_config()
        ->set_inter_op_parallelism_threads(tensorflow_session_parallelism);
    options.platform_config_map = CreateTensorFlowPlatformConfigMap(
        session_bundle_config, use_saved_model);
  } else {
    options.platform_config_map = ParsePlatformConfigMap(platform_config_file);
  }

  options.custom_model_config_loader = &LoadCustomModelConfig;

  options.aspired_version_policy =
      std::unique_ptr<AspiredVersionPolicy>(new EagerLoadPolicy);
  options.file_system_poll_wait_seconds = file_system_poll_wait_seconds;

  std::unique_ptr<ServerCore> core;
  TF_CHECK_OK(ServerCore::Create(std::move(options), &core));
  RunServer(port, std::move(core), use_saved_model);

  return 0;
}

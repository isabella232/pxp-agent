#include <cthun-agent/request_processor.hpp>
#include <cthun-agent/action_outcome.hpp>
#include <cthun-agent/configuration.hpp>
#include <cthun-agent/file_utils.hpp>
#include <cthun-agent/string_utils.hpp>
#include <cthun-agent/rpc_schemas.hpp>
#include <cthun-agent/timer.hpp>
#include <cthun-agent/uuid.hpp>
#include <cthun-agent/external_module.hpp>
#include <cthun-agent/modules/echo.hpp>
#include <cthun-agent/modules/inventory.hpp>
#include <cthun-agent/modules/ping.hpp>
#include <cthun-agent/modules/status.hpp>

#define LEATHERMAN_LOGGING_NAMESPACE "puppetlabs.cthun_agent.action_executer"
#include <leatherman/logging/logging.hpp>

#include <boost/filesystem/operations.hpp>

#include <vector>
#include <atomic>
#include <functional>
#include <stdexcept>  // out_of_range

namespace CthunAgent {

namespace fs = boost::filesystem;

//
// Results Storage
//

class ResultsStorage {
  public:
    // Throw a file_error in case of failure while writing to any of
    // result files
    ResultsStorage(const ActionRequest& request, const std::string& results_dir)
            : module { request.module() },
              action { request.action() },
              out_path { results_dir + "/stdout" },
              err_path { results_dir + "/stderr" },
              status_path { results_dir + "/status" },
              action_status {} {
        initialize(request, results_dir);
    }

    void write(const ActionOutcome& outcome, const std::string& exec_error,
               const std::string& duration) {
        action_status.set<std::string>("status", "completed");
        action_status.set<std::string>("duration", duration);
        FileUtils::writeToFile(action_status.toString() + "\n", status_path);

        if (exec_error.empty()) {
            if (outcome.type == ActionOutcome::Type::External) {
                FileUtils::writeToFile(outcome.stdout + "\n", out_path);
                if (!outcome.stderr.empty()) {
                    FileUtils::writeToFile(outcome.stderr + "\n", err_path);
                }
            } else {
                // ActionOutcome::Type::Internal
                FileUtils::writeToFile(outcome.results.toString() + "\n", out_path);
            }
        } else {
            FileUtils::writeToFile(exec_error, err_path);
        }
    }

  private:
    std::string module;
    std::string action;
    std::string out_path;
    std::string err_path;
    std::string status_path;
    CthunClient::DataContainer action_status;

    void initialize(const ActionRequest& request, const std::string results_dir) {
        if (!fs::exists(results_dir)) {
            LOG_DEBUG("Creating results directory for '%1% %2%', transaction "
                       "%3%, in '%4%'", request.module(), request.action(),
                       request.transactionId(), results_dir);
            if (!FileUtils::createDirectory(results_dir)) {
                throw file_error { "failed to create directory '"
                                   + results_dir + "'" };
            }
        }

        action_status.set<std::string>("module", module);
        action_status.set<std::string>("action", action);
        action_status.set<std::string>("status", "running");
        action_status.set<std::string>("duration", "0 s");

        if (!request.paramsTxt().empty()) {
            action_status.set<std::string>("input", request.paramsTxt());
        } else {
            action_status.set<std::string>("input", "none");
        }

        FileUtils::writeToFile("", out_path);
        FileUtils::writeToFile("", err_path);
        FileUtils::writeToFile(action_status.toString() + "\n", status_path);
    }
};

//
// Non-blocking action task
//

void nonBlockingActionTask(std::shared_ptr<Module> module_ptr,
                           ActionRequest&& request,
                           std::string job_id,
                           ResultsStorage results_storage,
                           std::shared_ptr<CthunConnector> connector_ptr,
                           std::shared_ptr<std::atomic<bool>> done) {
    CthunAgent::Timer timer {};
    std::string exec_error {};
    ActionOutcome outcome {};

    try {
        outcome = module_ptr->executeAction(request);

        if (request.parsedChunks().data.get<bool>("notify_outcome")) {
            connector_ptr->sendNonBlockingResponse(request, outcome.results, job_id);
        }
    } catch (request_error& e) {
        connector_ptr->sendRPCError(request, e.what());
        exec_error = "Failed to execute '" + request.module() + " "
                     + request.action() + "': " + e.what() + "\n";
    }

    // Store results on disk
    auto duration = std::to_string(timer.elapsedSeconds()) + " s";
    results_storage.write(outcome, exec_error, duration);

    // Flag end of processing
    *done = true;
}

//
// Public interface
//

RequestProcessor::RequestProcessor(std::shared_ptr<CthunConnector> connector_ptr,
                                   const std::string& modules_dir)
        : thread_container_ { "Action Executer" },
          connector_ptr_ { connector_ptr },
          spool_dir_ { Configuration::Instance().get<std::string>("spool-dir") } {
    if (!fs::exists(spool_dir_)) {
        LOG_INFO("Creating spool directory '%1%'", spool_dir_);
        if (!FileUtils::createDirectory(spool_dir_)) {
            throw fatal_error { "failed to create the results directory '"
                                + spool_dir_ + "'" };
        }
    }

    // NB: certificate paths are validated by HW
    loadInternalModules();

    if (!modules_dir.empty()) {
        loadExternalModulesFrom(modules_dir);
    } else {
        LOG_INFO("The modules directory was not provided; no external module "
                 "will be loaded");
    }

    logLoadedModules();
}

void RequestProcessor::processRequest(const RequestType& request_type,
                                      const CthunClient::ParsedChunks& parsed_chunks) {
    try {
        // Inspect and validate the request message format
        ActionRequest request { request_type, parsed_chunks };

        LOG_INFO("About to process %1% request %2% by %3%, transaction %4%",
                 requestTypeNames[request_type], request.id(), request.sender(),
                 request.transactionId());

        try {
            // We can access the request content; validate it
            validateRequestContent(request);
        } catch (request_validation_error& e) {
            // Invalid request; send *RPC error*

            LOG_ERROR("Invalid %1% request %2% by %3%, transaction %4%: %5%",
                      requestTypeNames[request_type], request.id(),
                      request.sender(), request.transactionId(), e.what());
            connector_ptr_->sendRPCError(request, e.what());
            return;
        }

        try {
            if (request.type() == RequestType::Blocking) {
                processBlockingRequest(request);
            } else {
                processNonBlockingRequest(request);
            }
        } catch (request_error& e) {
            // Process failure; send *RPC error*
            LOG_ERROR("Failed to process %1% request %2% by %3%, transaction %4%: "
                      "%5%", requestTypeNames[request.type()], request.id(),
                      request.sender(), request.transactionId(), e.what());
            connector_ptr_->sendRPCError(request, e.what());
        }
    } catch (request_format_error& e) {
        // Failed to instantiate ActionRequest - bad message; send *Cthun error*

        auto id = parsed_chunks.envelope.get<std::string>("id");
        auto sender = parsed_chunks.envelope.get<std::string>("sender");
        std::vector<std::string> endpoints { sender };
        LOG_ERROR("Invalid %1% request by %2%: %3%", id, sender, e.what());
        connector_ptr_->sendCthunError(id, e.what(), endpoints);
    }
}

//
// Private interface
//

void RequestProcessor::validateRequestContent(const ActionRequest& request) {
    // Validate requested module and action
    try {
        if (!modules_.at(request.module())->hasAction(request.action())) {
            throw request_validation_error { "unknown action '" + request.action()
                                             + "' for module " + request.module() };
        }
    } catch (std::out_of_range& e) {
        throw request_validation_error { "unknown module: " + request.module() };
    }

    // Validate request input params
    try {
        LOG_DEBUG("Validating input for parameters of '%1% %2%' request %3% "
                  "by %4%, transaction %5%", request.module(), request.action(),
                  request.id(), request.sender(), request.transactionId());

        // TODO(ale): try to make const the DataContainer argument of
        // CthunClient::Validator::validate() to avoid copying

        CthunClient::DataContainer input_params { request.params() };
        auto& validator = modules_.at(request.module())->input_validator_;

        // NB: the registred schemas have the same name as the action
        validator.validate(input_params, request.action());
    } catch (CthunClient::validation_error) {
        throw request_validation_error { "invalid input for '" + request.module()
                                         + " " + request.action() + "'" };
    }
}

void RequestProcessor::processBlockingRequest(const ActionRequest& request) {
    // Execute action; possible request errors will be propagated
    auto outcome = modules_[request.module()]->executeAction(request);

    connector_ptr_->sendBlockingResponse(request, outcome.results);
}

void RequestProcessor::processNonBlockingRequest(const ActionRequest& request) {
    auto job_id = UUID::getUUID();

    // HERE(ale): assuming spool_dir ends with '/' (up to Configuration)
    std::string results_dir { spool_dir_ + job_id };

    LOG_DEBUG("Starting '%1% %2%' job with ID %3% for non-blocking request %4% "
              "by %5%, transaction %6%", request.module(), request.action(),
              job_id, request.id(), request.sender(), request.transactionId());

    // To keep track of errors and write them on file
    std::string err_msg {};

    try {
        // Flag to enable signaling from task to thread_container
        auto done = std::make_shared<std::atomic<bool>>(false);

        thread_container_.add(std::thread(&nonBlockingActionTask,
                                          modules_[request.module()],
                                          request,
                                          job_id,
                                          ResultsStorage { request, results_dir },
                                          connector_ptr_,
                                          done),
                              done);
    } catch (file_error& e) {
        // Failed to instantiate ResultsStorage
        LOG_ERROR("Failed to initialize the result files for '%1% %2%' action "
                  "job with ID %3%: %4%", request.module(), request.action(),
                  job_id, e.what());
        err_msg = std::string { "failed to initialize result files: " } + e.what();
    } catch (std::exception& e) {
        LOG_ERROR("Failed to spawn '%1% %2%' action job with ID %3%: %4%",
                  request.module(), request.action(), job_id, e.what());
        err_msg = std::string { "failed to start action task: " } + e.what();
    }

    // Send back provisional data
    connector_ptr_->sendProvisionalResponse(request, job_id, err_msg);
}

void RequestProcessor::loadInternalModules() {
    modules_["echo"] = std::shared_ptr<Module>(new Modules::Echo);
    modules_["inventory"] = std::shared_ptr<Module>(new Modules::Inventory);
    modules_["ping"] = std::shared_ptr<Module>(new Modules::Ping);
    modules_["status"] = std::shared_ptr<Module>(new Modules::Status);
}

void RequestProcessor::loadExternalModulesFrom(fs::path dir_path) {
    LOG_INFO("Loading external modules from %1%", dir_path.string());

    if (fs::is_directory(dir_path)) {
        fs::directory_iterator end;

        for (auto f = fs::directory_iterator(dir_path); f != end; ++f) {
            if (!fs::is_directory(f->status())) {
                auto f_p = f->path().string();

                try {
                    ExternalModule* e_m = new ExternalModule(f_p);
                    modules_[e_m->module_name] = std::shared_ptr<Module>(e_m);
                } catch (module_error& e) {
                    LOG_ERROR("Failed to load %1%; %2%", f_p, e.what());
                } catch (std::exception& e) {
                    LOG_ERROR("Unexpected error when loading %1%; %2%",
                              f_p, e.what());
                } catch (...) {
                    LOG_ERROR("Unexpected error when loading %1%", f_p);
                }
            }
        }
    } else {
        LOG_WARNING("Failed to locate the modules directory; no external "
                    "module will be loaded");
    }
}

void RequestProcessor::logLoadedModules() const {
    for (auto& module : modules_) {
        std::string txt { "found no action" };
        std::string actions_list { "" };

        for (auto& action : module.second->actions) {
            if (actions_list.empty()) {
                txt = "action";
                actions_list += ": ";
            } else {
                actions_list += ", ";
            }
            actions_list += action;
        }

        auto txt_suffix = StringUtils::plural(module.second->actions.size());
        LOG_INFO("Loaded '%1%' module - %2%%3%%4%",
                 module.first, txt, txt_suffix, actions_list);
    }
}

}  // namespace CthunAgent

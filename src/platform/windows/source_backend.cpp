/**
 * @file src/platform/windows/source_backend.cpp
 * @brief Concrete, explicitly inactive Windows source preparation and recovery.
 */
// header include
#include "source_backend.h"

// standard includes
#include <algorithm>
#include <mutex>
#include <string>
#include <utility>

// lib includes
#include <boost/algorithm/string/predicate.hpp>
#include <display_device/windows/win_api_layer.h>
#include <dxgi1_2.h>
#include <objbase.h>
#include <wrl/client.h>

// local includes
#include "capture_binding.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "utf_utils.h"

namespace platf::source_backend {
  namespace {
    using namespace rtsp_stream;
    using phase_e = source_backend_status_t::phase_e;
    using owned_phase_e = display_device::owned_display_phase_e;

    /** @brief Deferred operating-system boundaries; copying them never invokes them. */
    struct dependencies_t {
      std::function<std::shared_ptr<display_device::WinApiLayerInterface>()> make_api;  ///< Deferred readback API allocation.
      std::function<std::vector<std::string>()> capture_names;  ///< Existing capturable-selector discovery.
      std::function<void(const std::string &)> wake_native;  ///< Ordinary native wake; never used for Virtual.
      std::function<std::optional<LUID>(const std::string &)> render_adapter;  ///< Exact configured render adapter.
      std::function<std::unique_ptr<sudovda::system_t>()> make_system;  ///< Deferred driver transport allocation.
      std::function<std::expected<std::unique_ptr<display_device::owned_display_configuration_t>, sudovda::failure_t>(const std::filesystem::path &, const GUID &, std::uint64_t, const sudovda::recovery_authority_t &)> begin_owned;  ///< Existing B intent transaction.
#ifdef SUNSHINE_TESTS
      std::function<int(const capture_source_t &, std::shared_ptr<capture_pin_t>)> probe {};  ///< Fake-only codec boundary; production always uses the managed video entry.
#endif
    };

    std::optional<LUID> resolve_render_adapter(const std::string &name) {
      Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
      if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return std::nullopt;
      }
      const auto selected = utf_utils::from_utf8(name);
      std::optional<LUID> result;
      for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const auto status = factory->EnumAdapters1(index, &adapter);
        if (status == DXGI_ERROR_NOT_FOUND) {
          break;
        }
        DXGI_ADAPTER_DESC1 description {};
        if (FAILED(status) || FAILED(adapter->GetDesc1(&description))) {
          return std::nullopt;
        }
        if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || (!selected.empty() && selected != description.Description)) {
          continue;
        }
        if (selected.empty()) {
          return description.AdapterLuid;
        }
        if (result) {
          return std::nullopt;
        }
        result = description.AdapterLuid;
      }
      return result;
    }

    dependencies_t native_dependencies() {
      return {
        []() {
          return std::make_shared<display_device::WinApiLayer>();
        },
        []() {
          return platf::display_names(mem_type_e::dxgi);
        },
        [](const auto &selector) {
          std::ignore = display_device::wake_display(selector, std::chrono::milliseconds {500});
        },
        resolve_render_adapter,
        sudovda::make_win32_system,
        display_device::owned_display_configuration_t::begin
      };
    }

    std::optional<bool> checked_availability(const display_device::WinApiLayerInterface &api) {
      const auto queried = api.queryDisplayConfig(display_device::QueryType::All);
      if (!queried) {
        return std::nullopt;
      }
      // A successful empty Windows path query proves the empty baseline. Inactive but available
      // targets still permit ordinary native configuration; missing active selectors are errors.
      for (const auto &path : queried->m_paths) {
        if (!path.targetInfo.targetAvailable) {
          continue;
        }
        if (api.getDeviceId(path).empty() || api.getMonitorDevicePath(path).empty()) {
          return std::nullopt;
        }
        return true;
      }
      return false;
    }

    void copy_observation(capture_source_t &source, const capture_binding::observation_t &observed) {
      source.width = observed.width;
      source.height = observed.height;
      source.rate_numerator = observed.rate_numerator;
      source.rate_denominator = observed.rate_denominator;
      source.observed_hdr = observed.hdr;
    }

    /** @brief One operation-lane dispatcher with an independently copied, nonblocking owner view. */
    class backend_t final: public source_backend_t {
    public:
      backend_t(options_t options, dependencies_t dependencies):
          options_ {std::move(options)},
          dependencies_ {std::move(dependencies)} {}

      bool admits(const display_policy::source_request_t &request) const override {
        const auto startup = display_device::ordinary_startup_recovery();
        if (startup.mutation_excluded || (request.virtual_display && startup.recovery_pending)) {
          return false;
        }
        const auto current = status();
        std::lock_guard lock {publication_mutex_};
        if (current.generation && !current.source_released) {
          return false;
        }
        return !episode_live_ || request.virtual_display == episode_virtual_;
      }

      source_backend_status_t status() const override {
        std::lock_guard lock {publication_mutex_};
        auto result = published_;
        if (episode_live_ && !episode_virtual_) {
          const auto native = native_.status();
          result.recovery_pending = native.recovery_pending;
          result.recovery_retry_required = native.recovery_retry_required;
        }
        // This accessor only copies the lease's asynchronous failure latch. It performs no
        // transport, scheduler or owner work, even while an operation worker is blocked.
        result.heartbeat_failed = result.heartbeat_failed || (heartbeat_view_ && heartbeat_view_->heartbeat_failure().has_value());
        return result;
      }

      diagnostics_t diagnostics() const {
        std::lock_guard lock {publication_mutex_};
        auto result = diagnostics_;
        if (!result.liveness && heartbeat_view_) {
          result.liveness = heartbeat_view_->heartbeat_failure();
        }
        return result;
      }

      bool begin(const display_policy::source_request_t &request, std::uint64_t generation, std::uint64_t epoch) override {
        if (!admits(request) || !generation || generation <= status().generation) {
          return false;
        }
        // Initialize the logical fence before validation. NoEffects is explicit and cannot stand in
        // for an adapter receipt after B intent/acquisition has been entered.
        generation_ = generation;
        epoch_ = epoch;
        effects_ = effects_e::none;
        retired_ = false;
        removed_waiting_close_ = false;
        {
          std::lock_guard lock {publication_mutex_};
          published_ = {.generation = generation, .phase = phase_e::failed};
          diagnostics_ = {};
        }
        request_ = request;
        if (!epoch || request.restore_delay < std::chrono::milliseconds::zero()) {
          return false;
        }
        if (!request.virtual_display) {
          effects_ = effects_e::native;
          if (!native_.begin(request.native_action == display_policy::native_action_e::configure ? request.native_configuration : std::nullopt, request.native_action == display_policy::native_action_e::revert, generation, request.restore_delay)) {
            return false;
          }
          set_episode(false);
          api_ = dependencies_.make_api();
          if (!api_) {
            return false;
          }
          auto source = select_native();
          if (!source) {
            return false;
          }
          publish_ready(std::move(*source));
          return true;
        }
        if (!options_.authority) {
          acquisition_failure({sudovda::errc_t::exclusive_control_required, sudovda::operation_t::journal});
          return false;
        }
        const auto authority = options_.authority->verify(options_.journal_path, request, generation);
        if (!authority || authority->deployment.empty() || !authority->trusted_storage || !authority->exclusive_driver_control) {
          acquisition_failure(authority ? sudovda::failure_t {sudovda::errc_t::ownership_unverified, sudovda::operation_t::journal} : authority.error());
          return false;
        }
        GUID identity {};
        const auto canonical = display_policy::canonical_client_uuid(request.monitor_uuid);
        if (!canonical || *canonical != request.monitor_uuid || FAILED(CLSIDFromString(utf_utils::from_utf8("{" + request.monitor_uuid + "}").c_str(), &identity))) {
          acquisition_failure({sudovda::errc_t::invalid_metadata, sudovda::operation_t::journal});
          return false;
        }
        const auto render_adapter = dependencies_.render_adapter(request.adapter_name);
        if (!render_adapter) {
          acquisition_failure({sudovda::errc_t::enumeration_failed, sudovda::operation_t::render_adapter});
          return false;
        }
        authority_ = *authority;
        if (owned_) {
          if (!owned_->journal()->validate(authority_)) {
            acquisition_failure({sudovda::errc_t::ownership_unverified, sudovda::operation_t::journal});
            return false;
          }
          const auto previous = owned_->status().generation;
          if (!owned_->begin_next(identity, generation)) {
            if (owned_->journal()->record().generation == generation) {
              effects_ = effects_e::owned;
              // Rotation wrote new intent before reconstruction failed. Obtain an actual unarmed
              // adapter receipt without opening a transport; NoEffects cannot retire this intent.
              const sudovda::request_t unarmed {identity, *render_adapter, request.mode.resolution.width, request.mode.resolution.height, request.mode.refresh_rate.numerator, request.mode.refresh_rate.denominator, request.edid_name, request.edid_serial, authority_.exclusive_driver_control};
              auto proof = sudovda::lease_t::acquire_owned(nullptr, unarmed, owned_->journal(), authority_);
              receipt_ = std::move(proof.cleanup_receipt);
            } else {
              // begin_next may have stopped the previous recovery retry before rejecting rollover.
              std::ignore = owned_->restore(previous);
            }
            return false;
          }
          effects_ = effects_e::owned;
        } else {
          effects_ = effects_e::uncertain;
          set_episode(true);
          auto transaction = dependencies_.begin_owned(options_.journal_path, identity, generation, authority_);
          if (!transaction) {
            acquisition_failure(transaction.error());
            if (transaction.error().code == sudovda::errc_t::busy || transaction.error().code == sudovda::errc_t::enumeration_failed) {
              // These B failures occur before durable intent is created.
              effects_ = effects_e::none;
              std::lock_guard lock {publication_mutex_};
              episode_live_ = false;
            }
            return false;
          }
          owned_ = std::move(*transaction);
          effects_ = effects_e::owned;
        }
        receipt_.reset();
        {
          std::lock_guard lock {publication_mutex_};
          heartbeat_view_ = nullptr;
        }
        lease_.reset();
        api_ = dependencies_.make_api();
        const sudovda::request_t acquisition {identity, *render_adapter, request.mode.resolution.width, request.mode.resolution.height, request.mode.refresh_rate.numerator, request.mode.refresh_rate.denominator, request.edid_name, request.edid_serial, authority_.exclusive_driver_control};
        auto acquired = sudovda::lease_t::acquire_owned(dependencies_.make_system(), acquisition, owned_->journal(), authority_);
        lease_ = std::move(acquired.lease);
        receipt_ = std::move(acquired.cleanup_receipt);
        {
          std::lock_guard lock {publication_mutex_};
          diagnostics_ = {acquired.failure, acquired.cleanup_failure, acquired.liveness_failure};
          heartbeat_view_ = lease_.get();
        }
        if (acquired.failure || !lease_ || !api_ || !lease_->display() || !owned_->bind(*lease_->display(), generation)) {
          return false;
        }
        auto configuration = virtual_configuration();
        if (!owned_->prepare(configuration, generation)) {
          return false;
        }
        {
          std::lock_guard lock {publication_mutex_};
          published_.phase = phase_e::preparing;
        }
        progress(generation);
        return status().phase != phase_e::failed;
      }

      void progress(std::uint64_t generation) override {
        if (generation != generation_ || !owned_) {
          return;
        }
        const auto observed = owned_->status();
        const auto heartbeat = lease_ ? lease_->heartbeat_failure() : std::nullopt;
        {
          std::lock_guard lock {publication_mutex_};
          if (heartbeat && !diagnostics_.liveness) {
            diagnostics_.liveness = heartbeat;
          }
          published_.heartbeat_failed = diagnostics_.liveness.has_value();
          published_.recovery_pending = observed.source_released && (observed.unavailable_settings_pending || observed.phase != owned_phase_e::restored);
          published_.recovery_retry_required = observed.source_released && observed.restore_retry_required;
          if (effects_ == effects_e::none) {
            return;
          }
          switch (observed.phase) {
            case owned_phase_e::ready:
              published_.phase = phase_e::preparing;
              break;
            case owned_phase_e::reverting:
              published_.phase = phase_e::reverting;
              break;
            case owned_phase_e::removal_ready:
              published_.phase = phase_e::removal_ready;
              break;
            case owned_phase_e::restored:
              published_.phase = phase_e::restored;
              break;
            case owned_phase_e::recovering:
              published_.phase = phase_e::reverting;
              break;
            case owned_phase_e::cleanup_pending:
              published_.phase = phase_e::cleanup_pending;
              break;
            case owned_phase_e::failed:
            case owned_phase_e::cancelled:
              published_.phase = phase_e::failed;
              break;
            default:
              published_.phase = phase_e::preparing;
              break;
          }
          published_.source_released = retired_ && observed.source_released;
        }
        if (!retired_ && observed.generation == generation && observed.phase == owned_phase_e::ready && lease_ && lease_->display() && api_) {
          auto source = virtual_source(observed);
          const auto checked = source ? capture_binding::verify(*source, *api_) : std::nullopt;
          if (checked) {
            copy_observation(*source, *checked);
            publish_ready(std::move(*source));
          } else {
            std::lock_guard lock {publication_mutex_};
            published_.phase = phase_e::failed;
          }
        }
      }

      bool validate_retained(const capture_source_t &source, std::uint64_t epoch) override {
        const auto current = status();
        if (!epoch || epoch <= epoch_ || !current.source || *current.source != source || retired_ || source.generation != generation_ || !api_) {
          return false;
        }
        if (request_.virtual_display && (!lease_ || !lease_->refresh_display() || !same_lease_mapping())) {
          return false;
        }
        const auto observed = capture_binding::verify(source, *api_);
        if (!observed) {
          return false;
        }
        auto next = source;
        next.epoch = epoch;
        copy_observation(next, *observed);
        epoch_ = epoch;
        publish_ready(std::move(next));
        return true;
      }

      int probe(const capture_source_t &source, std::shared_ptr<capture_pin_t> pin) override {
        const auto current = status();
        if (!pin || !current.source || *current.source != source || current.heartbeat_failed) {
          return 503;
        }
#ifdef SUNSHINE_TESTS
        if (dependencies_.probe) {
          return dependencies_.probe(source, std::move(pin));
        }
#endif
        return video::probe_encoders(source, std::move(pin)) == 0 ? 200 : 503;
      }

      bool restore(std::uint64_t generation, std::chrono::milliseconds delay) override {
        if (generation != generation_ || !generation || delay < std::chrono::milliseconds::zero()) {
          return false;
        }
        if (effects_ == effects_e::none) {
          std::lock_guard lock {publication_mutex_};
          published_.phase = phase_e::cleanup_pending;
          return true;
        }
        if (effects_ == effects_e::native) {
          if (!native_.restore(generation, delay)) {
            return false;
          }
          std::lock_guard lock {publication_mutex_};
          published_.phase = phase_e::cleanup_pending;
          return true;
        }
        if (!owned_ || effects_ == effects_e::uncertain) {
          return false;
        }
        if (owned_->status().phase == owned_phase_e::reserved && !bind_for_restoration()) {
          return false;
        }
        if (!owned_->restore(owned_->status().generation, delay)) {
          return false;
        }
        progress(generation);
        return true;
      }

      bool cleanup_and_retire(std::uint64_t generation) override {
        if (generation != generation_ || !generation) {
          return false;
        }
        if (effects_ == effects_e::none || effects_ == effects_e::native) {
          if (effects_ == effects_e::native && !native_.status().source_released) {
            return false;
          }
          retired_ = true;
          std::lock_guard lock {publication_mutex_};
          published_.source_released = true;
          published_.phase = phase_e::restored;
          // An older B episode remains independent of a denied NoEffects rollover attempt.
          published_.recovery_pending = owned_ != nullptr;
          return true;
        }
        if (!owned_) {
          return false;
        }
        const auto owned_generation = owned_->status().generation;
        if (!receipt_) {
          if (!lease_) {
            return false;
          }
          if (removed_waiting_close_) {
            if (!lease_->identity_absent() || !lease_->belongs_to(owned_->journal())) {
              return false;
            }
          } else if (!lease_->refresh_display() || !same_lease_mapping()) {
            return false;
          }
          // Refresh the retained exact target, then obtain current B removal permission. A
          // close-only retry already removed that target under the preceding permission.
          if (!removed_waiting_close_ && (!owned_->restore(owned_generation) || !owned_->status().removal_permitted)) {
            return false;
          }
          const auto cleaned = lease_->cleanup();
          if (!cleaned) {
            // A completed exact removal with a failed close cannot refresh an already removed
            // lease. Retry its transport close without performing another display removal.
            removed_waiting_close_ = cleaned.error().operation == sudovda::operation_t::close && lease_->identity_absent();
            std::lock_guard lock {publication_mutex_};
            diagnostics_.cleanup = cleaned.error();
            return false;
          }
          removed_waiting_close_ = false;
          receipt_ = lease_->cleanup_receipt();
        }
        if (!receipt_ || !owned_->retire_source(*receipt_, owned_generation)) {
          return false;
        }
        retired_ = true;
        {
          std::lock_guard lock {publication_mutex_};
          heartbeat_view_ = nullptr;
        }
        lease_.reset();
        progress(generation);
        return status().source_released;
      }

      bool complete(std::uint64_t generation) override {
        if (generation != generation_ || !retired_) {
          return false;
        }
        if (owned_) {
          if (!owned_->complete(owned_->status().generation)) {
            return false;
          }
          owned_.reset();
        } else if (effects_ == effects_e::uncertain || (effects_ == effects_e::native && !native_.complete(generation))) {
          return false;
        }
        std::lock_guard lock {publication_mutex_};
        episode_live_ = false;
        published_.recovery_pending = false;
        return true;
      }

      bool retry_recovery(std::uint64_t generation) override {
        if (!generation || generation != generation_ || !retired_) {
          return false;
        }
        if (owned_) {
          const auto current = owned_->status();
          if (!current.source_released) {
            return false;
          }
          if (current.restore_retry_required && !owned_->restore(current.generation)) {
            return false;
          }
          progress(generation);
        } else {
          const auto current = native_.status();
          if (current.recovery_retry_required && (!current.source_released || !native_.restore(current.generation, std::chrono::milliseconds::zero()))) {
            return false;
          }
        }
        return !status().recovery_retry_required;
      }

    private:
      enum class effects_e {
        none,
        native,
        owned,
        uncertain
      };

      void set_episode(bool virtual_display) {
        std::lock_guard lock {publication_mutex_};
        episode_live_ = true;
        episode_virtual_ = virtual_display;
      }

      void acquisition_failure(sudovda::failure_t failure) {
        std::lock_guard lock {publication_mutex_};
        diagnostics_.acquisition = failure;
      }

      void publish_ready(capture_source_t source) {
        std::lock_guard lock {publication_mutex_};
        published_.source = std::make_shared<const capture_source_t>(std::move(source));
        published_.phase = phase_e::ready;
      }

      std::optional<capture_source_t> select_native() {
        // Reuse Sunshine's capture feasibility enumeration, including native display-awake
        // behavior in the subsequent managed probe/capture. This operation never runs on owner.
        const auto mapped = display_device::map_output_name(request_.native_output);
        if (!request_.native_output.empty() && mapped.empty()) {
          return std::nullopt;
        }
        dependencies_.wake_native(mapped);
        const auto names = dependencies_.capture_names();
        const auto paths = api_->queryDisplayConfig(display_device::QueryType::All);
        if (!paths) {
          return std::nullopt;
        }
        std::optional<LUID> selected_adapter;
        if (!request_.adapter_name.empty()) {
          selected_adapter = dependencies_.render_adapter(request_.adapter_name);
          if (!selected_adapter) {
            return std::nullopt;
          }
        }
        for (const auto &name : names) {
          if (!mapped.empty() && name != mapped) {
            continue;
          }
          for (const auto &path : paths->m_paths) {
            if (!(path.flags & DISPLAYCONFIG_PATH_ACTIVE) || !path.targetInfo.targetAvailable || api_->getDisplayName(path) != name) {
              continue;
            }
            capture_source_t source;
            source.generation = generation_;
            source.epoch = epoch_;
            source.selector = name;
            source.device_id = api_->getDeviceId(path);
            source.windows_output = windows_output_t {capture_binding::luid(path.targetInfo.adapterId), path.targetInfo.id, api_->getMonitorDevicePath(path)};
            source.requested_hdr = request_.requested_hdr;
            if (const auto observed = capture_binding::observe(source, *api_)) {
              if (selected_adapter && observed->source_adapter != capture_binding::luid(*selected_adapter)) {
                continue;
              }
              copy_observation(source, *observed);
              return source;
            }
          }
        }
        return std::nullopt;
      }

      display_device::SingleDisplayConfiguration virtual_configuration() const {
        display_device::SingleDisplayConfiguration result;
        using prep_e = display_device::SingleDisplayConfiguration::DevicePreparation;
        switch (request_.topology) {
          case config::video_t::dd_t::config_option_e::ensure_primary:
            result.m_device_prep = prep_e::EnsurePrimary;
            break;
          case config::video_t::dd_t::config_option_e::ensure_only_display:
            result.m_device_prep = prep_e::EnsureOnlyDisplay;
            break;
          default:
            result.m_device_prep = prep_e::EnsureActive;
            break;
        }
        result.m_resolution = display_device::Resolution {request_.mode.resolution.width, request_.mode.resolution.height};
        result.m_refresh_rate = display_device::Rational {request_.mode.refresh_rate.numerator, request_.mode.refresh_rate.denominator};
        if (request_.requested_hdr) {
          result.m_hdr_state = *request_.requested_hdr ? display_device::HdrState::Enabled : display_device::HdrState::Disabled;
        }
        return result;
      }

      std::optional<capture_source_t> virtual_source(const display_device::owned_display_status_t &observed) const {
        if (!lease_ || !lease_->display() || observed.device_id.empty() || observed.display_name.empty()) {
          return std::nullopt;
        }
        const auto &display = *lease_->display();
        capture_source_t source;
        source.kind = capture_source_t::kind_e::owned_virtual;
        source.generation = generation_;
        source.epoch = epoch_;
        source.selector = observed.display_name;
        source.device_id = observed.device_id;
        source.monitor_uuid = request_.monitor_uuid;
        source.windows_output = windows_output_t {capture_binding::luid(display.target.adapter), display.target.target, utf_utils::to_utf8(display.monitor_path)};
        source.width = request_.mode.resolution.width;
        source.height = request_.mode.resolution.height;
        source.rate_numerator = request_.mode.refresh_rate.numerator;
        source.rate_denominator = request_.mode.refresh_rate.denominator;
        source.requested_hdr = request_.requested_hdr;
        return source;
      }

      bool same_lease_mapping() const {
        if (!lease_ || !owned_) {
          return false;
        }
        if (lease_->identity_absent()) {
          return lease_->belongs_to(owned_->journal());
        }
        const auto mapping = owned_->journal()->mapping();
        const auto &current = lease_->display();
        return mapping && current && capture_binding::luid(mapping->target.adapter) == capture_binding::luid(current->target.adapter) && mapping->target.target == current->target.target && boost::iequals(mapping->monitor_path, current->monitor_path) && owned_->status().device_id == mapping->device_id;
      }

      bool bind_for_restoration() {
        if (receipt_) {
          return receipt_->kind() == sudovda::cleanup_receipt_t::kind_e::unarmed || owned_->bind_absent(*receipt_, owned_->status().generation);
        }
        if (!lease_ && !owned_->journal()->add_was_armed()) {
          // Intent exists, but a fallible boundary prevented acquisition from being entered.
          // Ask the adapter for its real unarmed receipt without opening any transport.
          const sudovda::request_t unarmed {owned_->journal()->record().identity, {}, request_.mode.resolution.width, request_.mode.resolution.height, request_.mode.refresh_rate.numerator, request_.mode.refresh_rate.denominator, request_.edid_name, request_.edid_serial, authority_.exclusive_driver_control};
          auto proof = sudovda::lease_t::acquire_owned(nullptr, unarmed, owned_->journal(), authority_);
          receipt_ = std::move(proof.cleanup_receipt);
          return receipt_ && receipt_->kind() == sudovda::cleanup_receipt_t::kind_e::unarmed;
        }
        if (lease_ && !owned_->journal()->add_was_armed()) {
          const auto cleaned = lease_->cleanup();
          if (!cleaned) {
            std::lock_guard lock {publication_mutex_};
            diagnostics_.cleanup = cleaned.error();
            return false;
          }
          receipt_ = lease_->cleanup_receipt();
          return receipt_ && receipt_->kind() == sudovda::cleanup_receipt_t::kind_e::unarmed;
        }
        if (!lease_ || !lease_->refresh_display()) {
          return false;
        }
        if (lease_->identity_absent()) {
          return owned_->bind_absent(*lease_, owned_->status().generation);
        }
        return lease_->display() && owned_->bind(*lease_->display(), owned_->status().generation);
      }

      options_t options_;  ///< Stored verifier and journal location; construction performs no qualification.
      dependencies_t dependencies_;  ///< Deferred boundaries called only on the operation lane.
      display_device::native_display_configuration_t native_;  ///< Existing ordinary scheduler facade.
      std::shared_ptr<display_device::WinApiLayerInterface> api_;  ///< Readback API, allocated after begin enters.
      std::unique_ptr<display_device::owned_display_configuration_t> owned_;  ///< B recovery survives logical retirement.
      std::unique_ptr<sudovda::lease_t> lease_;  ///< Operation-lane ownership remains alive until checked cleanup.
      std::optional<sudovda::cleanup_receipt_t> receipt_;  ///< Adapter-issued exact retirement permission.
      sudovda::recovery_authority_t authority_;  ///< Independent verifier output for this operation.
      display_policy::source_request_t request_;  ///< Frozen request, never replaced by joining viewers.
      std::uint64_t generation_ {}, epoch_ {};  ///< Operation-lane fences.
      effects_e effects_ = effects_e::none;  ///< NoEffects differs from entered uncertain B intent.
      bool retired_ = false;  ///< Logical source retirement is independent of physical recovery.
      bool removed_waiting_close_ = false;  ///< Exact removal succeeded; retry close without repeating removal.
      mutable std::mutex publication_mutex_;  ///< Only copied state; never held across native work.
      source_backend_status_t published_;  ///< Copyable logical status; no resource pin is stored in metadata.
      const sudovda::lease_t *heartbeat_view_ = nullptr;  ///< Publication-locked failure-latch view; detached before operation-lane destruction.
      diagnostics_t diagnostics_;  ///< Independent failure observations retained through cleanup retries.
      bool episode_live_ = false;  ///< Admission exclusion lasts until actual recovery completion.
      bool episode_virtual_ = false;  ///< Cross-kind recovery cannot be transferred implicitly.
    };
  }  // namespace

  std::optional<bool> physical_source_available() {
    return checked_availability(display_device::WinApiLayer {});
  }

  std::shared_ptr<rtsp_stream::source_backend_t> make_backend(options_t options) {
    return std::make_shared<backend_t>(std::move(options), native_dependencies());
  }

#ifdef SUNSHINE_TESTS
  std::shared_ptr<rtsp_stream::source_backend_t> test_make_backend(options_t options, test_dependencies_t dependencies) {
    auto boundaries = dependencies_t {std::move(dependencies.make_api), std::move(dependencies.capture_names), std::move(dependencies.wake_native), std::move(dependencies.render_adapter), std::move(dependencies.make_system), std::move(dependencies.begin_owned)};
    boundaries.probe = std::move(dependencies.probe);
    return std::make_shared<backend_t>(std::move(options), std::move(boundaries));
  }

  std::optional<bool> test_physical_source_available(const display_device::WinApiLayerInterface &api) {
    return checked_availability(api);
  }

  diagnostics_t test_diagnostics(const rtsp_stream::source_backend_t &backend) {
    return dynamic_cast<const backend_t &>(backend).diagnostics();
  }
#endif
}  // namespace platf::source_backend

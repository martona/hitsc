#include "view_base.hpp"

#include "log.hpp"
#include "view_input.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace hitsc {
namespace {

std::string message_from_exception(std::exception_ptr exception)
{
    if (!exception) {
        return {};
    }
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "Unknown error";
    }
}

} // namespace

void ViewStateBase::set_exception(std::exception_ptr exception)
{
    std::lock_guard lock(control_mutex);
    if (!exception_) {
        exception_ = exception;
    }
}

std::exception_ptr ViewStateBase::take_exception()
{
    std::lock_guard lock(control_mutex);
    return exception_;
}

void ViewStateBase::clear_exception()
{
    std::lock_guard lock(control_mutex);
    exception_ = nullptr;
}

void ViewStateBase::set_force_close(std::function<void()> force_close)
{
    std::lock_guard lock(control_mutex);
    force_close_ = std::move(force_close);
}

std::function<void()> ViewStateBase::force_close_snapshot()
{
    std::lock_guard lock(control_mutex);
    return force_close_;
}

KvmNetworkWorker::KvmNetworkWorker(ViewStateBase& state, std::function<void()> cleanup)
    : state_(state)
    , cleanup_(std::move(cleanup))
{
}

void KvmNetworkWorker::stop()
{
    stop_requested_.store(true);

    if (std::function<void()> force_close = state_.force_close_snapshot()) {
        force_close();
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

bool KvmNetworkWorker::done() const
{
    return done_.load();
}

KvmViewBase::KvmViewBase(
    ViewStateBase& state,
    std::string host,
    std::string log_name,
    std::function<void()> network_cleanup)
    : state_(state)
    , network_(state_, std::move(network_cleanup))
    , host_(std::move(host))
    , log_name_(std::move(log_name))
{
}

TargetRect KvmViewBase::centered_target_rect(
    int window_width,
    int window_height,
    int frame_width,
    int frame_height)
{
    const float width_scale = static_cast<float>(window_width) / static_cast<float>(frame_width);
    const float height_scale = static_cast<float>(window_height) / static_cast<float>(frame_height);
    const float scale = std::min(width_scale, height_scale);
    TargetRect rect{};
    rect.w = std::floor(static_cast<float>(frame_width) * scale);
    rect.h = std::floor(static_cast<float>(frame_height) * scale);
    rect.x = std::floor((static_cast<float>(window_width) - rect.w) / 2.0f);
    rect.y = std::floor((static_cast<float>(window_height) - rect.h) / 2.0f);
    return rect;
}

void KvmViewBase::frame_presented(int width, int height)
{
    state_.view_status.frame_presented(width, height);
}

bool KvmViewBase::build_console_screen(const ViewRenderState& render_state, ConsoleScreen& screen) const
{
    if (session_ended_) {
        if (had_error_) {
            screen.severity = ConsoleSeverity::Error;
            screen.headline = "Connection failed";
            screen.detail = error_message_;
        } else {
            screen.severity = ConsoleSeverity::Info;
            screen.headline = "Disconnected";
            screen.detail = "The session ended.";
        }
        screen.hint = "Press R to reconnect      Esc to close";
        return true;
    }
    if (!render_state.connected) {
        screen.severity = ConsoleSeverity::Info;
        screen.headline = "Connecting to " + host_ + "...";
        screen.hint = "Esc to cancel";
        return true;
    }
    if (render_state.display_online.has_value() && !render_state.display_online.value()) {
        screen.severity = ConsoleSeverity::Info;
        screen.headline = "No video signal";
        screen.detail = "The host display may be off or asleep.";
        return true;
    }
    return false;
}

void KvmViewBase::do_retry()
{
    log_info() << log_name_ << " reconnect requested";
    network_.stop();
    network_started_ = false;
    state_.clear_exception();
    reset_for_reconnect();
    state_.view_status.kvm_connection(false);
    session_ended_ = false;
    had_error_ = false;
    error_message_.clear();
    start_network(network_);
    network_started_ = true;
}

// ---------------------------------------------------------------------------
// Qt-hosted mode. The Qt viewer host (run_viewer) drives these: start the
// network, poll for session end, feed input, and pull frames/console.
// ---------------------------------------------------------------------------

void KvmViewBase::hosted_start_network()
{
    // The Qt surface maps pointer coordinates against itself: centre the latest
    // frame within the surface's current size. (Persists across reconnect;
    // reset() does not touch the geometry source.)
    if (KvmInputController* controller = hosted_input_controller()) {
        controller->set_frame_geometry_source([this]() -> std::optional<FrameGeometry> {
            const std::optional<std::pair<int, int>> size = latest_frame_size();
            if (!size || hosted_surface_w_ <= 0 || hosted_surface_h_ <= 0) {
                return std::nullopt;
            }
            return FrameGeometry{
                size->first,
                size->second,
                centered_target_rect(hosted_surface_w_, hosted_surface_h_, size->first, size->second)};
        });
    }

    start_network(network_);
    network_started_ = true;
}

void KvmViewBase::hosted_set_surface_size(int width, int height)
{
    hosted_surface_w_ = width;
    hosted_surface_h_ = height;
}

void KvmViewBase::hosted_stop_network()
{
    if (network_started_) {
        network_.stop();
        network_started_ = false;
    }
}

void KvmViewBase::hosted_poll()
{
    if (network_.done() && !session_ended_) {
        session_ended_ = true;
        const std::exception_ptr exception = state_.take_exception();
        had_error_ = static_cast<bool>(exception);
        error_message_ = message_from_exception(exception);
        if (had_error_) {
            log_error() << log_name_ << " session ended with error: " << error_message_;
        } else {
            log_info() << log_name_ << " session ended";
        }
    }
}

void KvmViewBase::hosted_retry()
{
    do_retry();
}

bool KvmViewBase::hosted_connected() const
{
    return state_.view_status.render_state().connected;
}

std::string KvmViewBase::hosted_title() const
{
    return state_.view_status.title(host_);
}

std::optional<ConsoleScreen> KvmViewBase::hosted_console_screen() const
{
    const ViewRenderState render_state = state_.view_status.render_state();
    ConsoleScreen screen;
    if (build_console_screen(render_state, screen)) {
        return screen;
    }
    return std::nullopt;
}

void KvmViewBase::hosted_minimized()
{
    state_.view_status.minimize();
    on_minimized();
}

void KvmViewBase::hosted_restored()
{
    on_restored();
}

void KvmViewBase::hosted_focus_lost()
{
    on_focus_lost();
}

void KvmViewBase::hosted_close()
{
    on_close();
}

void KvmViewBase::feed_key(const KvmKeyEvent& key)
{
    if (KvmInputController* controller = hosted_input_controller()) {
        controller->feed_key(key);
    }
}

void KvmViewBase::feed_pointer_button(const KvmPointerButton& button)
{
    if (KvmInputController* controller = hosted_input_controller()) {
        controller->feed_pointer_button(button);
    }
}

void KvmViewBase::feed_pointer_motion(const KvmPointerMotion& motion)
{
    if (KvmInputController* controller = hosted_input_controller()) {
        controller->feed_pointer_motion(motion);
    }
}

void KvmViewBase::feed_pointer_wheel(const KvmPointerWheel& wheel)
{
    if (KvmInputController* controller = hosted_input_controller()) {
        controller->feed_pointer_wheel(wheel);
    }
}

} // namespace hitsc

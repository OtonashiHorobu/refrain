// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/permissions/admin.hpp>

#include <mutex>

namespace refrain::permissions {

namespace {

/// The published management surface. The built-in base provider owns exactly
/// one instance per process, and the host rebuilds it when the composition
/// changes, so a single slot with replace-on-publish semantics is enough; a
/// plain map would outlive the provider it points at.
class admin_registry {
  public:
    void publish(admin *instance) {
        const std::lock_guard lock{mutex_};
        instance_ = instance;
    }

    void retract(admin *instance) {
        const std::lock_guard lock{mutex_};
        if (instance_ == instance) {
            instance_ = nullptr;
        }
    }

    [[nodiscard]] auto active() const -> admin * {
        const std::lock_guard lock{mutex_};
        return instance_;
    }

  private:
    mutable std::mutex mutex_{};
    admin *instance_{nullptr};
};

[[nodiscard]] auto registry() -> admin_registry & {
    static admin_registry instance{};
    return instance;
}

} // namespace

void publish_admin(admin *instance) { registry().publish(instance); }

void retract_admin(admin *instance) { registry().retract(instance); }

auto active_admin() -> admin * { return registry().active(); }

} // namespace refrain::permissions

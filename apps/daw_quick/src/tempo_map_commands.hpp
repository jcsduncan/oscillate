#pragma once

#include "oscillate/application/project_session.hpp"

#include <QString>

#include <functional>

namespace oscillate::quick {

// UI-thread owner for tempo-map editing and paging. The bridge remains the
// revisioned read-model publisher and the only graph-submission boundary.
class TempoMapCommands final {
public:
  using Session = std::function<application::ProjectSession*()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Commit = std::function<void()>;
  using Publish = std::function<void()>;

  TempoMapCommands(Session session, CanEdit can_edit, Report report, Commit commit,
                   Publish publish);

  [[nodiscard]] int page() const noexcept { return page_; }
  void reset_page() noexcept { page_ = 0; }
  void clamp_page(int page_count) noexcept;
  void set_page(int page);
  void set_initial_bpm(double bpm);
  void set_initial_meter(int numerator, int denominator);
  void add_event(int bar, int beat, double bpm, int shape_index);
  void update_event(const QString& tick_text, double bpm, int shape_index);
  void remove_event(const QString& tick_text);

private:
  Session session_;
  CanEdit can_edit_;
  Report report_;
  Commit commit_;
  Publish publish_;
  int page_ = 0;
};

} // namespace oscillate::quick

#pragma once

#include <QtGlobal>

namespace hitsc {

class ParentLivenessMonitor {
public:
    ParentLivenessMonitor() = default;
    ParentLivenessMonitor(const ParentLivenessMonitor&) = delete;
    ParentLivenessMonitor& operator=(const ParentLivenessMonitor&) = delete;

    void start(qint64 parent_process_id);
};

} // namespace hitsc

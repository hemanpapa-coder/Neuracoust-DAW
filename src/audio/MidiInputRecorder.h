#pragma once

#include "project/EditOperations.h"
#include <memory>
#include <string>
#include <vector>

namespace neuracoust::daw {

struct MidiInputSourceInfo {
    std::string id;
    std::string name;
    bool connected = false;
};

struct MidiInputRecordingStatus {
    bool recording = false;
    double durationSeconds = 0.0;
    size_t eventCount = 0;
    size_t sourceCount = 0;
    std::string message;
};

class MidiInputRecorder {
public:
    MidiInputRecorder();
    ~MidiInputRecorder();

    MidiInputRecorder(const MidiInputRecorder&) = delete;
    MidiInputRecorder& operator=(const MidiInputRecorder&) = delete;

    std::vector<MidiInputSourceInfo> availableInputs() const;
    std::vector<MidiInputSourceInfo> availableOutputs() const;
    bool start(const std::string& preferredInputId = {});
    bool stop(std::vector<RecordedMidiEvent>& events, std::string& error);
    std::vector<RecordedMidiEvent> consumePendingEvents();
    MidiInputRecordingStatus status() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neuracoust::daw
